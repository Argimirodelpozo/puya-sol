#include "builder/AWSTBuilder.h"
#include "awst/Termination.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-eb/FunctionPointerBuilder.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

#include <set>
#include <queue>

namespace puyasol::builder
{

using awst::statementAlwaysTerminates;
using awst::blockAlwaysTerminates;

/// Apply dead code elimination to all methods in a contract.
static void eliminateDeadCode(awst::Contract& _contract)
{
	auto dce = [](awst::ContractMethod& m) {
		if (m.body) awst::removeDeadCode(m.body->body);
	};
	dce(_contract.approvalProgram);
	dce(_contract.clearProgram);
	for (auto& m: _contract.methods)
		dce(m);
}

std::vector<std::shared_ptr<awst::RootNode>> AWSTBuilder::build(
	solidity::frontend::CompilerStack& _compiler,
	std::string const& _sourceFile,
	uint64_t _opupBudget,
	std::map<std::string, uint64_t> const& _ensureBudget,
	bool _viaYulBehavior
)
{
	m_storageMapper = std::make_unique<StorageMapper>(m_typeMapper);
	m_libraryFunctionIds.clear();
	std::vector<std::shared_ptr<awst::RootNode>> roots;

	// First pass: register all library and free function IDs (before translating any bodies)
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& sourceUnit = _compiler.ast(sourceName);

		for (auto const& node: sourceUnit.nodes())
		{
			// Register library functions
			auto const* contract = dynamic_cast<solidity::frontend::ContractDefinition const*>(
				node.get()
			);

			if (contract && contract->isLibrary())
			{
				std::string libraryName = contract->name();

				// Detect overloaded function names and name+paramcount collisions
				std::unordered_map<std::string, int> nameCount;
				std::unordered_map<std::string, int> nameParamCount;
				for (auto const* func: contract->definedFunctions())
				{
					if (!func->isImplemented())
						continue;
					std::string baseName = libraryName + "." + func->name();
					nameCount[baseName]++;
					nameParamCount[baseName + "(" + std::to_string(func->parameters().size()) + ")"]++;
				}

				// Track sequence numbers for same-name-same-paramcount overloads
				std::unordered_map<std::string, int> nameParamSeq;

				for (auto const* func: contract->definedFunctions())
				{
					if (!func->isImplemented())
						continue;

					// Skip functions with non-internal function-type parameters
					{
						bool hasNonInternalFnParam = false;
						for (auto const& p: func->parameters())
						{
							if (auto const* ft = dynamic_cast<solidity::frontend::FunctionType const*>(p->type()))
								if (ft->kind() != solidity::frontend::FunctionType::Kind::Internal)
								{ hasNonInternalFnParam = true; break; }
						}
						if (hasNonInternalFnParam)
							continue;
					}

					std::string baseName = libraryName + "." + func->name();
					std::string qualifiedName = baseName;
					std::string subroutineId = _sourceFile + "." + baseName;
					// Disambiguate overloaded functions by parameter count
					if (nameCount[baseName] > 1)
					{
						std::string paramKey = baseName + "(" + std::to_string(func->parameters().size()) + ")";
						qualifiedName = paramKey;
						subroutineId = _sourceFile + "." + paramKey;
						// Further disambiguate if same name AND same param count
						if (nameParamCount[paramKey] > 1)
						{
							int seq = nameParamSeq[paramKey]++;
							qualifiedName += "_" + std::to_string(seq);
							subroutineId += "_" + std::to_string(seq);
						}
					}
					m_libraryFunctionIds[qualifiedName] = subroutineId;
					// Also store by AST ID for precise overload resolution
					m_freeFunctionById[func->id()] = subroutineId;
					Logger::instance().debug("[REG] lib func id=" + std::to_string(func->id()) + " name=" + qualifiedName + " => " + subroutineId);
				}
				continue;
			}

			// Register free (file-level) functions
			auto const* func = dynamic_cast<solidity::frontend::FunctionDefinition const*>(
				node.get()
			);
			if (func && func->isImplemented() && func->isFree())
			{
				std::string qualifiedName = func->name();
				std::string subroutineId = _sourceFile + "." + qualifiedName;
				// Disambiguate free functions with the same name (e.g. UD60x18.powu vs SD59x18.powu)
				// by appending the AST ID when the name is already registered by a different function
				auto existingIt = m_freeFunctionById.find(func->id());
				if (existingIt == m_freeFunctionById.end())
				{
					// Check if another function already uses this name
					for (auto const& [otherId, otherSid]: m_freeFunctionById)
					{
						if (otherSid == subroutineId && otherId != func->id())
						{
							subroutineId += "_" + std::to_string(func->id());
							break;
						}
					}
				}
				m_libraryFunctionIds[qualifiedName] = subroutineId;
				// Also store by AST ID for operator overload resolution
				m_freeFunctionById[func->id()] = subroutineId;
				Logger::instance().debug("[REG] free func id=" + std::to_string(func->id()) + " name=" + qualifiedName + " => " + subroutineId);
			}
		}
	}

	// Pre-set the function pointer dispatch cref to the first deployable
	// contract so that library subroutines can construct SubroutineIDs
	// for dispatch calls. Library bodies are translated before contracts,
	// but the dispatch subroutines live in the contract scope.
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& su = _compiler.ast(sourceName);
		for (auto const& node: su.nodes())
		{
			auto const* c = dynamic_cast<solidity::frontend::ContractDefinition const*>(node.get());
			if (c && !c->isLibrary() && !c->abstract() && !c->isInterface())
			{
				eb::FunctionPointerBuilder::setCurrentCref(_sourceFile + "." + c->name());
				goto crefSet;
			}
		}
	}
	crefSet:

	// Second pass: translate library functions as Subroutine root nodes
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& sourceUnit = _compiler.ast(sourceName);

		for (auto const& node: sourceUnit.nodes())
		{
			auto const* contract = dynamic_cast<solidity::frontend::ContractDefinition const*>(
				node.get()
			);

			if (!contract || !contract->isLibrary())
				continue;

			std::string libraryName = contract->name();
			Logger::instance().info("Translating library: " + libraryName);

			for (auto const* func: contract->definedFunctions())
			{
				if (!func->isImplemented())
					continue;

				std::string qualifiedName = libraryName + "." + func->name();
				std::string subroutineId;
				// Prefer AST ID lookup for precise overload resolution
				auto byId = m_freeFunctionById.find(func->id());
				if (byId != m_freeFunctionById.end())
				{
					subroutineId = byId->second;
				}
				else
				{
					auto it = m_libraryFunctionIds.find(qualifiedName);
					if (it != m_libraryFunctionIds.end())
					{
						subroutineId = it->second;
					}
					else
					{
						std::string overloadName = qualifiedName + "(" + std::to_string(func->parameters().size()) + ")";
						auto it2 = m_libraryFunctionIds.find(overloadName);
						if (it2 != m_libraryFunctionIds.end())
						{
							qualifiedName = overloadName;
							subroutineId = it2->second;
						}
						else
							subroutineId = _sourceFile + "." + qualifiedName;
					}
				}

				// Skip library functions with non-internal function-type
				// parameters (external/delegate call pointers can't be
				// represented on AVM). Internal function pointers work via
				// our dispatch table mechanism.
				{
					bool hasNonInternalFnParam = false;
					for (auto const& p: func->parameters())
					{
						if (auto const* ft = dynamic_cast<solidity::frontend::FunctionType const*>(p->type()))
						{
							if (ft->kind() != solidity::frontend::FunctionType::Kind::Internal)
							{
								hasNonInternalFnParam = true;
								break;
							}
						}
					}
					if (hasNonInternalFnParam)
					{
						Logger::instance().debug("Skipping library function with non-internal function-type param: " + qualifiedName);
						continue;
					}
				}

				Logger::instance().debug("Translating library function: " + qualifiedName);

				auto sub = std::make_shared<awst::Subroutine>();
				sub->inlineOpt = false; // Prevent puya from inlining large subroutines

				awst::SourceLocation loc;
				loc.file = _sourceFile;
				loc.line = func->location().start >= 0 ? func->location().start : 0;
				loc.endLine = func->location().end >= 0 ? func->location().end : 0;

				sub->sourceLocation = loc;
				sub->id = subroutineId;
				sub->name = qualifiedName;

				// Documentation
				if (func->documentation())
					sub->documentation.description = *func->documentation()->text();

				// Parameters — mapping storage refs become bytes (runtime key prefix)
				std::set<size_t> mappingStorageParams;
				for (size_t pi = 0; pi < func->parameters().size(); ++pi)
				{
					auto const& param = func->parameters()[pi];
					awst::SubroutineArgument arg;
					arg.name = param->name();
					// Assign synthetic name for unnamed parameters
					if (arg.name.empty())
						arg.name = "_param" + std::to_string(pi);
					arg.sourceLocation.file = _sourceFile;
					arg.sourceLocation.line = param->location().start >= 0 ? param->location().start : 0;
					arg.sourceLocation.endLine = param->location().end >= 0 ? param->location().end : 0;

					// Mapping storage refs: the callee receives the box key
					// PREFIX as bytes so that m[k] → box_get(prefix+sha256(k))
					// uses the caller's storage variable name, not the param name.
					if (param->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
						&& dynamic_cast<solidity::frontend::MappingType const*>(param->type()))
					{
						arg.wtype = awst::WType::bytesType();
						mappingStorageParams.insert(pi);
					}
					else
						arg.wtype = m_typeMapper.map(param->type());
					sub->args.push_back(std::move(arg));
				}

				// Return type — include `storage` params as extra returns so callers
				// can capture modified values for box write-back. Puya backend
				// already threads mutable args; we match the return type to avoid
				// the extra returns being discarded.
				// Detect storage reference params for return type augmentation.
				// Only augment non-private functions (internal/public) — private
				// library functions like _add/_remove are called internally and
				// puya handles their mutable arg threading automatically.
				std::vector<size_t> storageParamIndices;
				bool isMutating = func->stateMutability() != solidity::frontend::StateMutability::View
					&& func->stateMutability() != solidity::frontend::StateMutability::Pure;
				bool isPrivate = func->visibility() == solidity::frontend::Visibility::Private;
				if (isMutating && !isPrivate)
				{
					for (size_t pi = 0; pi < func->parameters().size(); ++pi)
					{
						auto refLoc = func->parameters()[pi]->referenceLocation();
						Logger::instance().debug("[STORAGE-AUG] " + qualifiedName
							+ " param[" + std::to_string(pi) + "]=" + func->parameters()[pi]->name()
							+ " refLoc=" + std::to_string(static_cast<int>(refLoc)));
						// Skip mapping storage-ref params — they use shared box keys
						// so no write-back is needed. Only struct-style storage refs
						// need return augmentation.
						if (refLoc == solidity::frontend::VariableDeclaration::Location::Storage
							&& !mappingStorageParams.count(pi))
							storageParamIndices.push_back(pi);
					}
				}

				// Freeze augmented storage params FIRST so return type and body
				// both use the same frozen type consistently.
				// Return type — augment with storage params.
				auto const& returnParams = func->returnParameters();
				{
					std::vector<awst::WType const*> types;
					for (auto const& rp: returnParams)
						types.push_back(m_typeMapper.map(rp->type()));
					for (size_t idx: storageParamIndices)
						types.push_back(sub->args[idx].wtype);

					if (types.empty())
						sub->returnType = awst::WType::voidType();
					else if (types.size() == 1)
						sub->returnType = types[0];
					else
						sub->returnType = new awst::WTuple(std::move(types));
				}

				// Pure
				sub->pure = func->stateMutability() == solidity::frontend::StateMutability::Pure;

				// Translate body
				eb::BuilderContext exprBuilder(
					m_typeMapper, *m_storageMapper, _sourceFile, libraryName, m_libraryFunctionIds,
					{}, m_freeFunctionById
				);

				// Register mapping-storage-ref params so SolIndexAccess can
				// use them as dynamic box key prefixes at runtime.
				for (size_t idx: mappingStorageParams)
				{
					auto const& param = func->parameters()[idx];
					exprBuilder.mappingKeyParams[param->id()] = param->name();
				}

				sol_ast::StatementContext stmtCtx{
					&exprBuilder, &m_typeMapper, _sourceFile,
					[&](solidity::frontend::Expression const& e) { return exprBuilder.build(e); },
					[&](solidity::frontend::Statement const& s) { return sol_ast::buildStatement(stmtCtx, exprBuilder, s); },
					[&](solidity::frontend::Block const& b) { return sol_ast::buildBlock(stmtCtx, exprBuilder, b); },
					[&]() { return exprBuilder.takePrePending(); },
					[&]() { return exprBuilder.takePending(); },
					{}, nullptr, {}, nullptr, nullptr, nullptr,
				};

			{
				std::vector<std::pair<std::string, awst::WType const*>> paramContext;
				std::map<std::string, unsigned> bitWidths;
				for (size_t pi = 0; pi < func->parameters().size(); ++pi)
				{
					auto const& param = func->parameters()[pi];
					std::string pname = param->name();
					if (pname.empty())
						pname = "_param" + std::to_string(pi);
					auto* ptype = mappingStorageParams.count(pi) ? awst::WType::bytesType() : m_typeMapper.map(param->type());
					paramContext.emplace_back(pname, ptype);
					if (auto const* solType = param->annotation().type)
					{
						auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(solType);
						if (!intType)
							if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
								intType = dynamic_cast<solidity::frontend::IntegerType const*>(&udvt->underlyingType());
						if (intType && intType->numBits() < 64)
							bitWidths[pname] = intType->numBits();
					}
				}
				for (auto const& rp: func->returnParameters())
				{
					auto const* solType = rp->annotation().type;
					auto const* intType = solType ? dynamic_cast<solidity::frontend::IntegerType const*>(solType) : nullptr;
					if (!intType && solType)
						if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
							intType = dynamic_cast<solidity::frontend::IntegerType const*>(&udvt->underlyingType());
					if (intType && intType->numBits() < 64)
						bitWidths[rp->name()] = intType->numBits();
				}
				stmtCtx.functionParams = paramContext;
				stmtCtx.returnType = sub->returnType;
				stmtCtx.functionParamBitWidths = bitWidths;
			}

				// Register named return variable names so inner scoping detects shadowing
				for (auto const& rp: returnParams)
					if (!rp->name().empty())
						exprBuilder.resolveVarName(rp->name(), rp->id());

				// Register mapping-storage-ref return params as mapping-key-params
				// too: `function f() returns (mapping(K=>V) storage r)` — `r` is a
				// local pointer to a mapping; r[k] resolves to box access prefixed
				// by `r`'s runtime bytes value (the holder name).
				for (auto const& rp: returnParams)
				{
					if (rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
						&& dynamic_cast<solidity::frontend::MappingType const*>(rp->type())
						&& !rp->name().empty())
					{
						exprBuilder.mappingKeyParams[rp->id()] = rp->name();
					}
				}

			sub->body = sol_ast::buildBlock(stmtCtx, exprBuilder, func->body());

				// Insert zero-initialization for named return variables
				// Solidity implicitly initializes named returns to their zero values
				{
					std::vector<std::shared_ptr<awst::Statement>> inits;
					for (auto const& rp: returnParams)
					{
						if (rp->name().empty())
							continue;
						auto* rpType = m_typeMapper.map(rp->type());

						auto target = awst::makeVarExpression(rp->name(), rpType, loc);

						std::shared_ptr<awst::Expression> zeroVal;
						if (rpType == awst::WType::boolType())
						{
							zeroVal = awst::makeBoolConstant(false, loc, rpType);
						}
						else if (rpType == awst::WType::uint64Type()
							|| rpType == awst::WType::biguintType())
						{
							auto def = awst::makeIntegerConstant("0", loc, rpType);
							zeroVal = std::move(def);
						}
						else if (rpType && rpType->kind() == awst::WTypeKind::Bytes)
						{
							// For fixed-size bytes types (bytes1..bytes32), produce N zero bytes
							std::vector<uint8_t> bytes;
							auto const* bytesType = dynamic_cast<awst::BytesWType const*>(rpType);
							if (bytesType && bytesType->length().has_value())
								bytes.assign(bytesType->length().value(), 0);
							zeroVal = awst::makeBytesConstant(
								std::move(bytes), loc, awst::BytesEncoding::Base16, rpType);
						}
						else
						{
							// Complex types (structs, arrays, etc.) — use makeDefaultValue
							// These need initialization because Solidity may assign individual
							// fields (e.g., vk.alfa1 = ...) which reads other fields from the
							// uninitialized variable via copy-on-write NewStruct pattern
							zeroVal = StorageMapper::makeDefaultValue(rpType, loc);
						}

						auto assign = awst::makeAssignmentStatement(std::move(target), std::move(zeroVal), loc);
						inits.push_back(std::move(assign));
					}
					if (!inits.empty())
					{
						sub->body->body.insert(
							sub->body->body.begin(),
							std::make_move_iterator(inits.begin()),
							std::make_move_iterator(inits.end())
						);
					}
				}

				// Augment return statements: append storage param values.
				// Only for non-private (internal/public) library functions.
				// Puya already threads mutable args — we match the declared type.
				if (!storageParamIndices.empty())
				{
					std::function<void(awst::Block&)> augmentReturns;
					augmentReturns = [&](awst::Block& block) {
						for (auto& stmt: block.body)
						{
							if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
							{
								auto tuple = std::make_shared<awst::TupleExpression>();
								tuple->sourceLocation = ret->sourceLocation;
								tuple->wtype = sub->returnType;
								if (ret->value)
									tuple->items.push_back(ret->value);
								for (size_t idx: storageParamIndices)
								{
									auto pv = awst::makeVarExpression(sub->args[idx].name, sub->args[idx].wtype, ret->sourceLocation);
									tuple->items.push_back(std::move(pv));
								}
								ret->value = std::move(tuple);
							}
							if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get()))
							{
								if (ifElse->ifBranch) augmentReturns(*ifElse->ifBranch);
								if (ifElse->elseBranch) augmentReturns(*ifElse->elseBranch);
							}
						}
					};
					augmentReturns(*sub->body);
				}

				// Handle assembly-only functions with known semantics
				if (sub->body->body.empty() && func->name() == "efficientKeccak256"
					&& func->parameters().size() == 2)
				{
					// efficientKeccak256(a, b) → return keccak256(concat(a, b))
					auto varA = awst::makeVarExpression(func->parameters()[0]->name(), m_typeMapper.map(func->parameters()[0]->type()), loc);

					auto varB = awst::makeVarExpression(func->parameters()[1]->name(), m_typeMapper.map(func->parameters()[1]->type()), loc);

					auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), loc);
					concat->stackArgs.push_back(std::move(varA));
					concat->stackArgs.push_back(std::move(varB));

					auto hash = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), loc);
					hash->stackArgs.push_back(std::move(concat));

					// Cast bytes → bytes[32] to match return type
					auto cast = awst::makeReinterpretCast(std::move(hash), sub->returnType, loc);

					auto ret = awst::makeReturnStatement(std::move(cast), loc);
					sub->body->body.push_back(std::move(ret));
				}

				// Synthesize implicit return for named return parameters
				// Also handle library functions with storage params that augment the return type
				if (!blockAlwaysTerminates(*sub->body)
					&& (!returnParams.empty() || !storageParamIndices.empty()))
				{
					bool hasNamedReturns = false;
					for (auto const& rp: returnParams)
						if (!rp->name().empty())
							hasNamedReturns = true;

					if (!hasNamedReturns)
					{
						// No named returns — append default return with zero value
						auto defReturn = awst::makeReturnStatement(StorageMapper::makeDefaultValue(sub->returnType, loc), loc);
						sub->body->body.push_back(std::move(defReturn));
					}
					else
					{
					auto implicitReturn = awst::makeReturnStatement(nullptr, loc);

					if (returnParams.size() == 1)
					{
						auto var = awst::makeVarExpression(returnParams[0]->name(), m_typeMapper.map(returnParams[0]->type()), loc);
						implicitReturn->value = std::move(var);
					}
					else
					{
						auto tuple = std::make_shared<awst::TupleExpression>();
						tuple->sourceLocation = loc;
						std::vector<awst::WType const*> types;
						for (auto const& rp: returnParams)
						{
							auto var = awst::makeVarExpression(rp->name(), m_typeMapper.map(rp->type()), loc);
							types.push_back(var->wtype);
							tuple->items.push_back(std::move(var));
						}
						tuple->wtype = sub->returnType;
						implicitReturn->value = std::move(tuple);
					}

					sub->body->body.push_back(std::move(implicitReturn));
					}
				}

				roots.push_back(std::move(sub));
			}
		}
	}

	// Translate free (file-level) functions as Subroutine root nodes
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& sourceUnit = _compiler.ast(sourceName);

		for (auto const& node: sourceUnit.nodes())
		{
			auto const* func = dynamic_cast<solidity::frontend::FunctionDefinition const*>(
				node.get()
			);
			if (!func || !func->isImplemented() || !func->isFree())
				continue;

			std::string qualifiedName = func->name();
			// Prefer AST ID lookup for precise resolution (avoids name collisions
			// between same-named free functions, e.g. UD60x18.powu vs SD59x18.powu)
			std::string subroutineId;
			auto byId = m_freeFunctionById.find(func->id());
			if (byId != m_freeFunctionById.end())
				subroutineId = byId->second;
			else
			{
				auto it = m_libraryFunctionIds.find(qualifiedName);
				subroutineId = (it != m_libraryFunctionIds.end())
					? it->second
					: _sourceFile + "." + qualifiedName;
			}

			Logger::instance().debug("Translating free function: " + qualifiedName);

			auto sub = std::make_shared<awst::Subroutine>();
			sub->inlineOpt = false; // Prevent puya from inlining large subroutines
			awst::SourceLocation loc;
			loc.file = _sourceFile;
			loc.line = func->location().start >= 0 ? func->location().start : 0;
			loc.endLine = func->location().end >= 0 ? func->location().end : 0;

			sub->sourceLocation = loc;
			sub->id = subroutineId;
			sub->name = qualifiedName;

			if (func->documentation())
				sub->documentation.description = *func->documentation()->text();

			for (size_t pi = 0; pi < func->parameters().size(); ++pi)
			{
				auto const& param = func->parameters()[pi];
				awst::SubroutineArgument arg;
				arg.name = param->name();
				if (arg.name.empty())
					arg.name = "_param" + std::to_string(pi);
				arg.sourceLocation.file = _sourceFile;
				arg.sourceLocation.line = param->location().start >= 0 ? param->location().start : 0;
				arg.sourceLocation.endLine = param->location().end >= 0 ? param->location().end : 0;
				arg.wtype = m_typeMapper.map(param->type());
				sub->args.push_back(std::move(arg));
			}

			// Free functions taking non-mapping storage references need their
			// modified args threaded back to callers. Same mechanism as library
			// functions — augment the return type with the storage-arg types
			// and callers write back via SolInternalCall.
			std::vector<size_t> storageParamIndices;
			bool freeIsMutating = func->stateMutability() != solidity::frontend::StateMutability::View
				&& func->stateMutability() != solidity::frontend::StateMutability::Pure;
			if (freeIsMutating)
			{
				for (size_t pi = 0; pi < func->parameters().size(); ++pi)
				{
					auto const& param = func->parameters()[pi];
					if (param->referenceLocation() != solidity::frontend::VariableDeclaration::Location::Storage)
						continue;
					if (dynamic_cast<solidity::frontend::MappingType const*>(param->type()))
						continue;
					storageParamIndices.push_back(pi);
				}
			}

			auto const& returnParams = func->returnParameters();
			{
				std::vector<awst::WType const*> types;
				for (auto const& rp: returnParams)
					types.push_back(m_typeMapper.map(rp->type()));
				for (size_t idx: storageParamIndices)
					types.push_back(sub->args[idx].wtype);

				if (types.empty())
					sub->returnType = awst::WType::voidType();
				else if (types.size() == 1)
					sub->returnType = types[0];
				else
					sub->returnType = new awst::WTuple(std::move(types));
			}

			sub->pure = func->stateMutability() == solidity::frontend::StateMutability::Pure;

			eb::BuilderContext exprBuilder(
				m_typeMapper, *m_storageMapper, _sourceFile, "", m_libraryFunctionIds,
				{}, m_freeFunctionById
			);
			sol_ast::StatementContext stmtCtx{
				&exprBuilder, &m_typeMapper, _sourceFile,
				[&](solidity::frontend::Expression const& e) { return exprBuilder.build(e); },
				[&](solidity::frontend::Statement const& s) { return sol_ast::buildStatement(stmtCtx, exprBuilder, s); },
				[&](solidity::frontend::Block const& b) { return sol_ast::buildBlock(stmtCtx, exprBuilder, b); },
				[&]() { return exprBuilder.takePrePending(); },
				[&]() { return exprBuilder.takePending(); },
				{}, nullptr, {}, nullptr, nullptr, nullptr,
			};

			{
				std::vector<std::pair<std::string, awst::WType const*>> paramContext;
				std::map<std::string, unsigned> bitWidths;
				for (size_t pi = 0; pi < func->parameters().size(); ++pi)
				{
					auto const& param = func->parameters()[pi];
					std::string pname = param->name();
					if (pname.empty())
						pname = "_param" + std::to_string(pi);
					paramContext.emplace_back(pname, m_typeMapper.map(param->type()));
					if (auto const* solType = param->annotation().type)
					{
						auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(solType);
						if (!intType)
							if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
								intType = dynamic_cast<solidity::frontend::IntegerType const*>(&udvt->underlyingType());
						if (intType && intType->numBits() < 64)
							bitWidths[pname] = intType->numBits();
					}
				}
				for (auto const& rp: func->returnParameters())
				{
					auto const* solType = rp->annotation().type;
					auto const* intType = solType ? dynamic_cast<solidity::frontend::IntegerType const*>(solType) : nullptr;
					if (!intType && solType)
						if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
							intType = dynamic_cast<solidity::frontend::IntegerType const*>(&udvt->underlyingType());
					if (intType && intType->numBits() < 64)
						bitWidths[rp->name()] = intType->numBits();
				}
				stmtCtx.functionParams = paramContext;
				stmtCtx.returnType = sub->returnType;
				stmtCtx.functionParamBitWidths = bitWidths;
			}

			// Register named return variable names so inner scoping detects shadowing
			for (auto const& rp: returnParams)
				if (!rp->name().empty())
					exprBuilder.resolveVarName(rp->name(), rp->id());

			sub->body = sol_ast::buildBlock(stmtCtx, exprBuilder, func->body());

			// Insert zero-initialization for named return variables
			{
				std::vector<std::shared_ptr<awst::Statement>> inits;
				for (auto const& rp: returnParams)
				{
					if (rp->name().empty())
						continue;
					auto* rpType = m_typeMapper.map(rp->type());

					auto target = awst::makeVarExpression(rp->name(), rpType, loc);

					std::shared_ptr<awst::Expression> zeroVal;
					if (rpType == awst::WType::boolType())
					{
						zeroVal = awst::makeBoolConstant(false, loc, rpType);
					}
					else if (rpType == awst::WType::uint64Type()
						|| rpType == awst::WType::biguintType())
					{
						auto def = awst::makeIntegerConstant("0", loc, rpType);
						zeroVal = std::move(def);
					}
					else
					{
						// Complex types (structs, arrays, etc.) — use makeDefaultValue
						zeroVal = StorageMapper::makeDefaultValue(rpType, loc);
					}

					auto assign = awst::makeAssignmentStatement(std::move(target), std::move(zeroVal), loc);
					inits.push_back(std::move(assign));
				}
				if (!inits.empty())
				{
					sub->body->body.insert(
						sub->body->body.begin(),
						std::make_move_iterator(inits.begin()),
						std::make_move_iterator(inits.end())
					);
				}
			}

			// Augment return statements: append storage param values so
			// callers can write the modified struct back to their storage.
			if (!storageParamIndices.empty())
			{
				std::function<void(awst::Block&)> augmentReturns;
				augmentReturns = [&](awst::Block& block) {
					for (auto& stmt: block.body)
					{
						if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
						{
							auto tuple = std::make_shared<awst::TupleExpression>();
							tuple->sourceLocation = ret->sourceLocation;
							tuple->wtype = sub->returnType;
							if (ret->value)
								tuple->items.push_back(ret->value);
							for (size_t idx: storageParamIndices)
							{
								auto pv = awst::makeVarExpression(sub->args[idx].name, sub->args[idx].wtype, ret->sourceLocation);
								tuple->items.push_back(std::move(pv));
							}
							ret->value = std::move(tuple);
						}
						if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get()))
						{
							if (ifElse->ifBranch) augmentReturns(*ifElse->ifBranch);
							if (ifElse->elseBranch) augmentReturns(*ifElse->elseBranch);
						}
					}
				};
				augmentReturns(*sub->body);
			}

			// Synthesize implicit return for named return parameters OR
			// for functions with only storage-param augmentation.
			if (!blockAlwaysTerminates(*sub->body)
				&& (!returnParams.empty() || !storageParamIndices.empty()))
			{
				bool hasNamedReturns = false;
				for (auto const& rp: returnParams)
					if (!rp->name().empty())
						hasNamedReturns = true;

				if (!hasNamedReturns && returnParams.empty() && !storageParamIndices.empty())
				{
					// Void return with storage augmentation — append trailing
					// return of (storage args...) so all paths terminate.
					auto implicitReturn = awst::makeReturnStatement(nullptr, loc);
					if (storageParamIndices.size() == 1)
					{
						size_t idx = storageParamIndices[0];
						implicitReturn->value = awst::makeVarExpression(sub->args[idx].name, sub->args[idx].wtype, loc);
					}
					else
					{
						auto tuple = std::make_shared<awst::TupleExpression>();
						tuple->sourceLocation = loc;
						tuple->wtype = sub->returnType;
						for (size_t idx: storageParamIndices)
							tuple->items.push_back(awst::makeVarExpression(sub->args[idx].name, sub->args[idx].wtype, loc));
						implicitReturn->value = std::move(tuple);
					}
					sub->body->body.push_back(std::move(implicitReturn));
				}
				else if (hasNamedReturns)
				{
					auto implicitReturn = awst::makeReturnStatement(nullptr, loc);

					if (returnParams.size() == 1)
					{
						auto var = awst::makeVarExpression(returnParams[0]->name(), m_typeMapper.map(returnParams[0]->type()), loc);
						implicitReturn->value = std::move(var);
					}
					else
					{
						auto tuple = std::make_shared<awst::TupleExpression>();
						tuple->sourceLocation = loc;
						std::vector<awst::WType const*> types;
						for (auto const& rp: returnParams)
						{
							auto var = awst::makeVarExpression(rp->name(), m_typeMapper.map(rp->type()), loc);
							types.push_back(var->wtype);
							tuple->items.push_back(std::move(var));
						}
						tuple->wtype = sub->returnType;
						implicitReturn->value = std::move(tuple);
					}

					sub->body->body.push_back(std::move(implicitReturn));
				}
				else
				{
					// No named returns — append default return with zero value
					auto defReturn = awst::makeReturnStatement(StorageMapper::makeDefaultValue(sub->returnType, loc), loc);
					sub->body->body.push_back(std::move(defReturn));
				}
			}

			roots.push_back(std::move(sub));
		}
	}

	// Translate contracts
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& sourceUnit = _compiler.ast(sourceName);

		for (auto const& node: sourceUnit.nodes())
		{
			auto const* contract = dynamic_cast<solidity::frontend::ContractDefinition const*>(
				node.get()
			);

			if (!contract)
				continue;

			// Skip interfaces, abstract contracts, and libraries (already handled)
			if (contract->isInterface())
			{
				Logger::instance().debug("Skipping interface: " + contract->name());
				continue;
			}

			if (contract->abstract())
			{
				Logger::instance().debug("Skipping abstract contract: " + contract->name());
				continue;
			}

			if (contract->isLibrary())
				continue;

			Logger::instance().info("Translating contract: " + contract->name());

			ContractBuilder translator(
				m_typeMapper, *m_storageMapper, _sourceFile, m_libraryFunctionIds,
				_opupBudget, m_freeFunctionById, _ensureBudget, _viaYulBehavior
			);
			auto awstContract = translator.build(*contract);

			// Collect dispatch subroutines as root nodes so library
			// subroutines can resolve them via SubroutineID.
			for (auto& sub : translator.takeDispatchSubroutines())
				roots.push_back(std::move(sub));

			// Only emit deployable contracts (those with public/external methods
			// or a constructor). Non-deployable contracts (e.g., ErrorReporter
			// with only internal functions) are translated for MRO/inheritance
			// resolution but not emitted to AWST.
			bool hasPublicMethod = false;
			for (auto const& method: awstContract->methods)
			{
				if (method.arc4MethodConfig.has_value())
				{
					hasPublicMethod = true;
					break;
				}
			}
			// Constructor-only contracts have no routable methods, but puya's
			// ARC4 router requires at least one. Add a dummy no-op method so
			// the contract is deployable and the constructor can run at create time.
			if (!hasPublicMethod && !contract->abstract())
			{
				awst::ContractMethod dummy;
				dummy.sourceLocation = awstContract->sourceLocation;
				dummy.cref = awstContract->id;
				dummy.memberName = "__dummy";
				dummy.returnType = awst::WType::boolType();

				auto body = std::make_shared<awst::Block>();
				body->sourceLocation = dummy.sourceLocation;
				auto ret = awst::makeReturnStatement(awst::makeBoolConstant(true, dummy.sourceLocation), dummy.sourceLocation);
				body->body.push_back(ret);
				dummy.body = body;

				awst::ARC4BareMethodConfig config;
				config.sourceLocation = dummy.sourceLocation;
				config.allowedCompletionTypes = {0}; // NoOp
				config.create = 3; // Disallow
				dummy.arc4MethodConfig = config;

				awstContract->methods.push_back(std::move(dummy));
				hasPublicMethod = true;
			}
			if (hasPublicMethod)
			{
				eliminateDeadCode(*awstContract);
				roots.push_back(std::move(awstContract));
			}
			else
				Logger::instance().debug("Skipping non-deployable contract: " + contract->name());
		}
	}

	// Dead-code elimination: only keep subroutines reachable from contract methods
	// Collect SubroutineID references from an expression tree
	std::function<void(awst::Expression const&, std::set<std::string>&)> collectRefs;
	collectRefs = [&](awst::Expression const& expr, std::set<std::string>& refs) {
		if (auto const* call = dynamic_cast<awst::SubroutineCallExpression const*>(&expr))
		{
			if (auto const* sid = std::get_if<awst::SubroutineID>(&call->target))
				refs.insert(sid->target);
			for (auto const& arg: call->args)
				if (arg.value) collectRefs(*arg.value, refs);
		}
		// Recurse into common expression types that contain sub-expressions
		if (auto const* e = dynamic_cast<awst::UInt64BinaryOperation const*>(&expr))
		{
			if (e->left) collectRefs(*e->left, refs);
			if (e->right) collectRefs(*e->right, refs);
		}
		if (auto const* e = dynamic_cast<awst::BigUIntBinaryOperation const*>(&expr))
		{
			if (e->left) collectRefs(*e->left, refs);
			if (e->right) collectRefs(*e->right, refs);
		}
		if (auto const* e = dynamic_cast<awst::NumericComparisonExpression const*>(&expr))
		{
			if (e->lhs) collectRefs(*e->lhs, refs);
			if (e->rhs) collectRefs(*e->rhs, refs);
		}
		if (auto const* e = dynamic_cast<awst::BytesComparisonExpression const*>(&expr))
		{
			if (e->lhs) collectRefs(*e->lhs, refs);
			if (e->rhs) collectRefs(*e->rhs, refs);
		}
		if (auto const* e = dynamic_cast<awst::BooleanBinaryOperation const*>(&expr))
		{
			if (e->left) collectRefs(*e->left, refs);
			if (e->right) collectRefs(*e->right, refs);
		}
		if (auto const* e = dynamic_cast<awst::Not const*>(&expr))
		{
			if (e->expr) collectRefs(*e->expr, refs);
		}
		if (auto const* e = dynamic_cast<awst::AssertExpression const*>(&expr))
		{
			if (e->condition) collectRefs(*e->condition, refs);
		}
		if (auto const* e = dynamic_cast<awst::AssignmentExpression const*>(&expr))
		{
			if (e->target) collectRefs(*e->target, refs);
			if (e->value) collectRefs(*e->value, refs);
		}
		if (auto const* e = dynamic_cast<awst::ReinterpretCast const*>(&expr))
		{
			if (e->expr) collectRefs(*e->expr, refs);
		}
		if (auto const* e = dynamic_cast<awst::ConditionalExpression const*>(&expr))
		{
			if (e->condition) collectRefs(*e->condition, refs);
			if (e->trueExpr) collectRefs(*e->trueExpr, refs);
			if (e->falseExpr) collectRefs(*e->falseExpr, refs);
		}
		if (auto const* e = dynamic_cast<awst::FieldExpression const*>(&expr))
		{
			if (e->base) collectRefs(*e->base, refs);
		}
		if (auto const* e = dynamic_cast<awst::IndexExpression const*>(&expr))
		{
			if (e->base) collectRefs(*e->base, refs);
			if (e->index) collectRefs(*e->index, refs);
		}
		if (auto const* e = dynamic_cast<awst::IntrinsicCall const*>(&expr))
		{
			for (auto const& arg: e->stackArgs)
				if (arg) collectRefs(*arg, refs);
		}
		if (auto const* e = dynamic_cast<awst::TupleExpression const*>(&expr))
		{
			for (auto const& item: e->items)
				if (item) collectRefs(*item, refs);
		}
		if (auto const* e = dynamic_cast<awst::NamedTupleExpression const*>(&expr))
		{
			for (auto const& [_, v]: e->values)
				if (v) collectRefs(*v, refs);
		}
		if (auto const* e = dynamic_cast<awst::NewStruct const*>(&expr))
		{
			for (auto const& [_, v]: e->values)
				if (v) collectRefs(*v, refs);
		}
		if (auto const* e = dynamic_cast<awst::NewArray const*>(&expr))
		{
			for (auto const& v: e->values)
				if (v) collectRefs(*v, refs);
		}
		if (auto const* e = dynamic_cast<awst::ArrayLength const*>(&expr))
		{
			if (e->array) collectRefs(*e->array, refs);
		}
		if (auto const* e = dynamic_cast<awst::ArrayPop const*>(&expr))
		{
			if (e->base) collectRefs(*e->base, refs);
		}
		if (auto const* e = dynamic_cast<awst::BoxValueExpression const*>(&expr))
		{
			if (e->key) collectRefs(*e->key, refs);
		}
		if (auto const* e = dynamic_cast<awst::BytesBinaryOperation const*>(&expr))
		{
			if (e->left) collectRefs(*e->left, refs);
			if (e->right) collectRefs(*e->right, refs);
		}
		if (auto const* e = dynamic_cast<awst::TupleItemExpression const*>(&expr))
		{
			if (e->base) collectRefs(*e->base, refs);
		}
		if (auto const* e = dynamic_cast<awst::ARC4Encode const*>(&expr))
		{
			if (e->value) collectRefs(*e->value, refs);
		}
		if (auto const* e = dynamic_cast<awst::ARC4Decode const*>(&expr))
		{
			if (e->value) collectRefs(*e->value, refs);
		}
		if (auto const* e = dynamic_cast<awst::Copy const*>(&expr))
		{
			if (e->value) collectRefs(*e->value, refs);
		}
		if (auto const* e = dynamic_cast<awst::SingleEvaluation const*>(&expr))
		{
			if (e->source) collectRefs(*e->source, refs);
		}
		if (auto const* e = dynamic_cast<awst::CheckedMaybe const*>(&expr))
		{
			if (e->expr) collectRefs(*e->expr, refs);
		}
		if (auto const* e = dynamic_cast<awst::Emit const*>(&expr))
		{
			if (e->value) collectRefs(*e->value, refs);
		}
		if (auto const* e = dynamic_cast<awst::ArrayConcat const*>(&expr))
		{
			if (e->left) collectRefs(*e->left, refs);
			if (e->right) collectRefs(*e->right, refs);
		}
		if (auto const* e = dynamic_cast<awst::ArrayExtend const*>(&expr))
		{
			if (e->base) collectRefs(*e->base, refs);
			if (e->other) collectRefs(*e->other, refs);
		}
		if (auto const* e = dynamic_cast<awst::StateGet const*>(&expr))
		{
			if (e->field) collectRefs(*e->field, refs);
			if (e->defaultValue) collectRefs(*e->defaultValue, refs);
		}
		if (auto const* e = dynamic_cast<awst::StateExists const*>(&expr))
		{
			if (e->field) collectRefs(*e->field, refs);
		}
		if (auto const* e = dynamic_cast<awst::StateDelete const*>(&expr))
		{
			if (e->field) collectRefs(*e->field, refs);
		}
		if (auto const* e = dynamic_cast<awst::StateGetEx const*>(&expr))
		{
			if (e->field) collectRefs(*e->field, refs);
		}
		if (auto const* e = dynamic_cast<awst::AppStateExpression const*>(&expr))
		{
			if (e->key) collectRefs(*e->key, refs);
		}
		if (auto const* e = dynamic_cast<awst::AppAccountStateExpression const*>(&expr))
		{
			if (e->key) collectRefs(*e->key, refs);
			if (e->account) collectRefs(*e->account, refs);
		}
		if (auto const* e = dynamic_cast<awst::BoxPrefixedKeyExpression const*>(&expr))
		{
			if (e->prefix) collectRefs(*e->prefix, refs);
			if (e->key) collectRefs(*e->key, refs);
		}
		if (auto const* e = dynamic_cast<awst::CreateInnerTransaction const*>(&expr))
		{
			for (auto const& [_, v]: e->fields)
				if (v) collectRefs(*v, refs);
		}
		if (auto const* e = dynamic_cast<awst::SubmitInnerTransaction const*>(&expr))
		{
			for (auto const& itxn: e->itxns)
				if (itxn) collectRefs(*itxn, refs);
		}
		if (auto const* e = dynamic_cast<awst::InnerTransactionField const*>(&expr))
		{
			if (e->itxn) collectRefs(*e->itxn, refs);
			if (e->arrayIndex) collectRefs(*e->arrayIndex, refs);
		}
		if (auto const* e = dynamic_cast<awst::CommaExpression const*>(&expr))
		{
			for (auto const& ex: e->expressions)
				if (ex) collectRefs(*ex, refs);
		}
	};

	// Collect refs from a statement tree
	std::function<void(awst::Statement const&, std::set<std::string>&)> collectStmtRefs;
	collectStmtRefs = [&](awst::Statement const& stmt, std::set<std::string>& refs) {
		if (auto const* block = dynamic_cast<awst::Block const*>(&stmt))
		{
			for (auto const& s: block->body)
				if (s) collectStmtRefs(*s, refs);
		}
		if (auto const* es = dynamic_cast<awst::ExpressionStatement const*>(&stmt))
		{
			if (es->expr) collectRefs(*es->expr, refs);
		}
		if (auto const* as = dynamic_cast<awst::AssignmentStatement const*>(&stmt))
		{
			if (as->target) collectRefs(*as->target, refs);
			if (as->value) collectRefs(*as->value, refs);
		}
		if (auto const* rs = dynamic_cast<awst::ReturnStatement const*>(&stmt))
		{
			if (rs->value) collectRefs(*rs->value, refs);
		}
		if (auto const* ie = dynamic_cast<awst::IfElse const*>(&stmt))
		{
			if (ie->condition) collectRefs(*ie->condition, refs);
			if (ie->ifBranch) collectStmtRefs(*ie->ifBranch, refs);
			if (ie->elseBranch) collectStmtRefs(*ie->elseBranch, refs);
		}
		if (auto const* wl = dynamic_cast<awst::WhileLoop const*>(&stmt))
		{
			if (wl->condition) collectRefs(*wl->condition, refs);
			if (wl->loopBody) collectStmtRefs(*wl->loopBody, refs);
		}
		if (auto const* sw = dynamic_cast<awst::Switch const*>(&stmt))
		{
			if (sw->value) collectRefs(*sw->value, refs);
			for (auto const& [caseExpr, caseBlock]: sw->cases)
			{
				if (caseExpr) collectRefs(*caseExpr, refs);
				if (caseBlock) collectStmtRefs(*caseBlock, refs);
			}
			if (sw->defaultCase) collectStmtRefs(*sw->defaultCase, refs);
		}
		if (auto const* fl = dynamic_cast<awst::ForInLoop const*>(&stmt))
		{
			if (fl->sequence) collectRefs(*fl->sequence, refs);
			if (fl->items) collectRefs(*fl->items, refs);
			if (fl->loopBody) collectStmtRefs(*fl->loopBody, refs);
		}
		if (auto const* ua = dynamic_cast<awst::UInt64AugmentedAssignment const*>(&stmt))
		{
			if (ua->target) collectRefs(*ua->target, refs);
			if (ua->value) collectRefs(*ua->value, refs);
		}
		if (auto const* ba = dynamic_cast<awst::BigUIntAugmentedAssignment const*>(&stmt))
		{
			if (ba->target) collectRefs(*ba->target, refs);
			if (ba->value) collectRefs(*ba->value, refs);
		}
	};

	// Collect refs from a method body
	auto collectMethodRefs = [&](awst::ContractMethod const& method, std::set<std::string>& refs) {
		if (method.body)
			collectStmtRefs(*method.body, refs);
	};

	// Build reachability set
	std::set<std::string> reachable;
	std::queue<std::string> worklist;

	// Seed with contract method references
	for (auto const& root: roots)
	{
		if (auto const* contract = dynamic_cast<awst::Contract const*>(root.get()))
		{
			std::set<std::string> refs;
			collectMethodRefs(contract->approvalProgram, refs);
			collectMethodRefs(contract->clearProgram, refs);
			for (auto const& method: contract->methods)
				collectMethodRefs(method, refs);
			for (auto const& id: refs)
			{
				reachable.insert(id);
				worklist.push(id);
			}
		}
	}

	// Build ID→subroutine map
	std::unordered_map<std::string, awst::Subroutine const*> subMap;
	for (auto const& root: roots)
	{
		if (auto const* sub = dynamic_cast<awst::Subroutine const*>(root.get()))
			subMap[sub->id] = sub;
	}

	// Transitively find all reachable subroutines
	while (!worklist.empty())
	{
		std::string id = worklist.front();
		worklist.pop();
		auto it = subMap.find(id);
		if (it == subMap.end())
			continue;
		std::set<std::string> refs;
		if (it->second->body)
			collectStmtRefs(*it->second->body, refs);
		for (auto const& ref: refs)
		{
			if (reachable.find(ref) == reachable.end())
			{
				reachable.insert(ref);
				worklist.push(ref);
			}
		}
	}

	// Filter roots: keep contracts and reachable subroutines
	std::vector<std::shared_ptr<awst::RootNode>> filteredRoots;
	for (auto& root: roots)
	{
		if (dynamic_cast<awst::Contract const*>(root.get()))
		{
			filteredRoots.push_back(std::move(root));
		}
		else if (auto const* sub = dynamic_cast<awst::Subroutine const*>(root.get()))
		{
			if (reachable.count(sub->id))
				filteredRoots.push_back(std::move(root));
		}
		else
		{
			filteredRoots.push_back(std::move(root));
		}
	}

	return filteredRoots;
}

} // namespace puyasol::builder
