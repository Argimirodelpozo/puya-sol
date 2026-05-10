#include "builder/AWSTBuilder.h"
#include "awst/Termination.h"
#include "builder/SubroutineReachability.h"
#include "builder/builtin/Ripemd160Builder.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-eb/FunctionPointerBuilder.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

using awst::statementAlwaysTerminates;
using awst::blockAlwaysTerminates;

namespace {
/// Walk a function body, identifying which `memory` ref params are
/// actually written. A param is "mutated" if any assignment LHS roots
/// back to that param's identifier (after peeling member / index
/// accesses). Used to decide whether to augment the function's return
/// type with that param — read-only memory-ref params skip the
/// augmentation to keep the return tuple small (otherwise honk's
/// transcript helpers, which take a ~14 KB Honk.Proof memory but only
/// READ it, would have a 14 KB+ augmented return that overflows the
/// AVM 4096 B stack-element cap and gets skipped from lifting).
class MemoryParamMutationDetector
	: public solidity::frontend::ASTConstVisitor
{
public:
	std::set<int64_t> paramIds;        // input — set of param decl ids
	std::set<int64_t> mutatedParams;   // output — subset of paramIds

	bool visit(solidity::frontend::Assignment const& a) override
	{
		recordRoot(&a.leftHandSide());
		return true;  // recurse so RHS's nested assignments also count
	}

private:
	void recordRoot(solidity::frontend::Expression const* lhs)
	{
		using namespace solidity::frontend;
		// Peel index / member accesses to find the root identifier.
		while (true)
		{
			if (auto const* ix = dynamic_cast<IndexAccess const*>(lhs))
				{ lhs = &ix->baseExpression(); continue; }
			if (auto const* mb = dynamic_cast<MemberAccess const*>(lhs))
				{ lhs = &mb->expression(); continue; }
			if (auto const* tup = dynamic_cast<TupleExpression const*>(lhs))
			{
				// Tuple destructuring: each component is an LHS.
				for (auto const& c : tup->components())
					if (c) recordRoot(c.get());
				return;
			}
			break;
		}
		if (auto const* id = dynamic_cast<Identifier const*>(lhs))
		{
			auto const* decl = id->annotation().referencedDeclaration;
			if (decl && paramIds.count(decl->id()))
				mutatedParams.insert(decl->id());
		}
	}
};
} // namespace

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

	registerFunctionIds(_compiler, _sourceFile);
	presetDispatchCref(_compiler, _sourceFile);
	translateLibraryFunctions(_compiler, _sourceFile, roots);
	translateFreeFunctions(_compiler, _sourceFile, roots);
	translateContracts(_compiler, _sourceFile, _opupBudget, _ensureBudget, _viaYulBehavior, roots);

	// Inject the synthetic RIPEMD-160 subroutine. Always emitted; the
	// reachability filter below drops it when no contract calls it.
	{
		awst::SourceLocation builtinLoc;
		builtinLoc.file = _sourceFile;
		roots.push_back(builder::builtin::buildRipemd160Subroutine(builtinLoc));
	}

	// Drop any subroutine root not reachable from a contract method.
	return filterToReachableSubroutines(std::move(roots));
}

void AWSTBuilder::registerFunctionIds(
	solidity::frontend::CompilerStack& _compiler,
	std::string const& _sourceFile)
{

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
}

void AWSTBuilder::presetDispatchCref(
	solidity::frontend::CompilerStack& _compiler,
	std::string const& _sourceFile)
{
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
				return;
			}
		}
	}
}

void AWSTBuilder::translateLibraryFunctions(
	solidity::frontend::CompilerStack& _compiler,
	std::string const& _sourceFile,
	std::vector<std::shared_ptr<awst::RootNode>>& roots)
{
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& sourceUnit = _compiler.ast(sourceName);

		// Use solc's filteredNodes<T>() template instead of dynamic_cast'ing
		// each node — same effect, less boilerplate.
		for (auto const* contract: solidity::frontend::ASTNode::filteredNodes<
			solidity::frontend::ContractDefinition>(sourceUnit.nodes()))
		{
			if (!contract->isLibrary())
				continue;

			std::string libraryName = contract->name();
			Logger::instance().info("Translating library: " + libraryName);

			for (auto const* func: contract->definedFunctions())
			{
				if (!func->isImplemented())
					continue;

				// Resolve the qualified name + subroutine ID via AST-id-first lookup
				// (precise overload resolution) with name-based fallback.
				std::string qualifiedName = libraryName + "." + func->name();
				std::string subroutineId;
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
				// parameters (external/delegate-call pointers can't be
				// represented on AVM). Internal function pointers work via
				// our dispatch table mechanism.
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

				Logger::instance().debug("Translating library function: " + qualifiedName);
				roots.push_back(buildFreestandingSubroutine(
					*func, _sourceFile, qualifiedName, subroutineId, libraryName));
			}
		}
	}
}

void AWSTBuilder::translateFreeFunctions(
	solidity::frontend::CompilerStack& _compiler,
	std::string const& _sourceFile,
	std::vector<std::shared_ptr<awst::RootNode>>& roots)
{
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& sourceUnit = _compiler.ast(sourceName);

		for (auto const* func: solidity::frontend::ASTNode::filteredNodes<
			solidity::frontend::FunctionDefinition>(sourceUnit.nodes()))
		{
			if (!func->isImplemented() || !func->isFree())
				continue;

			std::string qualifiedName = func->name();
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
			roots.push_back(buildFreestandingSubroutine(
				*func, _sourceFile, qualifiedName, subroutineId, /*libraryName=*/""));
		}
	}
}

std::shared_ptr<awst::Subroutine> AWSTBuilder::buildFreestandingSubroutine(
	solidity::frontend::FunctionDefinition const& _func,
	std::string const& _sourceFile,
	std::string const& _qualifiedName,
	std::string const& _subroutineId,
	std::string const& _libraryName)
{
	auto sub = std::make_shared<awst::Subroutine>();
	sub->inlineOpt = false; // Prevent puya from inlining large subroutines

	awst::SourceLocation loc;
	loc.file = _sourceFile;
	loc.line = _func.location().start >= 0 ? _func.location().start : 0;
	loc.endLine = _func.location().end >= 0 ? _func.location().end : 0;

	sub->sourceLocation = loc;
	sub->id = _subroutineId;
	sub->name = _qualifiedName;

	// Documentation
	if (_func.documentation())
		sub->documentation.description = *_func.documentation()->text();

	// Parameters — mapping storage refs become bytes (runtime key prefix).
	// Free functions don't have storage refs, so the library-only branch is a no-op there.
	std::set<size_t> mappingStorageParams;
	for (size_t pi = 0; pi < _func.parameters().size(); ++pi)
	{
		auto const& param = _func.parameters()[pi];
		awst::SubroutineArgument arg;
		arg.name = param->name();
		if (arg.name.empty())
			arg.name = "_param" + std::to_string(pi);
		arg.sourceLocation.file = _sourceFile;
		arg.sourceLocation.line = param->location().start >= 0 ? param->location().start : 0;
		arg.sourceLocation.endLine = param->location().end >= 0 ? param->location().end : 0;

		// Mapping storage refs: callee receives the box key PREFIX as bytes
		// so `m[k]` → box_get(prefix+sha256(k)) uses the caller's storage var
		// name, not the param name.
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

	// Detect storage-ref params for return-type augmentation. Callers receive
	// the modified value as an extra return slot for box write-back. Mapping
	// storage refs are excluded — they share box keys, no write-back needed.
	// Skip private library functions: puya threads their mutable args
	// internally so no augmentation is needed (and would be incorrect).
	std::vector<size_t> storageParamIndices;
	bool isMutating = _func.stateMutability() != solidity::frontend::StateMutability::View
		&& _func.stateMutability() != solidity::frontend::StateMutability::Pure;
	bool isPrivate = _func.visibility() == solidity::frontend::Visibility::Private;
	if (isMutating && !isPrivate)
	{
		for (size_t pi = 0; pi < _func.parameters().size(); ++pi)
		{
			auto refLoc = _func.parameters()[pi]->referenceLocation();
			if (refLoc == solidity::frontend::VariableDeclaration::Location::Storage
				&& !mappingStorageParams.count(pi))
				storageParamIndices.push_back(pi);
		}
	}

	// Detect `memory` reference params for return-type augmentation. Solidity
	// passes memory refs by reference, but our value-typed translation copies
	// at the boundary — mutations inside the callee don't propagate. To
	// recover the by-ref semantics for the common pattern (callee mutates
	// `Fr[N] memory evals` then returns void), we tag each memory-ref param
	// as an extra return; SolInternalCall captures the tuple and writes the
	// post-call value back to the caller's local. Applies regardless of
	// `pure` — Solidity allows pure functions to mutate memory params.
	// Skipped on private functions for the same reason as storage refs
	// (puya may thread these internally) and on bytes/string/mapping (those
	// keep their existing translations).
	//
	// Augment ONLY params that are actually mutated. Use-def analysis
	// catches assignments whose LHS root is a param after peeling
	// member/index access; tuple destructuring counts each component
	// as an LHS. Read-only memory-ref params (e.g. honk transcripts'
	// `Honk.Proof memory proof`, used to extract challenges but never
	// written) skip augmentation — otherwise the augmented return tuple
	// grows by their full ARC4 size, which for the 14 KB Proof struct
	// blows past the AVM 4096 B per-stack-element cap and the pure-
	// helper extractor refuses to lift them.
	std::vector<size_t> memoryRefParamIndices;
	if (!isPrivate && _func.isImplemented())
	{
		auto isMemRefType = [](solidity::frontend::Type const* t) {
			if (auto const* arr = dynamic_cast<solidity::frontend::ArrayType const*>(t))
				return !arr->isByteArrayOrString();
			return dynamic_cast<solidity::frontend::StructType const*>(t) != nullptr;
		};
		MemoryParamMutationDetector detector;
		for (auto const& p : _func.parameters())
			detector.paramIds.insert(p->id());
		_func.body().accept(detector);
		for (size_t pi = 0; pi < _func.parameters().size(); ++pi)
		{
			auto const& p = _func.parameters()[pi];
			if (p->referenceLocation()
				!= solidity::frontend::VariableDeclaration::Location::Memory)
				continue;
			if (!p->type() || !isMemRefType(p->type()))
				continue;
			if (!detector.mutatedParams.count(p->id()))
				continue;  // read-only — no need to thread the post-call value back
			memoryRefParamIndices.push_back(pi);
		}
	}

	// Return type — augment with storage + memory param types so callers can
	// capture the mutated value.
	auto const& returnParams = _func.returnParameters();
	{
		std::vector<awst::WType const*> types;
		for (auto const& rp: returnParams)
			types.push_back(m_typeMapper.map(rp->type()));
		for (size_t idx: storageParamIndices)
			types.push_back(sub->args[idx].wtype);
		for (size_t idx: memoryRefParamIndices)
			types.push_back(sub->args[idx].wtype);

		if (types.empty())
			sub->returnType = awst::WType::voidType();
		else if (types.size() == 1)
			sub->returnType = types[0];
		else
			sub->returnType = new awst::WTuple(std::move(types));
	}

	sub->pure = _func.stateMutability() == solidity::frontend::StateMutability::Pure;

	// Build body via a fresh ContractContext.
	// NOTE: ContractContext stores `overloadedNames` as `unordered_set const&`,
	// so we must pass a long-lived object (not a temporary `{}` — that would
	// dangle after the constructor returns and SIGSEGV on first access).
	static std::unordered_set<std::string> const EMPTY_OVERLOAD_NAMES;
	eb::ContractContext exprBuilder(
		m_typeMapper, *m_storageMapper, _sourceFile, _libraryName, m_libraryFunctionIds,
		EMPTY_OVERLOAD_NAMES, m_freeFunctionById
	);

	sol_ast::TranslationContext tr{exprBuilder, m_typeMapper, _sourceFile};
	auto trGuard = exprBuilder.pushScopeRaii(&tr);
	sol_ast::FunctionContext fnCtx{tr, {}, sub->returnType, {}};
	auto fnGuard = exprBuilder.pushScopeRaii(&fnCtx);

	// Register mapping-storage-ref params so SolIndexAccess can build dynamic
	// box-key prefixes at runtime. Must happen *after* the FunctionContext
	// is pushed — `setMappingKeyParam` writes into the innermost enclosing
	// FunctionContext via `nearestFunction(currentScope)`.
	for (size_t idx: mappingStorageParams)
	{
		auto const& param = _func.parameters()[idx];
		fnCtx.setMappingKeyParam(param->id(), param->name());
	}

	// Param + return-param context for inline assembly + sub-word integer truncation.
	{
		std::vector<std::pair<std::string, awst::WType const*>> paramContext;
		std::map<std::string, unsigned> bitWidths;
		for (size_t pi = 0; pi < _func.parameters().size(); ++pi)
		{
			auto const& param = _func.parameters()[pi];
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
		for (auto const& rp: _func.returnParameters())
		{
			auto const* solType = rp->annotation().type;
			auto const* intType = solType ? dynamic_cast<solidity::frontend::IntegerType const*>(solType) : nullptr;
			if (!intType && solType)
				if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
					intType = dynamic_cast<solidity::frontend::IntegerType const*>(&udvt->underlyingType());
			if (intType && intType->numBits() < 64)
				bitWidths[rp->name()] = intType->numBits();
		}
		fnCtx.params = paramContext;
		fnCtx.returnType = sub->returnType;
		fnCtx.paramBitWidths = bitWidths;
	}

	// Construct the function-body block context BEFORE registering named
	// returns — `resolveVarName` writes into the innermost BlockContext via
	// `nearestBlock(currentScope)`, so the block must already be the
	// current scope.
	auto blk = sol_ast::BlockContext::top(fnCtx);
	auto blkGuard = exprBuilder.pushScopeRaii(&blk);

	// Register named return variable names so inner scoping detects shadowing.
	for (auto const& rp: returnParams)
		if (!rp->name().empty())
			blk.resolveVarName(rp->name(), rp->id());

	// Register mapping-storage-ref return params: `function f() returns (mapping(K=>V) storage r)`
	// — `r` is a local pointer to a mapping; r[k] resolves to box access prefixed
	// by `r`'s runtime bytes value (the holder name).
	for (auto const& rp: returnParams)
	{
		if (rp->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
			&& dynamic_cast<solidity::frontend::MappingType const*>(rp->type())
			&& !rp->name().empty())
		{
			fnCtx.setMappingKeyParam(rp->id(), rp->name());
		}
	}

	sub->body = sol_ast::buildBlock(blk, _func.body());

	// Insert zero-initialization for named return variables — Solidity
	// implicitly initializes named returns to their zero values. Skip
	// `bytes`/`bytesN` defaults that are produced via specialized constants
	// to avoid emitting StorageMapper-style heavy default values.
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
				zeroVal = awst::makeIntegerConstant("0", loc, rpType);
			}
			else if (rpType && rpType->kind() == awst::WTypeKind::Bytes)
			{
				// For fixed-size bytes types (bytes1..bytes32), produce N zero bytes.
				std::vector<uint8_t> bytes;
				auto const* bytesType = dynamic_cast<awst::BytesWType const*>(rpType);
				if (bytesType && bytesType->length().has_value())
					bytes.assign(bytesType->length().value(), 0);
				zeroVal = awst::makeBytesConstant(
					std::move(bytes), loc, awst::BytesEncoding::Base16, rpType);
			}
			else
			{
				// Complex types (structs, arrays, etc.) — use makeDefaultValue.
				// Solidity may assign individual fields (e.g. `vk.alfa1 = ...`)
				// which read other fields from the uninitialized variable via
				// the copy-on-write NewStruct pattern.
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

	// Augment return statements: append storage param values so callers can
	// write the modified struct back to their storage.
	if (!storageParamIndices.empty() || !memoryRefParamIndices.empty())
	{
		// sub->returnType is either:
		//   - a WTuple (multi-element augmented return), OR
		//   - a bare type (void function + exactly 1 augmented arg → that
		//     arg's type; one return param + zero augmented args isn't
		//     reachable here because we only enter this branch when at
		//     least one augmented index is present; one return + zero
		//     augmented args is the void+1 case).
		// Wrapping a single value in a TupleExpression with the bare
		// element type as wtype produces invalid AWST (puya rejects
		// `TupleExpression.wtype` non-WTuple). Branch on shape.
		bool returnIsTuple =
			(dynamic_cast<awst::WTuple const*>(sub->returnType) != nullptr);
		size_t totalAugmented = storageParamIndices.size() + memoryRefParamIndices.size();

		std::function<void(awst::Block&)> augmentReturns;
		augmentReturns = [&](awst::Block& block) {
			for (auto& stmt: block.body)
			{
				if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
				{
					if (!returnIsTuple)
					{
						// sub->returnType is a bare type. There must be exactly
						// one source: either ret->value (if author wrote
						// `return val;`) OR the single augmented arg (if author
						// wrote bare `return;`). User-written `return val;` in
						// a void+1-arg-augmented function isn't valid Solidity,
						// so we only handle the bare-return case.
						if (!ret->value && totalAugmented == 1)
						{
							size_t idx = !storageParamIndices.empty()
								? storageParamIndices[0]
								: memoryRefParamIndices[0];
							ret->value = awst::makeVarExpression(
								sub->args[idx].name, sub->args[idx].wtype,
								ret->sourceLocation);
						}
						// else: leave ret->value as-is; the type-check at the
						// puya boundary will report the actual mismatch.
					}
					else
					{
						auto tuple = awst::makeTupleExpression(sub->returnType, ret->sourceLocation);
						if (ret->value)
						{
							// If author wrote `return (a, b, c);`, flatten the
							// tuple's items into the new augmented tuple — the
							// augmented return-type WTuple is flat, so nesting
							// here would produce a shape mismatch.
							if (auto* origTup = dynamic_cast<awst::TupleExpression*>(ret->value.get()))
							{
								for (auto& it: origTup->items)
									tuple->items.push_back(it);
							}
							else
							{
								tuple->items.push_back(ret->value);
							}
						}
						for (size_t idx: storageParamIndices)
						{
							auto pv = awst::makeVarExpression(
								sub->args[idx].name, sub->args[idx].wtype,
								ret->sourceLocation);
							tuple->items.push_back(std::move(pv));
						}
						for (size_t idx: memoryRefParamIndices)
						{
							auto pv = awst::makeVarExpression(
								sub->args[idx].name, sub->args[idx].wtype,
								ret->sourceLocation);
							tuple->items.push_back(std::move(pv));
						}
						ret->value = std::move(tuple);
					}
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

	// Special case: assembly-only library functions with known semantics.
	if (sub->body->body.empty() && _func.name() == "efficientKeccak256"
		&& _func.parameters().size() == 2)
	{
		auto varA = awst::makeVarExpression(_func.parameters()[0]->name(), m_typeMapper.map(_func.parameters()[0]->type()), loc);
		auto varB = awst::makeVarExpression(_func.parameters()[1]->name(), m_typeMapper.map(_func.parameters()[1]->type()), loc);
		auto concat = awst::makeConcat(std::move(varA), std::move(varB), loc);
		auto hash = awst::makeKeccak256(std::move(concat), loc);
		auto cast = awst::makeReinterpretCast(std::move(hash), sub->returnType, loc);
		auto ret = awst::makeReturnStatement(std::move(cast), loc);
		sub->body->body.push_back(std::move(ret));
	}

	// Synthesize an implicit return when the body falls through. Three cases:
	//  1. Void return + storage/memory augmentation → return augmented args.
	//  2. Named returns                              → return named values.
	//  3. Otherwise                                  → return makeDefaultValue(returnType).
	if (!awst::blockAlwaysTerminates(*sub->body)
		&& (!returnParams.empty() || !storageParamIndices.empty()
			|| !memoryRefParamIndices.empty()))
	{
		bool hasNamedReturns = false;
		for (auto const& rp: returnParams)
			if (!rp->name().empty())
				hasNamedReturns = true;

		size_t totalAugmented2 = storageParamIndices.size() + memoryRefParamIndices.size();
		if (!hasNamedReturns && returnParams.empty() && totalAugmented2 > 0)
		{
			// Void return with storage/memory augmentation — append return
			// of the augmented args (in storage-then-memory order, matching
			// the return-type tuple layout above).
			auto implicitReturn = awst::makeReturnStatement(nullptr, loc);
			if (totalAugmented2 == 1)
			{
				size_t idx = !storageParamIndices.empty()
					? storageParamIndices[0]
					: memoryRefParamIndices[0];
				implicitReturn->value = awst::makeVarExpression(sub->args[idx].name, sub->args[idx].wtype, loc);
			}
			else
			{
				auto tuple = awst::makeTupleExpression(sub->returnType, loc);
				for (size_t idx: storageParamIndices)
					tuple->items.push_back(awst::makeVarExpression(sub->args[idx].name, sub->args[idx].wtype, loc));
				for (size_t idx: memoryRefParamIndices)
					tuple->items.push_back(awst::makeVarExpression(sub->args[idx].name, sub->args[idx].wtype, loc));
				implicitReturn->value = std::move(tuple);
			}
			sub->body->body.push_back(std::move(implicitReturn));
		}
		else if (hasNamedReturns)
		{
			auto implicitReturn = awst::makeReturnStatement(nullptr, loc);

			// When the return type was augmented with storage / memory-ref
			// args, we must include those after the named-return values so
			// the synthesized return tuple matches `sub->returnType`.
			if (returnParams.size() == 1 && totalAugmented2 == 0)
			{
				auto var = awst::makeVarExpression(returnParams[0]->name(), m_typeMapper.map(returnParams[0]->type()), loc);
				implicitReturn->value = std::move(var);
			}
			else
			{
				auto tuple = awst::makeTupleExpression(nullptr, loc);
				for (auto const& rp: returnParams)
				{
					auto var = awst::makeVarExpression(rp->name(), m_typeMapper.map(rp->type()), loc);
					tuple->items.push_back(std::move(var));
				}
				for (size_t idx: storageParamIndices)
					tuple->items.push_back(awst::makeVarExpression(
						sub->args[idx].name, sub->args[idx].wtype, loc));
				for (size_t idx: memoryRefParamIndices)
					tuple->items.push_back(awst::makeVarExpression(
						sub->args[idx].name, sub->args[idx].wtype, loc));
				tuple->wtype = sub->returnType;
				implicitReturn->value = std::move(tuple);
			}

			sub->body->body.push_back(std::move(implicitReturn));
		}
		else
		{
			// No named returns — append default return with zero value.
			auto defReturn = awst::makeReturnStatement(StorageMapper::makeDefaultValue(sub->returnType, loc), loc);
			sub->body->body.push_back(std::move(defReturn));
		}
	}

	return sub;
}

void AWSTBuilder::translateContracts(
	solidity::frontend::CompilerStack& _compiler,
	std::string const& _sourceFile,
	uint64_t _opupBudget,
	std::map<std::string, uint64_t> const& _ensureBudget,
	bool _viaYulBehavior,
	std::vector<std::shared_ptr<awst::RootNode>>& roots)
{
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& sourceUnit = _compiler.ast(sourceName);

		for (auto const* contract: solidity::frontend::ASTNode::filteredNodes<
			solidity::frontend::ContractDefinition>(sourceUnit.nodes()))
		{
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

				auto body = awst::makeBlock(dummy.sourceLocation);
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
}

} // namespace puyasol::builder
