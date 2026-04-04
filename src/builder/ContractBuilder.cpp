#include "builder/ContractBuilder.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/storage/StorageLayout.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

#include <boost/multiprecision/cpp_int.hpp>
#include <map>
#include <set>

namespace puyasol::builder
{

/// Recursively checks if a statement list always terminates (return or revert).
static bool blockAlwaysReturns(std::vector<std::shared_ptr<awst::Statement>> const& _stmts)
{
	if (_stmts.empty())
		return false;
	auto const& last = _stmts.back();
	auto type = last->nodeType();
	if (type == "ReturnStatement")
		return true;
	if (type == "IfElse")
	{
		auto const& ifElse = static_cast<awst::IfElse const&>(*last);
		bool ifReturns = blockAlwaysReturns(ifElse.ifBranch->body);
		bool elseReturns = ifElse.elseBranch && blockAlwaysReturns(ifElse.elseBranch->body);
		return ifReturns && elseReturns;
	}
	// ExpressionStatement containing assert(false) is a guaranteed revert
	if (type == "ExpressionStatement")
	{
		auto const& exprStmt = static_cast<awst::ExpressionStatement const&>(*last);
		if (auto const* assertExpr = dynamic_cast<awst::AssertExpression const*>(exprStmt.expr.get()))
		{
			// Only assert(false) is guaranteed to terminate — assert(variable) may pass
			if (auto const* boolConst = dynamic_cast<awst::BoolConstant const*>(assertExpr->condition.get()))
				if (!boolConst->value)
					return true;
		}
	}
	return false;
}

/// Checks if a Solidity AST subtree references any state variable whose AST ID
/// is in the given set (i.e. box-stored state variables).
class BoxVarRefChecker: public solidity::frontend::ASTConstVisitor
{
public:
	explicit BoxVarRefChecker(std::set<int64_t> const& _boxVarIds): m_boxVarIds(_boxVarIds) {}
	bool found() const { return m_found; }

	bool visit(solidity::frontend::Identifier const& _node) override
	{
		if (m_found)
			return false;
		if (auto const* decl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
				_node.annotation().referencedDeclaration))
		{
			if (m_boxVarIds.count(decl->id()))
				m_found = true;
		}
		return !m_found;
	}

private:
	std::set<int64_t> const& m_boxVarIds;
	bool m_found = false;
};

/// Collects AST IDs of base functions that are called via `super.method()`.
/// These need to be emitted as separate subroutines with distinct names.
class SuperCallCollector: public solidity::frontend::ASTConstVisitor
{
public:
	/// AST IDs of base functions referenced by super.f() calls (MRO-dependent).
	std::set<int64_t> superTargetIds;
	/// AST IDs of base functions referenced by explicit Base.f() calls (fixed target).
	std::set<int64_t> explicitBaseTargetIds;

	bool visit(solidity::frontend::MemberAccess const& _node) override
	{
		auto const* baseType = _node.expression().annotation().type;
		if (!baseType)
			return true;
		// Unwrap TypeType if needed (super has type TypeType(ContractType(isSuper=true)))
		if (baseType->category() == solidity::frontend::Type::Category::TypeType)
		{
			auto const* typeType = dynamic_cast<solidity::frontend::TypeType const*>(baseType);
			if (typeType)
				baseType = typeType->actualType();
		}
		if (baseType->category() == solidity::frontend::Type::Category::Contract)
		{
			auto const* contractType = dynamic_cast<solidity::frontend::ContractType const*>(baseType);
			if (contractType)
			{
				auto const* refDecl = _node.annotation().referencedDeclaration;
				if (!refDecl)
					return true;

				if (contractType->isSuper())
				{
					// super.f() — MRO-dependent resolution
					superTargetIds.insert(refDecl->id());
				}
				else
				{
					// Explicit Base.f() — always calls the specific base version
					bool isExplicitBase = _node.expression().annotation().type
						&& _node.expression().annotation().type->category()
							== solidity::frontend::Type::Category::TypeType;
					if (isExplicitBase)
						explicitBaseTargetIds.insert(refDecl->id());
				}
			}
		}
		return true;
	}
};

static bool blockAlwaysTerminates(awst::Block const& _block)
{
	if (_block.body.empty())
		return false;
	auto const& last = _block.body.back();
	if (dynamic_cast<awst::ReturnStatement const*>(last.get()))
		return true;
	if (auto const* exprStmt = dynamic_cast<awst::ExpressionStatement const*>(last.get()))
	{
		if (auto const* assertExpr = dynamic_cast<awst::AssertExpression const*>(exprStmt->expr.get()))
			if (auto const* boolConst = dynamic_cast<awst::BoolConstant const*>(assertExpr->condition.get()))
				if (!boolConst->value)
					return true;
	}
	if (auto const* ifElse = dynamic_cast<awst::IfElse const*>(last.get()))
	{
		if (!ifElse->elseBranch)
			return false;
		return blockAlwaysTerminates(*ifElse->ifBranch) && blockAlwaysTerminates(*ifElse->elseBranch);
	}
	return false;
}

ContractBuilder::ContractBuilder(
	TypeMapper& _typeMapper,
	StorageMapper& _storageMapper,
	std::string const& _sourceFile,
	LibraryFunctionIdMap const& _libraryFunctionIds,
	uint64_t _opupBudget,
	FreeFunctionIdMap const& _freeFunctionById,
	std::map<std::string, uint64_t> const& _ensureBudget
)
	: m_typeMapper(_typeMapper),
	  m_storageMapper(_storageMapper),
	  m_sourceFile(_sourceFile),
	  m_libraryFunctionIds(_libraryFunctionIds),
	  m_opupBudget(_opupBudget),
	  m_freeFunctionById(_freeFunctionById),
	  m_ensureBudget(_ensureBudget)
{
	Logger::instance().debug("[TRACE] ContractBuilder m_freeFunctionById.size()=" + std::to_string(m_freeFunctionById.size()) + " addr=" + std::to_string((uintptr_t)&m_freeFunctionById));
}

awst::SourceLocation ContractBuilder::makeLoc(
	solidity::langutil::SourceLocation const& _solLoc
)
{
	awst::SourceLocation loc;
	loc.file = m_sourceFile;
	loc.line = _solLoc.start >= 0 ? _solLoc.start : 0;
	loc.endLine = _solLoc.end >= 0 ? _solLoc.end : 0;
	return loc;
}

std::shared_ptr<awst::Block> ContractBuilder::buildBlock(
	solidity::frontend::Block const& _block)
{
	return sol_ast::buildBlock(m_stmtCtx, *m_exprBuilder, _block);
}

void ContractBuilder::setFunctionContext(
	std::vector<std::pair<std::string, awst::WType const*>> const& _params,
	awst::WType const* _returnType,
	std::map<std::string, unsigned> const& _bitWidths)
{
	m_stmtCtx.functionParams = _params;
	m_stmtCtx.returnType = _returnType;
	m_stmtCtx.functionParamBitWidths = _bitWidths;
}

void ContractBuilder::setPlaceholderBody(std::shared_ptr<awst::Block> _body)
{
	m_stmtCtx.placeholderBody = std::move(_body);
}

std::shared_ptr<awst::Contract> ContractBuilder::build(
	solidity::frontend::ContractDefinition const& _contract
)
{
	m_currentContract = &_contract;
	std::string contractName = _contract.name();
	std::string contractId = m_sourceFile + "." + contractName;

	// Collect transient state variables
	m_transientStorage.collectVars(_contract, m_typeMapper);
	// Note: setTransientStorage called after m_exprBuilder is created (below)

	// Detect overloaded function names across all linearized base contracts
	// Must happen BEFORE creating translators so constructor body uses correct names.
	// Virtual overrides should NOT count as separate overloads — they occupy the same
	// "slot" as the base function. Only true overloads (same name, different params) count.
	m_overloadedNames.clear();
	{
		// Collect unique function signatures (name + param types) after override resolution.
		// The most-derived version wins — skip base functions that are overridden.
		std::set<int64_t> overriddenIds; // AST IDs of base functions that have been overridden
		for (auto const* base: _contract.annotation().linearizedBaseContracts)
		{
			for (auto const* func: base->definedFunctions())
			{
				if (func->isConstructor() || !func->isImplemented())
					continue;
				// Mark all base functions of this override as overridden
				for (auto const* baseFunc: func->annotation().baseFunctions)
					overriddenIds.insert(baseFunc->id());
			}
		}

		std::unordered_map<std::string, int> nameCount;
		for (auto const* base: _contract.annotation().linearizedBaseContracts)
		{
			for (auto const* func: base->definedFunctions())
			{
				if (func->isConstructor() || !func->isImplemented())
					continue;
				// Skip functions that have been overridden by a more-derived version
				if (overriddenIds.count(func->id()))
					continue;
				nameCount[func->name()]++;
			}
		}
		for (auto const& [name, count]: nameCount)
		{
			if (count > 1)
			{
				m_overloadedNames.insert(name);
				Logger::instance().debug("Overloaded function: " + name + " (" + std::to_string(count) + " versions)");
			}
		}
	}

	// Create translators for this contract (with overload info)
	m_exprBuilder = std::make_unique<ExpressionBuilder>(
		m_typeMapper, m_storageMapper, m_sourceFile, contractName,
		m_libraryFunctionIds, m_overloadedNames, m_freeFunctionById
	);
	// Initialize statement context
	m_stmtCtx = sol_ast::StatementContext{
		&*m_exprBuilder, &m_typeMapper, m_sourceFile,
		[this](solidity::frontend::Expression const& e) { return m_exprBuilder->build(e); },
		[this](solidity::frontend::Statement const& s) {
			return sol_ast::buildStatement(m_stmtCtx, *m_exprBuilder, s);
		},
		[this](solidity::frontend::Block const& b) {
			return sol_ast::buildBlock(m_stmtCtx, *m_exprBuilder, b);
		},
		[this]() { return m_exprBuilder->takePrePendingStatements(); },
		[this]() { return m_exprBuilder->takePendingStatements(); },
		{}, nullptr, {}, nullptr, nullptr, nullptr,
	};

	m_exprBuilder->setTransientStorage(
		m_transientStorage.hasTransientVars() ? &m_transientStorage : nullptr);

	auto contract = std::make_shared<awst::Contract>();
	contract->sourceLocation = makeLoc(_contract.location());
	contract->id = contractId;
	contract->name = contractName;

	// Description from NatSpec
	if (_contract.documentation())
		contract->description = *_contract.documentation()->text();

	// Method resolution order (linearized base contracts)
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		if (base != &_contract)
			contract->methodResolutionOrder.push_back(
				m_sourceFile + "." + base->name()
			);
	}

	// State variables → AppStorageDefinitions
	contract->appState = m_storageMapper.mapStateVariables(_contract, m_sourceFile);

	// Reserve scratch slots 0-4 for EVM memory simulation
	contract->reservedScratchSpace = AssemblyBuilder::reservedScratchSlots();

	// Approval and clear programs
	m_postInitMethod.reset();
	contract->approvalProgram = buildApprovalProgram(_contract, contractName);
	contract->clearProgram = buildClearProgram(_contract, contractName);

	// If constructor auto-split was triggered, add the __postInit method
	// and the __ctor_pending state variable
	if (m_postInitMethod)
	{
		// Add __ctor_pending global state variable
		awst::AppStorageDefinition ctorPendingState;
		ctorPendingState.memberName = "__ctor_pending";
		ctorPendingState.sourceLocation = contract->approvalProgram.sourceLocation;
		ctorPendingState.storageKind = awst::AppStorageKind::AppGlobal;
		ctorPendingState.storageWType = awst::WType::uint64Type();
		auto key = std::make_shared<awst::BytesConstant>();
		key->sourceLocation = ctorPendingState.sourceLocation;
		key->wtype = awst::WType::bytesType();
		key->encoding = awst::BytesEncoding::Utf8;
		std::string keyStr = "__ctor_pending";
		key->value = std::vector<uint8_t>(keyStr.begin(), keyStr.end());
		ctorPendingState.key = key;
		contract->appState.push_back(std::move(ctorPendingState));

		contract->methods.push_back(std::move(*m_postInitMethod));
		m_postInitMethod.reset();
	}

	// Scan all functions (own + inherited) for super.method() calls.
	// Collect AST IDs of base functions that need separate subroutines.
	// MRO-aware super resolution: super.f() in contract X calls the NEXT
	// implementation of f in the most-derived contract's MRO, not X's own base.
	// E.g., for D is B,C: MRO = D→C→B→A. C.super.f() should call B.f(), not A.f().

	// Build per-function-name MRO chains
	auto const& mro = _contract.annotation().linearizedBaseContracts;
	struct MroChainEntry {
		solidity::frontend::ContractDefinition const* contract;
		solidity::frontend::FunctionDefinition const* func;
	};
	// funcName → ordered chain of implementations in MRO order
	std::map<std::string, std::vector<MroChainEntry>> mroChains;
	for (auto const* base: mro)
		for (auto const* func: base->definedFunctions())
			if (!func->isConstructor() && func->isImplemented())
				mroChains[func->name()].push_back({base, func});

	// For each function in the MRO chain, build (callerFuncId → superSubroutineName, targetFunc).
	// This allows per-calling-context super resolution.
	// Key: caller function AST ID → (super subroutine name, target FunctionDefinition)
	std::unordered_map<int64_t, solidity::frontend::FunctionDefinition const*> superTargetFuncs;
	// Map: caller func AST ID → list of (superCallTargetId → superSubName) overrides
	m_perFuncSuperOverrides.clear();

	for (auto const& [fname, chain]: mroChains)
	{
		for (size_t i = 0; i < chain.size(); ++i)
		{
			// Check if this function has super.f() calls
			SuperCallCollector funcCollector;
			chain[i].func->body().accept(funcCollector);
			if (funcCollector.superTargetIds.empty())
				continue;

			// This function calls super → target is chain[i+1]
			if (i + 1 >= chain.size())
				continue;

			auto const* mroTarget = chain[i + 1].func;
			int64_t callerId = chain[i].func->id();

			for (int64_t superCallTargetId: funcCollector.superTargetIds)
			{
				// Create a unique super subroutine name per caller context
				std::string name = fname;
				if (m_overloadedNames.count(name))
					name += "(" + std::to_string(mroTarget->parameters().size()) + ")";
				std::string superName = name + "__super_" + std::to_string(callerId);

				// Record the override for this caller
				m_perFuncSuperOverrides[callerId].push_back({superCallTargetId, superName});

				// Record the target function for this super subroutine
				superTargetFuncs[callerId] = mroTarget;
			}
		}
	}

	// Fallback: super calls not handled by MRO chains (e.g., g() calling super.f()
	// where g and f are different function names). Use original AST-ID-based resolution.
	m_fallbackSuperFuncs.clear();
	{
		SuperCallCollector globalCollector;
		for (auto const* base: mro)
			for (auto const* func: base->definedFunctions())
				if (func->isImplemented())
					func->body().accept(globalCollector);

		// Collect all super target IDs already handled by MRO chain
		std::set<int64_t> handledSuperIds;
		for (auto const& [callerId, overrides]: m_perFuncSuperOverrides)
			for (auto const& [targetId, name]: overrides)
				handledSuperIds.insert(targetId);

		for (int64_t id: globalCollector.superTargetIds)
		{
			if (handledSuperIds.count(id))
				continue; // Already handled by MRO chain

			// Find the target function by AST ID (original resolution)
			for (auto const* base: mro)
			{
				for (auto const* func: base->definedFunctions())
				{
					if (func->id() == id && func->isImplemented())
					{
						m_fallbackSuperFuncs[id] = func;
						std::string name = func->name();
						if (m_overloadedNames.count(name))
							name += "_" + std::to_string(func->parameters().size());
						std::string superName = name + "__super_" + std::to_string(id);
						m_exprBuilder->addSuperTarget(id, superName);
					}
				}
			}
		}
	}

	// Handle explicit base class calls (Base.f() — NOT super.f()).
	// These always resolve to the specific base function, regardless of MRO.
	// Collect from ALL functions in the hierarchy.
	std::unordered_map<int64_t, solidity::frontend::FunctionDefinition const*> explicitBaseTargetFuncs;
	{
		SuperCallCollector globalCollector;
		for (auto const* base: mro)
			for (auto const* func: base->definedFunctions())
				if (func->isImplemented())
					func->body().accept(globalCollector);

		for (int64_t id: globalCollector.explicitBaseTargetIds)
		{
			for (auto const* base: mro)
			{
				for (auto const* func: base->definedFunctions())
				{
					if (func->id() == id && func->isImplemented())
					{
						explicitBaseTargetFuncs[id] = func;
						std::string name = func->name();
						if (m_overloadedNames.count(name))
							name += "(" + std::to_string(func->parameters().size()) + ")";
						std::string superName = name + "__super_" + std::to_string(id);
						// Register globally — explicit base calls don't need per-function context
						m_exprBuilder->addSuperTarget(id, superName);
					}
				}
			}
		}
	}

	// Helper: set up super target overrides before translating a function.
	// Adds MRO-dependent super targets, then re-adds explicit base + fallback targets.
	auto setupSuperOverrides = [&](int64_t funcAstId) {
		// MRO-dependent overrides for this specific function
		auto it = m_perFuncSuperOverrides.find(funcAstId);
		if (it != m_perFuncSuperOverrides.end())
			for (auto const& [targetId, superName]: it->second)
				m_exprBuilder->addSuperTarget(targetId, superName);
		// Re-register fallback super targets (cross-function super calls)
		for (auto const& [id, func]: m_fallbackSuperFuncs)
		{
			std::string name = func->name();
			if (m_overloadedNames.count(name))
				name += "_" + std::to_string(func->parameters().size());
			m_exprBuilder->addSuperTarget(id, name + "__super_" + std::to_string(id));
		}
		// Re-register explicit base targets (they're fixed, not MRO-dependent)
		for (auto const& [id, func]: explicitBaseTargetFuncs)
		{
			std::string name = func->name();
			if (m_overloadedNames.count(name))
				name += "(" + std::to_string(func->parameters().size()) + ")";
			m_exprBuilder->addSuperTarget(id, name + "__super_" + std::to_string(id));
		}
	};

	// Helper: clear super targets (to avoid cross-contamination between functions)
	auto clearSuperOverrides = [&]() {
		m_exprBuilder->clearSuperTargets();
	};

	// Snapshot all super target registrations for use by constructor body.
	// The constructor is built in buildApprovalProgram() after clearSuperTargets()
	// has been called for each function, so we save the full map here.
	m_allSuperTargetNames = m_exprBuilder->superTargetNames();

	// Translate all defined functions in this contract
	// Use "name(paramCount)" for overloaded functions to disambiguate
	std::set<std::string> translatedFunctions;
	for (auto const* func: _contract.definedFunctions())
	{
		if (func->isConstructor())
			continue;

		std::string key = func->name();
		if (m_overloadedNames.count(key))
		{
			// Use AST ID to uniquely identify each overload
			key += "#" + std::to_string(func->id());
		}
		translatedFunctions.insert(key);
		// Set up MRO-correct super targets for this function
		clearSuperOverrides();
		setupSuperOverrides(func->id());
		auto method = buildFunction(*func, contractName);
		contract->methods.push_back(std::move(method));
		// Flush modifier subroutines generated by buildModifierChain
		for (auto& sub: m_modifierSubroutines)
			contract->methods.push_back(std::move(sub));
		m_modifierSubroutines.clear();
	}

	// Auto-generate getter methods for public state variables BEFORE inheriting
	// functions. This ensures that `uint256 public override test;` takes precedence
	// over an inherited `function test()` from a base contract.
	//
	// Uses Solidity's FunctionType(VariableDeclaration) to determine the getter
	// signature: mappings add key params, arrays add uint256 index params,
	// structs return only non-mapping/non-dynamic-array fields.
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		for (auto const* var: base->stateVariables())
		{
			if (var->visibility() != solidity::frontend::Visibility::Public)
				continue;
			if (translatedFunctions.count(var->name()))
				continue; // explicit getter already exists

			// Get the Solidity-computed getter function type.
			// This gives us parameter types (mapping keys, array indices)
			// and return types (struct field filtering).
			auto getterFuncType = var->functionType(/*_internal=*/false);
			if (!getterFuncType)
				continue;

			translatedFunctions.insert(var->name());

			auto loc = makeLoc(var->location());

			awst::ContractMethod getter;
			getter.sourceLocation = loc;
			getter.cref = m_sourceFile + "." + contractName;
			getter.memberName = var->name();
			getter.pure = var->isConstant();

			awst::ARC4ABIMethodConfig config;
			config.name = var->name();
			config.sourceLocation = loc;
			config.allowedCompletionTypes = {0}; // NoOp
			config.create = 3; // Disallow
			config.readonly = true;
			getter.arc4MethodConfig = config;

			// Build getter parameters from the Solidity getter function type.
			auto const& solParamTypes = getterFuncType->parameterTypes();
			auto const solParamNames = getterFuncType->parameterNames();
			for (size_t i = 0; i < solParamTypes.size(); ++i)
			{
				awst::SubroutineArgument arg;
				std::string paramName = (i < solParamNames.size() && !solParamNames[i].empty())
					? solParamNames[i]
					: "key" + std::to_string(i);
				arg.name = paramName;
				arg.sourceLocation = loc;
				arg.wtype = m_typeMapper.map(solParamTypes[i]);
				getter.args.push_back(std::move(arg));
			}

			// Determine return type from Solidity getter return types.
			auto const& solReturnTypes = getterFuncType->returnParameterTypes();
			auto const& solReturnNames = getterFuncType->returnParameterNames();
			// Track signed return for sign-extension
			unsigned signedGetterBits = 0;
			if (solReturnTypes.size() == 1)
			{
				getter.returnType = m_typeMapper.map(solReturnTypes[0]);
				// Detect signed integer return ≤64 bits for sign-extension
				auto const* solType = solReturnTypes[0];
				if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
					solType = &udvt->underlyingType();
				if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(solType))
				{
					if (intType->isSigned() && intType->numBits() <= 64)
					{
						getter.returnType = awst::WType::biguintType();
						signedGetterBits = intType->numBits();
					}
				}
			}
			else if (solReturnTypes.size() > 1)
			{
				// Multiple return values (struct fields) — use WTuple.
				std::vector<awst::WType const*> tupleTypes;
				std::vector<std::string> tupleNames;
				for (size_t i = 0; i < solReturnTypes.size(); ++i)
				{
					tupleTypes.push_back(m_typeMapper.map(solReturnTypes[i]));
					tupleNames.push_back(i < solReturnNames.size() ? solReturnNames[i] : "");
				}
				getter.returnType = m_typeMapper.createType<awst::WTuple>(
					std::move(tupleTypes), std::move(tupleNames)
				);
			}
			else
			{
				// No return types — shouldn't happen for getters, skip.
				continue;
			}

			// Build body: return value
			auto body = std::make_shared<awst::Block>();
			body->sourceLocation = loc;

			std::shared_ptr<awst::Expression> readExpr;
			if (var->isConstant())
			{
				// Compile-time constant — return the value directly.
				if (var->value())
					readExpr = m_exprBuilder->build(*var->value());
				if (!readExpr)
					readExpr = StorageMapper::makeDefaultValue(getter.returnType, loc);
				if (readExpr && readExpr->wtype != getter.returnType)
					readExpr = ExpressionBuilder::implicitNumericCast(
						std::move(readExpr), getter.returnType, loc
					);
				// String literal → bytes[N]: right-pad to N bytes
				if (readExpr && readExpr->wtype != getter.returnType)
				{
					auto const* bytesType = dynamic_cast<awst::BytesWType const*>(getter.returnType);
					if (bytesType && bytesType->length().has_value() && *bytesType->length() > 0)
					{
						if (auto padded = TypeCoercion::stringToBytesN(
								readExpr.get(), getter.returnType, *bytesType->length(), loc))
							readExpr = std::move(padded);
					}
					else
					{
						// Generic ReinterpretCast for other bytes-compatible coercions
						bool compat = readExpr->wtype == awst::WType::stringType()
							|| (readExpr->wtype && readExpr->wtype->kind() == awst::WTypeKind::Bytes);
						if (compat)
						{
							auto cast = std::make_shared<awst::ReinterpretCast>();
							cast->sourceLocation = loc;
							cast->wtype = getter.returnType;
							cast->expr = std::move(readExpr);
							readExpr = std::move(cast);
						}
					}
				}
			}
			else if (getter.args.empty())
			{
				// Simple state variable (no mapping/array params) — read from storage.
				auto storageKind = StorageMapper::shouldUseBoxStorage(*var)
					? awst::AppStorageKind::Box
					: awst::AppStorageKind::AppGlobal;

				// For struct types with multiple return values, read the full
				// ARC4Struct and extract/decode each returned field.
				auto const* solStructType = dynamic_cast<solidity::frontend::StructType const*>(var->type());
				if (solStructType && solReturnTypes.size() > 1)
				{
					auto* storedWType = m_typeMapper.map(var->type());
					auto fullStruct = m_storageMapper.createStateRead(
						var->name(), storedWType, storageKind, loc
					);

					auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(storedWType);
					auto tuple = std::make_shared<awst::TupleExpression>();
					tuple->sourceLocation = loc;
					tuple->wtype = getter.returnType;

					for (auto const& member: solStructType->members(nullptr))
					{
						if (member.type->category() == solidity::frontend::Type::Category::Mapping)
							continue;
						if (auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(member.type))
							if (!at->isByteArrayOrString())
								continue;

						awst::WType const* arc4FieldType = nullptr;
						if (arc4Struct)
							for (auto const& [fname, ftype]: arc4Struct->fields())
								if (fname == member.name)
								{
									arc4FieldType = ftype;
									break;
								}

						auto fieldExpr = std::make_shared<awst::FieldExpression>();
						fieldExpr->sourceLocation = loc;
						fieldExpr->wtype = arc4FieldType ? arc4FieldType : m_typeMapper.map(member.type);
						fieldExpr->base = fullStruct;
						fieldExpr->name = member.name;

						auto* nativeType = m_typeMapper.map(member.type);
						if (arc4FieldType && arc4FieldType != nativeType)
						{
							auto decode = std::make_shared<awst::ARC4Decode>();
							decode->sourceLocation = loc;
							decode->wtype = nativeType;
							decode->value = std::move(fieldExpr);
							tuple->items.push_back(std::move(decode));
						}
						else
							tuple->items.push_back(std::move(fieldExpr));
					}
					readExpr = std::move(tuple);
				}
				else
				{
					// Read with original storage type (not promoted return type)
					auto* readType = signedGetterBits > 0
						? m_typeMapper.map(var->type()) : getter.returnType;
					readExpr = m_storageMapper.createStateRead(
						var->name(), readType, storageKind, loc
					);
				}
			}
			else
			{
				// Mapping/array getter — build box read with key from arguments.
				// Unwound value type: walk past mappings and arrays to get the stored type.
				solidity::frontend::Type const* valueType = var->type();
				size_t paramIdx = 0;
				while (paramIdx < getter.args.size())
				{
					if (auto const* mt = dynamic_cast<solidity::frontend::MappingType const*>(valueType))
					{
						valueType = mt->valueType();
						paramIdx++;
					}
					else if (auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(valueType))
					{
						if (at->isByteArrayOrString())
							break;
						valueType = at->baseType();
						paramIdx++;
					}
					else
						break;
				}

				// Map the unwound value type (may be a struct, in which case we need
				// the full stored type, not the getter return type).
				awst::WType const* storedWType = m_typeMapper.map(valueType);

				// Build the box key from the getter arguments.
				// Each arg is converted to bytes, concatenated, then sha256-hashed.
				auto prefix = std::make_shared<awst::BytesConstant>();
				prefix->sourceLocation = loc;
				prefix->wtype = awst::WType::boxKeyType();
				prefix->encoding = awst::BytesEncoding::Utf8;
				std::string varName = var->name();
				prefix->value = std::vector<uint8_t>(varName.begin(), varName.end());

				std::vector<std::shared_ptr<awst::Expression>> keyParts;
				for (size_t i = 0; i < getter.args.size(); ++i)
				{
					auto argRef = std::make_shared<awst::VarExpression>();
					argRef->sourceLocation = loc;
					argRef->wtype = getter.args[i].wtype;
					argRef->name = getter.args[i].name;

					std::shared_ptr<awst::Expression> keyBytes;
					if (argRef->wtype == awst::WType::uint64Type())
					{
						auto itob = std::make_shared<awst::IntrinsicCall>();
						itob->sourceLocation = loc;
						itob->wtype = awst::WType::bytesType();
						itob->opCode = "itob";
						itob->stackArgs.push_back(std::move(argRef));
						keyBytes = std::move(itob);
					}
					else if (argRef->wtype == awst::WType::biguintType())
					{
						// Normalize biguint to exactly 32 bytes before hashing.
						auto reinterpret = std::make_shared<awst::ReinterpretCast>();
						reinterpret->sourceLocation = loc;
						reinterpret->wtype = awst::WType::bytesType();
						reinterpret->expr = std::move(argRef);

						auto padWidth = std::make_shared<awst::IntegerConstant>();
						padWidth->sourceLocation = loc;
						padWidth->wtype = awst::WType::uint64Type();
						padWidth->value = "32";

						auto pad = std::make_shared<awst::IntrinsicCall>();
						pad->sourceLocation = loc;
						pad->wtype = awst::WType::bytesType();
						pad->opCode = "bzero";
						pad->stackArgs.push_back(std::move(padWidth));

						auto cat = std::make_shared<awst::IntrinsicCall>();
						cat->sourceLocation = loc;
						cat->wtype = awst::WType::bytesType();
						cat->opCode = "concat";
						cat->stackArgs.push_back(std::move(pad));
						cat->stackArgs.push_back(std::move(reinterpret));

						auto lenCall = std::make_shared<awst::IntrinsicCall>();
						lenCall->sourceLocation = loc;
						lenCall->wtype = awst::WType::uint64Type();
						lenCall->opCode = "len";
						lenCall->stackArgs.push_back(cat);

						auto width32 = std::make_shared<awst::IntegerConstant>();
						width32->sourceLocation = loc;
						width32->wtype = awst::WType::uint64Type();
						width32->value = "32";

						auto offset = std::make_shared<awst::IntrinsicCall>();
						offset->sourceLocation = loc;
						offset->wtype = awst::WType::uint64Type();
						offset->opCode = "-";
						offset->stackArgs.push_back(std::move(lenCall));
						offset->stackArgs.push_back(std::move(width32));

						auto width32b = std::make_shared<awst::IntegerConstant>();
						width32b->sourceLocation = loc;
						width32b->wtype = awst::WType::uint64Type();
						width32b->value = "32";

						auto extract = std::make_shared<awst::IntrinsicCall>();
						extract->sourceLocation = loc;
						extract->wtype = awst::WType::bytesType();
						extract->opCode = "extract3";
						extract->stackArgs.push_back(std::move(cat));
						extract->stackArgs.push_back(std::move(offset));
						extract->stackArgs.push_back(std::move(width32b));

						keyBytes = std::move(extract);
					}
					else
					{
						// string / bytes / address → ReinterpretCast to bytes
						auto reinterpret = std::make_shared<awst::ReinterpretCast>();
						reinterpret->sourceLocation = loc;
						reinterpret->wtype = awst::WType::bytesType();
						reinterpret->expr = std::move(argRef);
						keyBytes = std::move(reinterpret);
					}
					keyParts.push_back(std::move(keyBytes));
				}

				// Concatenate key parts
				std::shared_ptr<awst::Expression> compositeKey;
				if (keyParts.size() == 1)
					compositeKey = std::move(keyParts[0]);
				else
				{
					compositeKey = std::move(keyParts[0]);
					for (size_t i = 1; i < keyParts.size(); ++i)
					{
						auto concat = std::make_shared<awst::IntrinsicCall>();
						concat->sourceLocation = loc;
						concat->wtype = awst::WType::bytesType();
						concat->opCode = "concat";
						concat->stackArgs.push_back(std::move(compositeKey));
						concat->stackArgs.push_back(std::move(keyParts[i]));
						compositeKey = std::move(concat);
					}
				}

				// Hash the composite key
				auto hashCall = std::make_shared<awst::IntrinsicCall>();
				hashCall->sourceLocation = loc;
				hashCall->wtype = awst::WType::bytesType();
				hashCall->opCode = "sha256";
				hashCall->stackArgs.push_back(std::move(compositeKey));

				auto boxKey = std::make_shared<awst::BoxPrefixedKeyExpression>();
				boxKey->sourceLocation = loc;
				boxKey->wtype = awst::WType::boxKeyType();
				boxKey->prefix = prefix;
				boxKey->key = std::move(hashCall);

				auto boxExpr = std::make_shared<awst::BoxValueExpression>();
				boxExpr->sourceLocation = loc;
				boxExpr->wtype = storedWType;
				boxExpr->key = std::move(boxKey);

				auto defaultVal = StorageMapper::makeDefaultValue(storedWType, loc);

				auto stateGet = std::make_shared<awst::StateGet>();
				stateGet->sourceLocation = loc;
				stateGet->wtype = storedWType;
				stateGet->field = std::move(boxExpr);
				stateGet->defaultValue = std::move(defaultVal);

				// If the stored type is a struct but the getter returns a tuple
				// of selected fields, extract and ARC4-decode each field.
				if (auto const* structType = dynamic_cast<solidity::frontend::StructType const*>(valueType))
				{
					if (solReturnTypes.size() > 1)
					{
						// stateGet returns the full ARC4Struct; extract fields.
						std::shared_ptr<awst::Expression> fullStruct = std::move(stateGet);
						auto tuple = std::make_shared<awst::TupleExpression>();
						tuple->sourceLocation = loc;
						tuple->wtype = getter.returnType;

						// Get the ARC4Struct type's field types for FieldExpression
						auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(storedWType);

						for (auto const& member: structType->members(nullptr))
						{
							if (member.type->category() == solidity::frontend::Type::Category::Mapping)
								continue;
							if (auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(member.type))
								if (!at->isByteArrayOrString())
									continue;

							// Look up the ARC4 field type from the struct type
							awst::WType const* arc4FieldType = nullptr;
							if (arc4Struct)
								for (auto const& [fname, ftype]: arc4Struct->fields())
									if (fname == member.name)
									{
										arc4FieldType = ftype;
										break;
									}

							auto fieldExpr = std::make_shared<awst::FieldExpression>();
							fieldExpr->sourceLocation = loc;
							fieldExpr->wtype = arc4FieldType ? arc4FieldType : m_typeMapper.map(member.type);
							fieldExpr->base = fullStruct;
							fieldExpr->name = member.name;

							// ARC4Decode to native type if needed
							auto* nativeType = m_typeMapper.map(member.type);
							if (arc4FieldType && arc4FieldType != nativeType)
							{
								auto decode = std::make_shared<awst::ARC4Decode>();
								decode->sourceLocation = loc;
								decode->wtype = nativeType;
								decode->value = std::move(fieldExpr);
								tuple->items.push_back(std::move(decode));
							}
							else
								tuple->items.push_back(std::move(fieldExpr));
						}
						readExpr = std::move(tuple);
					}
					else
					{
						readExpr = std::move(stateGet);
					}
				}
				else
				{
					readExpr = std::move(stateGet);
				}
			}

			// Sign-extend getter return for signed integer types
			if (signedGetterBits > 0 && readExpr)
			{
				readExpr = signExtendToUint256(std::move(readExpr), signedGetterBits, loc);
			}

			auto ret = std::make_shared<awst::ReturnStatement>();
			ret->sourceLocation = loc;
			ret->value = std::move(readExpr);
			body->body.push_back(std::move(ret));

			getter.body = body;
			contract->methods.push_back(std::move(getter));
		}
	}

	// Include inherited functions that may be needed
	// (e.g. _checkOwner from Ownable, owner() from Ownable).
	// This runs AFTER public state variable getters so that `uint256 public override x`
	// takes precedence over an inherited `function x()` from a base contract.
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		if (base == &_contract)
			continue; // Already handled above

		for (auto const* func: base->definedFunctions())
		{
			if (func->isConstructor())
				continue;

			std::string key = func->name();
			if (m_overloadedNames.count(key))
				key += "#" + std::to_string(func->id());
			if (translatedFunctions.count(key))
				continue;

			if (!func->isImplemented())
				continue;

			translatedFunctions.insert(key);
			// Set up MRO-correct super targets for this inherited function
			clearSuperOverrides();
			setupSuperOverrides(func->id());
			auto method = buildFunction(*func, contractName);
			contract->methods.push_back(std::move(method));
			for (auto& sub: m_modifierSubroutines)
				contract->methods.push_back(std::move(sub));
			m_modifierSubroutines.clear();
		}
	}

	// Clear super overrides before emitting super subroutines
	clearSuperOverrides();

	// Emit MRO-dependent super subroutines (keyed by caller func AST ID)
	for (auto const& [callerFuncId, targetFunc]: superTargetFuncs)
	{
		std::string name = targetFunc->name();
		if (m_overloadedNames.count(name))
			name += "(" + std::to_string(targetFunc->parameters().size()) + ")";
		std::string superName = name + "__super_" + std::to_string(callerFuncId);
		// Set up super targets for the target function too (it may chain further)
		clearSuperOverrides();
		setupSuperOverrides(targetFunc->id());
		auto method = buildFunction(*targetFunc, contractName, superName);
		method.arc4MethodConfig.reset();
		contract->methods.push_back(std::move(method));
	}

	// Emit fallback super subroutines (cross-function super calls)
	for (auto const& [targetId, func]: m_fallbackSuperFuncs)
	{
		std::string name = func->name();
		if (m_overloadedNames.count(name))
			name += "_" + std::to_string(func->parameters().size());
		std::string superName = name + "__super_" + std::to_string(targetId);
		clearSuperOverrides();
		auto method = buildFunction(*func, contractName, superName);
		method.arc4MethodConfig.reset();
		contract->methods.push_back(std::move(method));
	}

	// Emit explicit base class call subroutines (keyed by target func AST ID)
	for (auto const& [targetId, func]: explicitBaseTargetFuncs)
	{
		std::string name = func->name();
		if (m_overloadedNames.count(name))
			name += "(" + std::to_string(func->parameters().size()) + ")";
		std::string superName = name + "__super_" + std::to_string(targetId);
		clearSuperOverrides();
		auto method = buildFunction(*func, contractName, superName);
		method.arc4MethodConfig.reset();
		contract->methods.push_back(std::move(method));
	}

	// Generate __storage_read/__storage_write dispatch subroutines
	// for assembly sload/sstore support
	buildStorageDispatch(_contract, contract.get(), contractName);

	return contract;
}

awst::ContractMethod ContractBuilder::buildApprovalProgram(
	solidity::frontend::ContractDefinition const& _contract,
	std::string const& _contractName
)
{
	awst::ContractMethod method;
	method.sourceLocation = makeLoc(_contract.location());
	method.returnType = awst::WType::boolType();
	method.cref = m_sourceFile + "." + _contractName;
	method.memberName = "approval_program";

	auto body = std::make_shared<awst::Block>();
	body->sourceLocation = method.sourceLocation;

	// Detect if constructor needs auto-split (box writes in constructor)
	// Only generate __postInit if the constructor body actually references
	// box-stored state variables. Having box storage + constructor code is
	// not sufficient — if the constructor only writes global state, it can
	// all happen during the create transaction.
	bool needsPostInit = false;
	{
		// Collect AST IDs of all box-stored state variables
		std::set<int64_t> boxVarIds;
		for (auto const* base: _contract.annotation().linearizedBaseContracts)
		{
			for (auto const* var: base->stateVariables())
			{
				if (var->isConstant())
					continue;
				if (StorageMapper::shouldUseBoxStorage(*var))
					boxVarIds.insert(var->id());
			}
		}

		if (!boxVarIds.empty())
		{
			// Walk constructor bodies to check if they reference any box-stored variable.
			// Also check functions called from constructors (transitively).
			BoxVarRefChecker checker(boxVarIds);

			// First, scan ALL non-constructor functions to find which ones
			// reference box-stored state variables
			std::set<int64_t> boxWriteFuncIds;
			for (auto const* base: _contract.annotation().linearizedBaseContracts)
			{
				for (auto const* func: base->definedFunctions())
				{
					if (func->isConstructor() || !func->isImplemented())
						continue;
					BoxVarRefChecker funcChecker(boxVarIds);
					func->body().accept(funcChecker);
					if (funcChecker.found())
						boxWriteFuncIds.insert(func->id());
				}
			}

			// Now walk constructor bodies checking for:
			// 1. Direct references to box-stored state variables
			// 2. Calls to functions that reference box-stored state variables
			auto const* ctor = _contract.constructor();
			if (ctor && !ctor->body().statements().empty())
				ctor->body().accept(checker);

			if (!checker.found())
			{
				for (auto const* base: _contract.annotation().linearizedBaseContracts)
				{
					if (base == &_contract)
						continue;
					auto const* baseCtor = base->constructor();
					if (baseCtor && baseCtor->isImplemented()
						&& !baseCtor->body().statements().empty())
					{
						baseCtor->body().accept(checker);
						if (checker.found())
							break;
					}
				}
			}

			// If direct references weren't found, check if constructors
			// call any function that writes to boxes
			if (!checker.found() && !boxWriteFuncIds.empty())
			{
				// Scan constructor bodies for FunctionCall nodes whose
				// referenced declaration is in boxWriteFuncIds
				struct CtorCallChecker: public solidity::frontend::ASTConstVisitor
				{
					std::set<int64_t> const& targetIds;
					bool found = false;
					explicit CtorCallChecker(std::set<int64_t> const& _ids): targetIds(_ids) {}
					bool visit(solidity::frontend::FunctionCall const& _node) override
					{
						if (found) return false;
						auto const* expr = &_node.expression();
						// Unwrap MemberAccess for calls like _grantRole(...)
						if (auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(expr))
						{
							auto const* decl = id->annotation().referencedDeclaration;
							if (decl && targetIds.count(decl->id()))
								found = true;
						}
						return !found;
					}
				};
				CtorCallChecker callChecker(boxWriteFuncIds);
				if (ctor && !ctor->body().statements().empty())
					ctor->body().accept(callChecker);
				if (!callChecker.found)
				{
					for (auto const* base: _contract.annotation().linearizedBaseContracts)
					{
						if (base == &_contract)
							continue;
						auto const* baseCtor = base->constructor();
						if (baseCtor && baseCtor->isImplemented()
							&& !baseCtor->body().statements().empty())
						{
							baseCtor->body().accept(callChecker);
							if (callChecker.found)
								break;
						}
					}
				}
				needsPostInit = callChecker.found;
			}
			else
			{
				needsPostInit = checker.found();
			}
		}
	}

	// Create-time check: if (Txn.ApplicationID == 0) { base_ctors; ctor_body; return true; }
	{
		auto appIdCheck = std::make_shared<awst::IntrinsicCall>();
		appIdCheck->sourceLocation = method.sourceLocation;
		appIdCheck->opCode = "txn";
		appIdCheck->immediates = {std::string("ApplicationID")};
		appIdCheck->wtype = awst::WType::uint64Type();

		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = method.sourceLocation;
		zero->wtype = awst::WType::uint64Type();
		zero->value = "0";

		auto isCreate = std::make_shared<awst::NumericComparisonExpression>();
		isCreate->sourceLocation = method.sourceLocation;
		isCreate->wtype = awst::WType::boolType();
		isCreate->lhs = appIdCheck;
		isCreate->op = awst::NumericComparison::Eq;
		isCreate->rhs = zero;

		auto createBlock = std::make_shared<awst::Block>();
		createBlock->sourceLocation = method.sourceLocation;

		// Helper: emit state variable initialization statements for one contract's state vars.
		// Initializes global state variables with explicit initializers or zero/default values.
		// Tracks already-initialized variable names via the 'initialized' set to handle overrides.
		std::set<std::string> stateVarInitialized;
		auto emitStateVarInit = [&](solidity::frontend::ContractDefinition const& base,
			std::vector<std::shared_ptr<awst::Statement>>& targetBody)
		{
			for (auto const* var: base.stateVariables())
			{
				if (var->isConstant())
					continue;
				if (stateVarInitialized.count(var->name()))
					continue;
				stateVarInitialized.insert(var->name());

				auto kind = StorageMapper::shouldUseBoxStorage(*var)
					? awst::AppStorageKind::Box
					: awst::AppStorageKind::AppGlobal;

				// Only zero-initialize global state (not box storage)
				if (kind != awst::AppStorageKind::AppGlobal)
					continue;

				auto* wtype = m_typeMapper.map(var->type());

				// Build key
				auto key = std::make_shared<awst::BytesConstant>();
				key->sourceLocation = method.sourceLocation;
				key->wtype = awst::WType::bytesType();
				key->encoding = awst::BytesEncoding::Utf8;
				std::string keyStr = var->name();
				key->value = std::vector<uint8_t>(keyStr.begin(), keyStr.end());

				// Build initial value: use explicit initializer if present,
				// otherwise default to zero/empty.
				std::shared_ptr<awst::Expression> defaultVal;
				if (var->value())
				{
					// Translate the initializer expression (e.g. `= 'Wrapped Ether'`)
					defaultVal = m_exprBuilder->build(*var->value());
					if (defaultVal)
						defaultVal = TypeCoercion::coerceForAssignment(
							std::move(defaultVal), wtype, method.sourceLocation);
				}
				if (!defaultVal)
				{
				if (wtype == awst::WType::accountType())
				{
					auto addr = std::make_shared<awst::AddressConstant>();
					addr->sourceLocation = method.sourceLocation;
					addr->wtype = awst::WType::accountType();
					addr->value = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ";
					defaultVal = addr;
				}
				else if (wtype == awst::WType::biguintType())
				{
					auto val = std::make_shared<awst::IntegerConstant>();
					val->sourceLocation = method.sourceLocation;
					val->wtype = awst::WType::biguintType();
					val->value = "0";
					defaultVal = val;
				}
				else if (wtype == awst::WType::boolType()
					|| wtype == awst::WType::uint64Type())
				{
					auto val = std::make_shared<awst::IntegerConstant>();
					val->sourceLocation = method.sourceLocation;
					val->wtype = awst::WType::uint64Type();
					val->value = "0";
					defaultVal = val;
				}
				else if (wtype->kind() == awst::WTypeKind::ReferenceArray)
				{
					// Fixed-size array → NewArray with default elements
					auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(wtype);
					auto arr = std::make_shared<awst::NewArray>();
					arr->sourceLocation = method.sourceLocation;
					arr->wtype = wtype;
					if (refArr && refArr->arraySize())
					{
						for (int i = 0; i < *refArr->arraySize(); ++i)
							arr->values.push_back(
								StorageMapper::makeDefaultValue(refArr->elementType(), method.sourceLocation));
					}
					defaultVal = arr;
				}
				else if (wtype->kind() == awst::WTypeKind::ARC4Struct
					|| wtype->kind() == awst::WTypeKind::WTuple)
				{
					// Struct → use StorageMapper's default
					defaultVal = StorageMapper::makeDefaultValue(wtype, method.sourceLocation);
				}
				else
				{
					// bytes or other → empty bytes
					auto val = std::make_shared<awst::BytesConstant>();
					val->sourceLocation = method.sourceLocation;
					val->wtype = awst::WType::bytesType();
					val->encoding = awst::BytesEncoding::Base16;
					val->value = {};
					defaultVal = val;
				}
				} // end if (!defaultVal)

				// app_global_put(key, defaultVal)
				auto put = std::make_shared<awst::IntrinsicCall>();
				put->sourceLocation = method.sourceLocation;
				put->opCode = "app_global_put";
				put->wtype = awst::WType::voidType();
				put->stackArgs.push_back(key);
				put->stackArgs.push_back(defaultVal);

				auto stmt = std::make_shared<awst::ExpressionStatement>();
				stmt->sourceLocation = method.sourceLocation;
				stmt->expr = put;
				targetBody.push_back(stmt);
			}
		};

		// Initialize length counters for dynamic array state variables stored in boxes
		{
			auto const& linearized = _contract.annotation().linearizedBaseContracts;
			std::set<std::string> lengthInitialized;
			for (auto it = linearized.rbegin(); it != linearized.rend(); ++it)
			{
				for (auto const* var: (*it)->stateVariables())
				{
					if (var->isConstant())
						continue;
					if (lengthInitialized.count(var->name()))
						continue;

					auto kind = StorageMapper::shouldUseBoxStorage(*var)
						? awst::AppStorageKind::Box
						: awst::AppStorageKind::AppGlobal;

					// Only for box-stored arrays (dynamic arrays)
					if (kind != awst::AppStorageKind::Box)
						continue;

					auto* wtype = m_typeMapper.map(var->type());
					if (!wtype || wtype->kind() != awst::WTypeKind::ReferenceArray)
						continue;

					lengthInitialized.insert(var->name());
					// Dynamic array boxes are created in __postInit (after funding)
					// Length is derived from box_len / element_size (no separate counter)
					m_boxArrayVarNames.push_back(var->name());
				}
			}
		}

		// Force __postInit if we have box array vars that need box_create
		if (!m_boxArrayVarNames.empty())
			needsPostInit = true;

		// Collect explicit base constructor calls from the constructor's modifiers
		auto const* constructor = _contract.constructor();
		std::map<solidity::frontend::ContractDefinition const*,
			std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression>> const*>
			explicitBaseArgs;

		if (constructor)
		{
			// Read constructor parameters from ApplicationArgs during create.
			// Each param is ARC4-encoded in ApplicationArgs[i].
			// For contracts with no constructor params, this loop is skipped.
			int argIndex = 0;
			for (auto const& param: constructor->parameters())
			{
				auto* paramType = m_typeMapper.map(param->type());

				// txna ApplicationArgs i → raw ARC4 bytes
				auto readArg = std::make_shared<awst::IntrinsicCall>();
				readArg->sourceLocation = method.sourceLocation;
				readArg->opCode = "txna";
				readArg->immediates = {std::string("ApplicationArgs"), argIndex};
				readArg->wtype = awst::WType::bytesType();

				std::shared_ptr<awst::Expression> paramVal;

				if (paramType == awst::WType::accountType())
				{
					// bytes → account via ReinterpretCast
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = method.sourceLocation;
					cast->wtype = awst::WType::accountType();
					cast->expr = std::move(readArg);
					paramVal = std::move(cast);
				}
				else if (paramType == awst::WType::biguintType())
				{
					// bytes → biguint via ReinterpretCast (big-endian, no-op on AVM)
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = method.sourceLocation;
					cast->wtype = awst::WType::biguintType();
					cast->expr = std::move(readArg);
					paramVal = std::move(cast);
				}
				else if (paramType == awst::WType::uint64Type()
					|| paramType == awst::WType::boolType())
				{
					// ARC4 uint64 is 8-byte big-endian → btoi to native uint64
					auto btoi = std::make_shared<awst::IntrinsicCall>();
					btoi->sourceLocation = method.sourceLocation;
					btoi->opCode = "btoi";
					btoi->wtype = awst::WType::uint64Type();
					btoi->stackArgs.push_back(std::move(readArg));
					paramVal = std::move(btoi);
				}
				else if (paramType == awst::WType::stringType())
				{
					// bytes → string via ReinterpretCast
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = method.sourceLocation;
					cast->wtype = awst::WType::stringType();
					cast->expr = std::move(readArg);
					paramVal = std::move(cast);
				}
				else
				{
					// bytes, etc. → use raw bytes directly
					paramVal = std::move(readArg);
				}

				auto target = std::make_shared<awst::VarExpression>();
				target->sourceLocation = method.sourceLocation;
				target->name = param->name();
				target->wtype = paramType;

				auto assignment = std::make_shared<awst::AssignmentStatement>();
				assignment->sourceLocation = method.sourceLocation;
				assignment->target = target;
				assignment->value = std::move(paramVal);
				createBlock->body.push_back(std::move(assignment));

				++argIndex;
			}

			for (auto const& mod: constructor->modifiers())
			{
				auto const* refDecl = mod->name().annotation().referencedDeclaration;
				if (auto const* baseContract =
						dynamic_cast<solidity::frontend::ContractDefinition const*>(refDecl))
				{
					explicitBaseArgs[baseContract] = mod->arguments();
				}
			}
		}

		// Also collect arguments from inheritance specifiers (e.g. `is Base(arg1, arg2)`)
		for (auto const& baseSpec: _contract.baseContracts())
		{
			auto const* refDecl = baseSpec->name().annotation().referencedDeclaration;
			auto const* baseContract =
				dynamic_cast<solidity::frontend::ContractDefinition const*>(refDecl);
			if (baseContract && baseSpec->arguments()
				&& !baseSpec->arguments()->empty()
				&& explicitBaseArgs.find(baseContract) == explicitBaseArgs.end())
			{
				explicitBaseArgs[baseContract] = baseSpec->arguments();
			}
		}

		// Collect transitive base constructor args from intermediate base contracts.
		// e.g. if ConfigPositionManager → PositionManagerBase(x) → Ownable(x),
		// we need explicitBaseArgs[Ownable] from PositionManagerBase's constructor.
		for (auto const* base: _contract.annotation().linearizedBaseContracts)
		{
			if (base == &_contract)
				continue;
			// Check base's constructor modifiers
			if (auto const* baseCtor = base->constructor())
			{
				for (auto const& mod: baseCtor->modifiers())
				{
					auto const* ref = mod->name().annotation().referencedDeclaration;
					if (auto const* grandBase =
							dynamic_cast<solidity::frontend::ContractDefinition const*>(ref))
					{
						if (explicitBaseArgs.find(grandBase) == explicitBaseArgs.end()
							&& mod->arguments() && !mod->arguments()->empty())
						{
							explicitBaseArgs[grandBase] = mod->arguments();
						}
					}
				}
			}
			// Check base's inheritance specifiers
			for (auto const& baseSpec: base->baseContracts())
			{
				auto const* ref = baseSpec->name().annotation().referencedDeclaration;
				auto const* grandBase =
					dynamic_cast<solidity::frontend::ContractDefinition const*>(ref);
				if (grandBase && baseSpec->arguments()
					&& !baseSpec->arguments()->empty()
					&& explicitBaseArgs.find(grandBase) == explicitBaseArgs.end())
				{
					explicitBaseArgs[grandBase] = baseSpec->arguments();
				}
			}
		}

		if (needsPostInit)
		{
			// For postInit path, initialize all state vars upfront in create block.
			// (Interleaving with ctor bodies isn't possible since ctors run in __postInit.)
			{
				auto const& linearized = _contract.annotation().linearizedBaseContracts;
				for (auto it = linearized.rbegin(); it != linearized.rend(); ++it)
					emitStateVarInit(**it, createBlock->body);
			}

			// Constructor writes to box storage — defer constructor body to __postInit().
			// Set __ctor_pending = 1 in create block.
			auto pendingKey = std::make_shared<awst::BytesConstant>();
			pendingKey->sourceLocation = method.sourceLocation;
			pendingKey->wtype = awst::WType::bytesType();
			pendingKey->encoding = awst::BytesEncoding::Utf8;
			std::string pendingKeyStr = "__ctor_pending";
			pendingKey->value = std::vector<uint8_t>(pendingKeyStr.begin(), pendingKeyStr.end());

			auto one = std::make_shared<awst::IntegerConstant>();
			one->sourceLocation = method.sourceLocation;
			one->wtype = awst::WType::uint64Type();
			one->value = "1";

			auto setPending = std::make_shared<awst::IntrinsicCall>();
			setPending->sourceLocation = method.sourceLocation;
			setPending->opCode = "app_global_put";
			setPending->wtype = awst::WType::voidType();
			setPending->stackArgs.push_back(pendingKey);
			setPending->stackArgs.push_back(one);

			auto setPendingStmt = std::make_shared<awst::ExpressionStatement>();
			setPendingStmt->sourceLocation = method.sourceLocation;
			setPendingStmt->expr = setPending;
			createBlock->body.push_back(std::move(setPendingStmt));

			// Build __postInit method with deferred constructor body
			awst::ContractMethod postInit;
			postInit.sourceLocation = method.sourceLocation;
			postInit.returnType = awst::WType::voidType();
			postInit.cref = m_sourceFile + "." + _contractName;
			postInit.memberName = "__postInit";

			// Add constructor parameters as __postInit method arguments.
			// This allows the caller to pass the same values when calling __postInit
			// that were originally passed to the constructor.
			if (constructor)
			{
				int paramIdx = 0;
				for (auto const& param: constructor->parameters())
				{
					awst::SubroutineArgument arg;
					arg.name = param->name().empty()
						? "_param" + std::to_string(paramIdx)
						: param->name();
					arg.sourceLocation = method.sourceLocation;
					arg.wtype = m_typeMapper.map(param->type());
					postInit.args.push_back(std::move(arg));
					++paramIdx;
				}
			}

			awst::ARC4ABIMethodConfig postInitConfig;
			postInitConfig.name = "__postInit";
			postInitConfig.sourceLocation = method.sourceLocation;
			postInitConfig.allowedCompletionTypes = {0}; // NoOp
			postInitConfig.create = 3; // Disallow
			postInitConfig.readonly = false;
			postInit.arc4MethodConfig = postInitConfig;

			// Remap aggregate types (arrays, tuples) to ARC4 encoding for __postInit args
			for (auto& arg: postInit.args)
			{
				bool isAggregate = arg.wtype
					&& (arg.wtype->kind() == awst::WTypeKind::ReferenceArray
						|| arg.wtype->kind() == awst::WTypeKind::WTuple);
				if (isAggregate)
				{
					awst::WType const* arc4Type = m_typeMapper.mapToARC4Type(arg.wtype);
					if (arc4Type != arg.wtype)
						arg.wtype = arc4Type;
				}
			}

			// Set function context so constructor body can reference params by name
			{
				std::vector<std::pair<std::string, awst::WType const*>> paramContext;
				for (auto const& arg: postInit.args)
					paramContext.emplace_back(arg.name, arg.wtype);
				setFunctionContext(paramContext, postInit.returnType);
			}

			auto postInitBody = std::make_shared<awst::Block>();
			postInitBody->sourceLocation = method.sourceLocation;

			// Guard: assert(__ctor_pending == 1)
			auto readPending = std::make_shared<awst::IntrinsicCall>();
			readPending->sourceLocation = method.sourceLocation;
			readPending->opCode = "app_global_get";
			readPending->wtype = awst::WType::uint64Type();
			auto readKey = std::make_shared<awst::BytesConstant>();
			readKey->sourceLocation = method.sourceLocation;
			readKey->wtype = awst::WType::bytesType();
			readKey->encoding = awst::BytesEncoding::Utf8;
			readKey->value = std::vector<uint8_t>(pendingKeyStr.begin(), pendingKeyStr.end());
			readPending->stackArgs.push_back(readKey);

			auto assertPending = std::make_shared<awst::AssertExpression>();
			assertPending->sourceLocation = method.sourceLocation;
			assertPending->wtype = awst::WType::voidType();
			assertPending->condition = readPending;
			assertPending->errorMessage = "__postInit already called";
			auto assertStmt = std::make_shared<awst::ExpressionStatement>();
			assertStmt->sourceLocation = method.sourceLocation;
			assertStmt->expr = assertPending;
			postInitBody->body.push_back(std::move(assertStmt));

			// Clear flag: __ctor_pending = 0
			auto clearKey = std::make_shared<awst::BytesConstant>();
			clearKey->sourceLocation = method.sourceLocation;
			clearKey->wtype = awst::WType::bytesType();
			clearKey->encoding = awst::BytesEncoding::Utf8;
			clearKey->value = std::vector<uint8_t>(pendingKeyStr.begin(), pendingKeyStr.end());

			auto zeroVal = std::make_shared<awst::IntegerConstant>();
			zeroVal->sourceLocation = method.sourceLocation;
			zeroVal->wtype = awst::WType::uint64Type();
			zeroVal->value = "0";

			auto clearPending = std::make_shared<awst::IntrinsicCall>();
			clearPending->sourceLocation = method.sourceLocation;
			clearPending->opCode = "app_global_put";
			clearPending->wtype = awst::WType::voidType();
			clearPending->stackArgs.push_back(clearKey);
			clearPending->stackArgs.push_back(zeroVal);

			auto clearStmt = std::make_shared<awst::ExpressionStatement>();
			clearStmt->sourceLocation = method.sourceLocation;
			clearStmt->expr = clearPending;
			postInitBody->body.push_back(std::move(clearStmt));

			// Create boxes for dynamic array state variables
			for (auto const& varName: m_boxArrayVarNames)
			{
				auto boxKey = std::make_shared<awst::BytesConstant>();
				boxKey->sourceLocation = method.sourceLocation;
				boxKey->wtype = awst::WType::bytesType();
				boxKey->encoding = awst::BytesEncoding::Utf8;
				boxKey->value = std::vector<uint8_t>(varName.begin(), varName.end());

				auto boxSize = std::make_shared<awst::IntegerConstant>();
				boxSize->sourceLocation = method.sourceLocation;
				boxSize->wtype = awst::WType::uint64Type();
				boxSize->value = "0";

				auto boxCreate = std::make_shared<awst::IntrinsicCall>();
				boxCreate->sourceLocation = method.sourceLocation;
				boxCreate->opCode = "box_create";
				boxCreate->wtype = awst::WType::boolType();
				boxCreate->stackArgs.push_back(std::move(boxKey));
				boxCreate->stackArgs.push_back(std::move(boxSize));

				auto boxStmt = std::make_shared<awst::ExpressionStatement>();
				boxStmt->sourceLocation = method.sourceLocation;
				boxStmt->expr = std::move(boxCreate);
				postInitBody->body.push_back(std::move(boxStmt));
			}

			// Inline base constructor bodies into __postInit
			auto const& linearized = _contract.annotation().linearizedBaseContracts;
			for (auto it = linearized.rbegin(); it != linearized.rend(); ++it)
			{
				auto const* base = *it;
				if (base == &_contract)
					continue;

				auto const* baseCtor = base->constructor();
				if (!baseCtor || !baseCtor->isImplemented())
					continue;
				if (baseCtor->body().statements().empty())
					continue;

				// Base constructor parameter assignments
				auto argIt = explicitBaseArgs.find(base);
				if (argIt != explicitBaseArgs.end() && argIt->second && !argIt->second->empty())
				{
					auto const& args = *(argIt->second);
					auto const& params = baseCtor->parameters();
					for (size_t i = 0; i < args.size() && i < params.size(); ++i)
					{
						auto argExpr = m_exprBuilder->build(*args[i]);
						if (!argExpr)
							continue;

						auto target = std::make_shared<awst::VarExpression>();
						target->sourceLocation = makeLoc(args[i]->location());
						target->name = params[i]->name();
						target->wtype = m_typeMapper.map(params[i]->type());

						argExpr = ExpressionBuilder::implicitNumericCast(
							std::move(argExpr), target->wtype, target->sourceLocation
						);

						auto assignment = std::make_shared<awst::AssignmentStatement>();
						assignment->sourceLocation = target->sourceLocation;
						assignment->target = target;
						assignment->value = std::move(argExpr);
						postInitBody->body.push_back(std::move(assignment));
					}
				}

				auto baseBody = buildBlock(baseCtor->body());
				inlineModifiers(*baseCtor, baseBody);
				for (auto& stmt: baseBody->body)
					postInitBody->body.push_back(std::move(stmt));
			}

			// Main constructor body
			if (constructor && constructor->body().statements().size() > 0)
			{
				m_exprBuilder->setInConstructor(true);
				auto ctorBody = buildBlock(constructor->body());
				inlineModifiers(*constructor, ctorBody);
				m_exprBuilder->setInConstructor(false);
				for (auto& stmt: ctorBody->body)
					postInitBody->body.push_back(std::move(stmt));
			}

			postInit.body = postInitBody;
			m_postInitMethod = std::move(postInit);
		}
		else
		{
		// Pre-evaluate constructor arguments in dependency order.
		// In viaIR, all ctor args are evaluated before any state var init or ctor body.
		// For transitive args (D→C→A), C's params must be assigned first so that
		// A's args (from C's modifier) see C's param values, not D's raw values.
		//
		// Phase 1: Assign direct ctor params (from D's modifiers/specifiers) into createBlock
		// Phase 2: Build pre-evaluated expressions for ALL base args
		std::map<solidity::frontend::ContractDefinition const*,
			std::vector<std::shared_ptr<awst::Expression>>> preEvaluatedArgs;
		{
			// Identify which args come directly from the main contract vs transitive
			std::set<solidity::frontend::ContractDefinition const*> directBases;
			if (constructor)
			{
				for (auto const& mod: constructor->modifiers())
				{
					auto const* ref = mod->name().annotation().referencedDeclaration;
					if (auto const* bc = dynamic_cast<solidity::frontend::ContractDefinition const*>(ref))
						directBases.insert(bc);
				}
			}
			for (auto const& baseSpec: _contract.baseContracts())
			{
				auto const* ref = baseSpec->name().annotation().referencedDeclaration;
				if (auto const* bc = dynamic_cast<solidity::frontend::ContractDefinition const*>(ref))
					directBases.insert(bc);
			}

			// Phase 1: Assign direct base ctor params into createBlock
			// (so transitive args can reference them)
			for (auto const* directBase: directBases)
			{
				auto argIt = explicitBaseArgs.find(directBase);
				if (argIt == explicitBaseArgs.end() || !argIt->second || argIt->second->empty())
					continue;
				auto const* baseCtor = directBase->constructor();
				if (!baseCtor)
					continue;

				auto const& args = *(argIt->second);
				auto const& params = baseCtor->parameters();
				for (size_t i = 0; i < args.size() && i < params.size(); ++i)
				{
					auto argExpr = m_exprBuilder->build(*args[i]);
					if (!argExpr)
						continue;
					auto* targetType = m_typeMapper.map(params[i]->type());
					argExpr = ExpressionBuilder::implicitNumericCast(
						std::move(argExpr), targetType, makeLoc(args[i]->location()));

					auto target = std::make_shared<awst::VarExpression>();
					target->sourceLocation = makeLoc(args[i]->location());
					target->name = params[i]->name();
					target->wtype = targetType;

					auto assignment = std::make_shared<awst::AssignmentStatement>();
					assignment->sourceLocation = target->sourceLocation;
					assignment->target = target;
					assignment->value = std::move(argExpr);
					createBlock->body.push_back(std::move(assignment));
				}
			}

			// Phase 2: Pre-evaluate transitive base ctor args in reverse-MRO order
			// (most-derived first), so intermediate params are assigned before
			// deeper transitive args reference them.
			// E.g., Final→Derived→Base1→Base: assign Base1.k first (from Derived.i),
			// then evaluate Base.j (from Base1.k).
			auto const& lin = _contract.annotation().linearizedBaseContracts;
			for (auto it = lin.begin(); it != lin.end(); ++it)
			{
				auto const* base = *it;
				if (base == &_contract)
					continue;
				if (directBases.count(base))
					continue;

				auto argIt = explicitBaseArgs.find(base);
				if (argIt == explicitBaseArgs.end() || !argIt->second || argIt->second->empty())
					continue;
				auto const* baseCtor = base->constructor();
				if (!baseCtor)
					continue;

				auto const& args = *(argIt->second);
				auto const& params = baseCtor->parameters();

				// Assign these params into createBlock NOW (so deeper transitives can see them)
				for (size_t i = 0; i < args.size() && i < params.size(); ++i)
				{
					auto argExpr = m_exprBuilder->build(*args[i]);
					if (!argExpr)
						continue;
					auto* targetType = m_typeMapper.map(params[i]->type());
					argExpr = ExpressionBuilder::implicitNumericCast(
						std::move(argExpr), targetType, makeLoc(args[i]->location()));

					auto target = std::make_shared<awst::VarExpression>();
					target->sourceLocation = makeLoc(args[i]->location());
					target->name = params[i]->name();
					target->wtype = targetType;

					auto assignment = std::make_shared<awst::AssignmentStatement>();
					assignment->sourceLocation = target->sourceLocation;
					assignment->target = target;
					assignment->value = std::move(argExpr);
					createBlock->body.push_back(std::move(assignment));
				}

				// Mark these params as pre-evaluated (empty vector = already assigned)
				preEvaluatedArgs[base] = {};
			}
		}

		// Interleave state variable initialization with constructor bodies.
		// For each base class in C3 linearization order (most-base first):
		//   1. Initialize that base's state variables (explicit initializers or zero)
		//   2. Inline that base's constructor body (with argument assignments)
		// This matches Solidity's viaIR semantics: a derived class's state variable
		// initializer (e.g. `uint y = f()`) can see state set by base constructors.
		auto const& linearized = _contract.annotation().linearizedBaseContracts;
		for (auto it = linearized.rbegin(); it != linearized.rend(); ++it)
		{
			auto const* base = *it;

			// 1. Initialize this base's state variables
			emitStateVarInit(*base, createBlock->body);

			if (base == &_contract)
				continue; // Main contract ctor handled separately below

			// 2. Inline this base's constructor body
			auto const* baseCtor = base->constructor();
			if (!baseCtor || !baseCtor->isImplemented())
				continue;
			if (baseCtor->body().statements().empty())
				continue;

			// Generate parameter assignments from pre-evaluated constructor arguments.
			// Direct base params were already assigned in Phase 1.
			// Transitive base params use pre-evaluated expressions.
			auto preIt = preEvaluatedArgs.find(base);
			if (preIt != preEvaluatedArgs.end())
			{
				auto const& evaledArgs = preIt->second;
				auto const& params = baseCtor->parameters();
				for (size_t i = 0; i < evaledArgs.size() && i < params.size(); ++i)
				{
					if (!evaledArgs[i])
						continue;

					auto target = std::make_shared<awst::VarExpression>();
					target->sourceLocation = method.sourceLocation;
					target->name = params[i]->name();
					target->wtype = m_typeMapper.map(params[i]->type());

					auto assignment = std::make_shared<awst::AssignmentStatement>();
					assignment->sourceLocation = method.sourceLocation;
					assignment->target = target;
					assignment->value = evaledArgs[i];
					createBlock->body.push_back(std::move(assignment));
				}
			}

			// Translate the base constructor body and inline its modifiers
			auto baseBody = buildBlock(baseCtor->body());
			inlineModifiers(*baseCtor, baseBody);
			for (auto& stmt: baseBody->body)
				createBlock->body.push_back(std::move(stmt));
		}

		// Include main contract constructor body if present
		if (constructor && constructor->body().statements().size() > 0)
		{
			// Restore super targets for constructor body (needed for super.f() calls).
			for (auto const& [id, name]: m_allSuperTargetNames)
				m_exprBuilder->addSuperTarget(id, name);
			// Also set up MRO overrides for the constructor specifically
			if (constructor)
			{
				auto pfit = m_perFuncSuperOverrides.find(constructor->id());
				if (pfit != m_perFuncSuperOverrides.end())
					for (auto const& [targetId, superName]: pfit->second)
						m_exprBuilder->addSuperTarget(targetId, superName);
			}
			m_exprBuilder->setInConstructor(true);
			auto ctorBody = buildBlock(constructor->body());
			inlineModifiers(*constructor, ctorBody);
			m_exprBuilder->setInConstructor(false);
			m_exprBuilder->clearSuperTargets();
			for (auto& stmt: ctorBody->body)
				createBlock->body.push_back(std::move(stmt));
		}
		} // end else (no postInit needed)

		// Return true to complete the create transaction
		auto trueLit = std::make_shared<awst::BoolConstant>();
		trueLit->sourceLocation = method.sourceLocation;
		trueLit->wtype = awst::WType::boolType();
		trueLit->value = true;

		auto createReturn = std::make_shared<awst::ReturnStatement>();
		createReturn->sourceLocation = method.sourceLocation;
		createReturn->value = trueLit;
		createBlock->body.push_back(createReturn);

		auto ifCreate = std::make_shared<awst::IfElse>();
		ifCreate->sourceLocation = method.sourceLocation;
		ifCreate->condition = isCreate;
		ifCreate->ifBranch = createBlock;

		body->body.push_back(ifCreate);
	}

	// Initialize EVM memory blob in scratch slots.
	// Each app call gets fresh scratch space, so we must initialize on every call.
	// store 0, bzero(4096) — pre-allocate a 4KB memory blob
	{
		auto blobSize = std::make_shared<awst::IntegerConstant>();
		blobSize->sourceLocation = method.sourceLocation;
		blobSize->wtype = awst::WType::uint64Type();
		blobSize->value = std::to_string(AssemblyBuilder::SLOT_SIZE);

		auto bzeroCall = std::make_shared<awst::IntrinsicCall>();
		bzeroCall->sourceLocation = method.sourceLocation;
		bzeroCall->wtype = awst::WType::bytesType();
		bzeroCall->opCode = "bzero";
		bzeroCall->stackArgs.push_back(std::move(blobSize));

		auto storeOp = std::make_shared<awst::IntrinsicCall>();
		storeOp->sourceLocation = method.sourceLocation;
		storeOp->wtype = awst::WType::voidType();
		storeOp->opCode = "store";
		storeOp->immediates = {AssemblyBuilder::MEMORY_SLOT_FIRST};
		storeOp->stackArgs.push_back(std::move(bzeroCall));

		auto exprStmt = std::make_shared<awst::ExpressionStatement>();
		exprStmt->sourceLocation = method.sourceLocation;
		exprStmt->expr = std::move(storeOp);
		body->body.push_back(std::move(exprStmt));

		// Write the free memory pointer (FMP) at offset 0x40 = 0x80.
		// This must be done once in the preamble, not in each assembly block,
		// so that subsequent assembly blocks see any FMP updates from earlier blocks.
		// Pattern: store 0, replace3(load(0), 64, pad32_0x80)
		auto loadBlob = std::make_shared<awst::IntrinsicCall>();
		loadBlob->sourceLocation = method.sourceLocation;
		loadBlob->wtype = awst::WType::bytesType();
		loadBlob->opCode = "load";
		loadBlob->immediates = {AssemblyBuilder::MEMORY_SLOT_FIRST};

		auto fmpOffset = std::make_shared<awst::IntegerConstant>();
		fmpOffset->sourceLocation = method.sourceLocation;
		fmpOffset->wtype = awst::WType::uint64Type();
		fmpOffset->value = "64"; // 0x40

		// 32-byte big-endian 0x80 = 0x00...0080
		auto fmpBytes = std::make_shared<awst::BytesConstant>();
		fmpBytes->sourceLocation = method.sourceLocation;
		fmpBytes->wtype = awst::WType::bytesType();
		fmpBytes->value = std::vector<uint8_t>(31, 0);
		fmpBytes->value.push_back(0x80);

		auto replaceOp = std::make_shared<awst::IntrinsicCall>();
		replaceOp->sourceLocation = method.sourceLocation;
		replaceOp->wtype = awst::WType::bytesType();
		replaceOp->opCode = "replace3";
		replaceOp->stackArgs.push_back(std::move(loadBlob));
		replaceOp->stackArgs.push_back(std::move(fmpOffset));
		replaceOp->stackArgs.push_back(std::move(fmpBytes));

		auto storeFmpOp = std::make_shared<awst::IntrinsicCall>();
		storeFmpOp->sourceLocation = method.sourceLocation;
		storeFmpOp->wtype = awst::WType::voidType();
		storeFmpOp->opCode = "store";
		storeFmpOp->immediates = {AssemblyBuilder::MEMORY_SLOT_FIRST};
		storeFmpOp->stackArgs.push_back(std::move(replaceOp));

		auto fmpStmt = std::make_shared<awst::ExpressionStatement>();
		fmpStmt->sourceLocation = method.sourceLocation;
		fmpStmt->expr = std::move(storeFmpOp);
		body->body.push_back(std::move(fmpStmt));
	}

	// ARC4 router
	auto routerExpr = std::make_shared<awst::ARC4Router>();
	routerExpr->sourceLocation = method.sourceLocation;
	routerExpr->wtype = awst::WType::boolType();

	auto routerReturn = std::make_shared<awst::ReturnStatement>();
	routerReturn->sourceLocation = method.sourceLocation;
	routerReturn->value = routerExpr;

	body->body.push_back(routerReturn);

	method.body = body;

	return method;
}

awst::ContractMethod ContractBuilder::buildClearProgram(
	solidity::frontend::ContractDefinition const& _contract,
	std::string const& _contractName
)
{
	awst::ContractMethod method;
	method.sourceLocation = makeLoc(_contract.location());
	method.returnType = awst::WType::boolType();
	method.cref = m_sourceFile + "." + _contractName;
	method.memberName = "clear_state_program";

	auto body = std::make_shared<awst::Block>();
	body->sourceLocation = method.sourceLocation;

	// return true
	auto trueLit = std::make_shared<awst::BoolConstant>();
	trueLit->sourceLocation = method.sourceLocation;
	trueLit->wtype = awst::WType::boolType();
	trueLit->value = true;

	auto ret = std::make_shared<awst::ReturnStatement>();
	ret->sourceLocation = method.sourceLocation;
	ret->value = trueLit;

	body->body.push_back(ret);
	method.body = body;

	return method;
}

awst::ContractMethod ContractBuilder::buildFunction(
	solidity::frontend::FunctionDefinition const& _func,
	std::string const& _contractName,
	std::string const& _nameOverride
)
{
	awst::ContractMethod method;
	method.sourceLocation = makeLoc(_func.location());
	method.cref = m_sourceFile + "." + _contractName;
	// Use name override if provided, otherwise disambiguate overloaded names
	if (!_nameOverride.empty())
	{
		method.memberName = _nameOverride;
	}
	else
	{
		method.memberName = _func.name();
		if (m_overloadedNames.count(_func.name()))
		{
			// Build unique suffix from parameter types to disambiguate
			// overloads with the same name and parameter count
			method.memberName += "(";
			bool first = true;
			for (auto const& p: _func.parameters())
			{
				if (!first) method.memberName += ",";
				// Use a short type tag for uniqueness
				auto const* solType = p->type();
				if (dynamic_cast<solidity::frontend::BoolType const*>(solType))
					method.memberName += "b";
				else if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(solType))
					method.memberName += (intType->isSigned() ? "i" : "u") + std::to_string(intType->numBits());
				else if (dynamic_cast<solidity::frontend::AddressType const*>(solType))
					method.memberName += "addr";
				else if (auto const* fixedBytes = dynamic_cast<solidity::frontend::FixedBytesType const*>(solType))
					method.memberName += "b" + std::to_string(fixedBytes->numBytes());
				else
					method.memberName += std::to_string(p->id());
				first = false;
			}
			method.memberName += ")";
		}
	}

	// Documentation
	if (_func.documentation())
		method.documentation.description = *_func.documentation()->text();

	// Parameters
	int paramIndex = 0;
	for (auto const& param: _func.parameters())
	{
		awst::SubroutineArgument arg;
		if (param->name().empty())
			arg.name = "_param" + std::to_string(paramIndex);
		else
			arg.name = param->name();
		arg.sourceLocation = makeLoc(param->location());
		arg.wtype = m_typeMapper.map(param->type());
		method.args.push_back(std::move(arg));
		paramIndex++;
	}

	// Return type
	auto const& returnParams = _func.returnParameters();
	// Track signed return params for sign-extension before ARC4 encoding
	struct SignedReturnInfo {
		unsigned bits;
		size_t index; // which return param (for tuples)
	};
	std::vector<SignedReturnInfo> signedReturns;

	if (returnParams.empty())
		method.returnType = awst::WType::voidType();
	else if (returnParams.size() == 1)
	{
		method.returnType = m_typeMapper.map(returnParams[0]->type());
		// For signed integer returns ≤64 bits, promote to biguint for proper
		// 256-bit two's complement ARC4 encoding.
		// Unwrap UserDefinedValueType to find the underlying IntegerType.
		auto const* retSolType = returnParams[0]->type();
		if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(retSolType))
			retSolType = &udvt->underlyingType();
		auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(retSolType);
		if (intType && intType->isSigned())
		{
			if (intType->numBits() <= 64)
				method.returnType = awst::WType::biguintType();
			signedReturns.push_back({intType->numBits(), 0});
		}
	}
	else
	{
		// Multiple return values → tuple
		std::vector<awst::WType const*> types;
		std::vector<std::string> names;
		bool hasNames = false;
		for (size_t ri = 0; ri < returnParams.size(); ++ri)
		{
			auto const& rp = returnParams[ri];
			auto* mappedType = m_typeMapper.map(rp->type());
			// Detect signed integer elements for sign-extension
			auto const* retSolType = rp->type();
			if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(retSolType))
				retSolType = &udvt->underlyingType();
			if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(retSolType))
			{
				if (intType->isSigned())
				{
					if (intType->numBits() <= 64)
						mappedType = awst::WType::biguintType();
					signedReturns.push_back({intType->numBits(), ri});
				}
			}
			types.push_back(mappedType);
			names.push_back(rp->name());
			if (!rp->name().empty())
				hasNames = true;
		}
		if (hasNames)
		{
			// Use function name + "Return" as the struct name to avoid
			// ARC56 collision when multiple methods return different named tuples.
			std::string tupleName = _func.name() + "Return";
			method.returnType = new awst::WTuple(std::move(types), std::move(names), std::move(tupleName));
		}
		else
			method.returnType = new awst::WTuple(std::move(types));
	}

	// Pure/view
	method.pure = _func.stateMutability() == solidity::frontend::StateMutability::Pure;

	// ARC4 method config for public/external functions
	method.arc4MethodConfig = buildARC4Config(_func, method.sourceLocation);

	// For ARC4 methods, convert array/tuple parameter types to ARC4 encoding
	// and prepare decode operations for the function body
	struct ParamDecode
	{
		std::string name;
		awst::WType const* nativeType;
		awst::WType const* arc4Type;
		awst::SourceLocation loc;
		unsigned maskBits = 0; // >0 for sub-64-bit unsigned types needing input masking
	};
	std::vector<ParamDecode> paramDecodes;
	if (method.arc4MethodConfig.has_value())
	{
		// Remap aggregate types to ARC4 encoding.
		// Note: Signed integers (int256) keep uint256 ABI type since
		// ARC4/Algorand ABI doesn't support signed types. Two's complement
		// encoding is handled at the application level.
		for (size_t pi = 0; pi < method.args.size(); ++pi)
		{
			auto& arg = method.args[pi];

			// Remap aggregate types (arrays, tuples) to ARC4 encoding
			bool isAggregate = arg.wtype
				&& (arg.wtype->kind() == awst::WTypeKind::ReferenceArray
					|| arg.wtype->kind() == awst::WTypeKind::WTuple);
			if (!isAggregate)
				continue;

			awst::WType const* arc4Type = m_typeMapper.mapToARC4Type(arg.wtype);
			if (arc4Type != arg.wtype)
			{
				paramDecodes.push_back({arg.name, arg.wtype, arc4Type, arg.sourceLocation});
				arg.wtype = arc4Type;
			}
		}
	}

	// Function body
	if (_func.isImplemented())
	{
		// Set function context for inline assembly translation
		// Use the (possibly ARC4-remapped) types from the method args
		{
			std::vector<std::pair<std::string, awst::WType const*>> paramContext;
			std::map<std::string, unsigned> bitWidths;
			for (auto const& arg: method.args)
				paramContext.emplace_back(arg.name, arg.wtype);
			// Collect sub-64-bit widths from function params and return params
			for (auto const& p: _func.parameters())
			{
				auto const* solType = p->annotation().type;
				auto const* intType = solType ? dynamic_cast<solidity::frontend::IntegerType const*>(solType) : nullptr;
				if (!intType && solType)
					if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
						intType = dynamic_cast<solidity::frontend::IntegerType const*>(&udvt->underlyingType());
				if (intType && intType->numBits() < 64)
					bitWidths[p->name()] = intType->numBits();
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
			setFunctionContext(paramContext, method.returnType, bitWidths);
		}

		method.body = buildBlock(_func.body());

		// Insert zero-initialization for named return variables
		// Solidity implicitly initializes named returns to their zero values.
		// This is critical for struct types where field-by-field assignment
		// reads other fields from the variable via copy-on-write pattern.
		{
			auto const& retParams = _func.returnParameters();
			std::vector<std::shared_ptr<awst::Statement>> inits;
			for (auto const& rp: retParams)
			{
				if (rp->name().empty())
					continue;
				auto* rpType = m_typeMapper.map(rp->type());

				auto target = std::make_shared<awst::VarExpression>();
				target->sourceLocation = method.sourceLocation;
				target->wtype = rpType;
				target->name = rp->name();

				auto zeroVal = StorageMapper::makeDefaultValue(rpType, method.sourceLocation);

				auto assign = std::make_shared<awst::AssignmentStatement>();
				assign->sourceLocation = method.sourceLocation;
				assign->target = std::move(target);
				assign->value = std::move(zeroVal);
				inits.push_back(std::move(assign));
			}
			if (!inits.empty())
			{
				method.body->body.insert(
					method.body->body.begin(),
					std::make_move_iterator(inits.begin()),
					std::make_move_iterator(inits.end())
				);
			}
		}

		// Ensure all non-void functions end with a return statement.
		// For named return parameters, synthesize a return referencing the variables.
		// Otherwise append a default zero-value return.
		if (method.returnType != awst::WType::voidType()
			&& !blockAlwaysReturns(method.body->body))
		{
			auto const& retParams = _func.returnParameters();
			bool hasNamedReturns = false;
			for (auto const& rp: retParams)
				if (!rp->name().empty())
					hasNamedReturns = true;

			auto retStmt = std::make_shared<awst::ReturnStatement>();
			retStmt->sourceLocation = method.sourceLocation;

			if (hasNamedReturns)
			{
				if (retParams.size() == 1)
				{
					auto var = std::make_shared<awst::VarExpression>();
					var->sourceLocation = method.sourceLocation;
					var->name = retParams[0]->name();
					var->wtype = m_typeMapper.map(retParams[0]->type());
					retStmt->value = std::move(var);
				}
				else
				{
					auto tuple = std::make_shared<awst::TupleExpression>();
					tuple->sourceLocation = method.sourceLocation;
					for (auto const& rp: retParams)
					{
						auto var = std::make_shared<awst::VarExpression>();
						var->sourceLocation = method.sourceLocation;
						var->name = rp->name();
						var->wtype = m_typeMapper.map(rp->type());
						tuple->items.push_back(std::move(var));
					}
					tuple->wtype = method.returnType;
					retStmt->value = std::move(tuple);
				}
			}
			else
			{
				retStmt->value = StorageMapper::makeDefaultValue(method.returnType, method.sourceLocation);
			}
			method.body->body.push_back(std::move(retStmt));
		}

		// For ARC4 methods returning dynamic arrays, convert the return type
		// to ARC4 encoding and wrap return values in ARC4Encode.
		if (method.arc4MethodConfig.has_value()
			&& method.returnType->kind() == awst::WTypeKind::ReferenceArray)
		{
			auto const* arc4RetType = m_typeMapper.mapToARC4Type(method.returnType);
			if (arc4RetType != method.returnType)
			{
				// Wrap all return values in ARC4Encode
				std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> wrapReturns;
				wrapReturns = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
				{
					for (auto& stmt: stmts)
					{
						if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
						{
							if (ret->value)
							{
								auto encode = std::make_shared<awst::ARC4Encode>();
								encode->sourceLocation = ret->value->sourceLocation;
								encode->wtype = arc4RetType;
								encode->value = std::move(ret->value);
								ret->value = std::move(encode);
							}
						}
						else if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get()))
						{
							if (ifElse->ifBranch)
								wrapReturns(ifElse->ifBranch->body);
							if (ifElse->elseBranch)
								wrapReturns(ifElse->elseBranch->body);
						}
						else if (auto* block = dynamic_cast<awst::Block*>(stmt.get()))
						{
							wrapReturns(block->body);
						}
					}
				};
				wrapReturns(method.body->body);
				method.returnType = arc4RetType;
			}
		}

		// Sign-extend return values for signed integer types ≤64 bits.
		if (!signedReturns.empty() && method.arc4MethodConfig.has_value())
		{
			std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> walk;
			walk = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
			{
				for (auto& stmt: stmts)
				{
					if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
					{
						if (!ret->value) continue;
						auto srcLoc = ret->value->sourceLocation;

						if (signedReturns.size() == 1 && signedReturns[0].index == 0
							&& returnParams.size() == 1)
						{
							// Single return — sign-extend directly
							ret->value = signExtendToUint256(
								std::move(ret->value), signedReturns[0].bits, srcLoc);
						}
						else if (auto* tuple = dynamic_cast<awst::TupleExpression*>(ret->value.get()))
						{
							// Tuple return — sign-extend individual elements
							for (auto const& sr: signedReturns)
							{
								if (sr.index < tuple->items.size())
								{
									tuple->items[sr.index] = signExtendToUint256(
										std::move(tuple->items[sr.index]), sr.bits, srcLoc);
								}
							}
							tuple->wtype = method.returnType;
						}
					}
					else if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get()))
					{
						if (ifElse->ifBranch) walk(ifElse->ifBranch->body);
						if (ifElse->elseBranch) walk(ifElse->elseBranch->body);
					}
					else if (auto* block = dynamic_cast<awst::Block*>(stmt.get()))
						walk(block->body);
				}
			};
			walk(method.body->body);
		}

		// Skip ARC4 decode for functions with inline assembly blocks.
		// The assembly translator handles parameter data directly via
		// calldataload mapping using ARC4-encoded types.
		bool hasInlineAssembly = false;
		for (auto const& stmt: _func.body().statements())
		{
			if (dynamic_cast<solidity::frontend::InlineAssembly const*>(stmt.get()))
			{
				hasInlineAssembly = true;
				break;
			}
		}

		// Insert ARC4 decode operations for aggregate parameters.
		// The method args were remapped to ARC4 types, but the body uses
		// native types. We rename the ARC4 arg and insert a decode statement.
		if (!paramDecodes.empty() && !hasInlineAssembly)
		{
			std::vector<std::shared_ptr<awst::Statement>> decodeStmts;
			for (auto& pd: paramDecodes)
			{
				// Rename the method arg to __arc4_<name>
				std::string arc4Name = "__arc4_" + pd.name;
				for (auto& arg: method.args)
				{
					if (arg.name == pd.name)
					{
						arg.name = arc4Name;
						break;
					}
				}

				// Create: <name> = arc4_decode(__arc4_<name>)
				// For dynamic arrays (ReferenceArray with null array_size), use
				// IntrinsicCall("extract", [2, 0]) to strip the ARC4 length header
				// instead of ARC4Decode — works around a puya backend issue where
				// extract3(value, 2, 0) returns empty bytes instead of extracting
				// to end (see puya-possible-bug.md).
				auto arc4Var = std::make_shared<awst::VarExpression>();
				arc4Var->sourceLocation = pd.loc;
				arc4Var->name = arc4Name;
				arc4Var->wtype = pd.arc4Type;

				std::shared_ptr<awst::Expression> decodeExpr;
				auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(pd.nativeType);
				if (refArr && !refArr->arraySize().has_value())
				{
					// Dynamic array: use ConvertArray instead of ARC4Decode.
					// This works around a puya backend bug where ARC4Decode
					// uses extract3(value, 2, 0) to strip the length header,
					// but extract3 with length=0 returns empty bytes instead
					// of extracting to end (see puya-possible-bug.md).
					// ConvertArray uses len+substring3 which works correctly.
					auto convert = std::make_shared<awst::ConvertArray>();
					convert->sourceLocation = pd.loc;
					convert->wtype = pd.nativeType;
					convert->expr = std::move(arc4Var);
					decodeExpr = std::move(convert);
				}
				else
				{
					auto decode = std::make_shared<awst::ARC4Decode>();
					decode->sourceLocation = pd.loc;
					decode->wtype = pd.nativeType;
					decode->value = std::move(arc4Var);
					decodeExpr = std::move(decode);
				}

				auto target = std::make_shared<awst::VarExpression>();
				target->sourceLocation = pd.loc;
				target->name = pd.name;
				target->wtype = pd.nativeType;

				auto assign = std::make_shared<awst::AssignmentStatement>();
				assign->sourceLocation = pd.loc;
				assign->target = std::move(target);
				assign->value = std::move(decodeExpr);
				decodeStmts.push_back(std::move(assign));
			}
			method.body->body.insert(
				method.body->body.begin(),
				std::make_move_iterator(decodeStmts.begin()),
				std::make_move_iterator(decodeStmts.end())
			);
		}

		// Mask sub-64-bit unsigned parameters at function entry.
		// EVM truncates ABI-decoded values to parameter type width;
		// AVM uint64 preserves the full value, so we must mask explicitly.
		{
			std::vector<std::shared_ptr<awst::Statement>> maskStmts;
			for (size_t pi = 0; pi < _func.parameters().size(); ++pi)
			{
				auto const& param = _func.parameters()[pi];
				auto const* solType = param->annotation().type;
				if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
					solType = &udvt->underlyingType();
				auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(solType);
				if (!intType || intType->numBits() >= 64 || intType->isSigned())
					continue;

				unsigned bits = intType->numBits();
				uint64_t mask = (uint64_t(1) << bits) - 1;
				auto loc = makeLoc(param->location());

				// ABI v2: assert param fits in type (revert on overflow)
				// ABI v1: silently truncate (mask only)
				bool useV2 = true; // default in 0.8+
				if (m_currentContract)
				{
					auto const& ann = m_currentContract->sourceUnit().annotation();
					if (ann.useABICoderV2.set())
						useV2 = *ann.useABICoderV2;
				}
				if (useV2)
				{
					auto paramCheck = std::make_shared<awst::VarExpression>();
					paramCheck->sourceLocation = loc;
					paramCheck->name = param->name();
					paramCheck->wtype = awst::WType::uint64Type();

					auto maxVal = std::make_shared<awst::IntegerConstant>();
					maxVal->sourceLocation = loc;
					maxVal->wtype = awst::WType::uint64Type();
					maxVal->value = std::to_string(mask);

					auto cmp = std::make_shared<awst::NumericComparisonExpression>();
					cmp->sourceLocation = loc;
					cmp->wtype = awst::WType::boolType();
					cmp->lhs = paramCheck;
					cmp->op = awst::NumericComparison::Lte;
					cmp->rhs = std::move(maxVal);

					auto assertExpr = std::make_shared<awst::AssertExpression>();
					assertExpr->sourceLocation = loc;
					assertExpr->wtype = awst::WType::voidType();
					assertExpr->condition = std::move(cmp);
					assertExpr->errorMessage = "ABI validation";

					auto assertStmt = std::make_shared<awst::ExpressionStatement>();
					assertStmt->sourceLocation = loc;
					assertStmt->expr = std::move(assertExpr);
					maskStmts.push_back(std::move(assertStmt));
				}

				auto paramVar = std::make_shared<awst::VarExpression>();
				paramVar->sourceLocation = loc;
				paramVar->name = param->name();
				paramVar->wtype = awst::WType::uint64Type();

				auto maskConst = std::make_shared<awst::IntegerConstant>();
				maskConst->sourceLocation = loc;
				maskConst->wtype = awst::WType::uint64Type();
				maskConst->value = std::to_string(mask);

				auto bitAnd = std::make_shared<awst::UInt64BinaryOperation>();
				bitAnd->sourceLocation = loc;
				bitAnd->wtype = awst::WType::uint64Type();
				bitAnd->left = paramVar;
				bitAnd->op = awst::UInt64BinaryOperator::BitAnd;
				bitAnd->right = std::move(maskConst);

				auto target = std::make_shared<awst::VarExpression>();
				target->sourceLocation = loc;
				target->name = param->name();
				target->wtype = awst::WType::uint64Type();

				auto assign = std::make_shared<awst::AssignmentStatement>();
				assign->sourceLocation = loc;
				assign->target = std::move(target);
				assign->value = std::move(bitAnd);
				maskStmts.push_back(std::move(assign));
			}
			if (!maskStmts.empty())
			{
				method.body->body.insert(
					method.body->body.begin(),
					std::make_move_iterator(maskStmts.begin()),
					std::make_move_iterator(maskStmts.end())
				);
			}
		}

		// Initialize transient storage blob at method entry
		// Needed for Solidity transient vars AND assembly tload/tstore
		bool hasInlineAsm = !_func.body().statements().empty() && std::any_of(
			_func.body().statements().begin(), _func.body().statements().end(),
			[](auto const& s) { return dynamic_cast<solidity::frontend::InlineAssembly const*>(s.get()) != nullptr; }
		);
		if (method.arc4MethodConfig.has_value()
			&& (m_transientStorage.hasTransientVars() || hasInlineAsm))
		{
			auto initStmt = m_transientStorage.buildInit(method.sourceLocation);
			method.body->body.insert(method.body->body.begin(), std::move(initStmt));
		}

		// Build modifier chain as separate subroutines (or inline for simple cases)
		if (!_func.modifiers().empty())
			buildModifierChain(_func, method, _contractName);

		// Inject ensure_budget for opup budget padding
		// Check per-function map first, then global opup budget
		uint64_t budgetForFunc = 0;
		if (auto it = m_ensureBudget.find(_func.name()); it != m_ensureBudget.end())
			budgetForFunc = it->second;
		else if (m_opupBudget > 0 && method.arc4MethodConfig.has_value())
			budgetForFunc = m_opupBudget;

		if (budgetForFunc > 0)
		{
			auto budgetVal = std::make_shared<awst::IntegerConstant>();
			budgetVal->sourceLocation = method.sourceLocation;
			budgetVal->wtype = awst::WType::uint64Type();
			budgetVal->value = std::to_string(budgetForFunc);

			auto feeSource = std::make_shared<awst::IntegerConstant>();
			feeSource->sourceLocation = method.sourceLocation;
			feeSource->wtype = awst::WType::uint64Type();
			feeSource->value = "0";

			auto call = std::make_shared<awst::PuyaLibCall>();
			call->sourceLocation = method.sourceLocation;
			call->wtype = awst::WType::voidType();
			call->func = "ensure_budget";
			call->args = {
				awst::CallArg{std::string("required_budget"), budgetVal},
				awst::CallArg{std::string("fee_source"), feeSource}
			};

			auto stmt = std::make_shared<awst::ExpressionStatement>();
			stmt->sourceLocation = method.sourceLocation;
			stmt->expr = std::move(call);

			method.body->body.insert(method.body->body.begin(), std::move(stmt));
		}

	}
	else
	{
		// Abstract function — empty body
		Logger::instance().debug("function '" + method.memberName + "' has no implementation", method.sourceLocation);
		method.body = std::make_shared<awst::Block>();
		method.body->sourceLocation = method.sourceLocation;
	}

	return method;
}

std::shared_ptr<awst::Expression> ContractBuilder::signExtendToUint256(
	std::shared_ptr<awst::Expression> _value,
	unsigned _bits,
	awst::SourceLocation const& _loc
)
{
	return TypeCoercion::signExtendToUint256(std::move(_value), _bits, _loc);
}


std::optional<awst::ARC4MethodConfig> ContractBuilder::buildARC4Config(
	solidity::frontend::FunctionDefinition const& _func,
	awst::SourceLocation const& _loc
)
{
	using namespace solidity::frontend;

	auto vis = _func.visibility();

	if (vis == Visibility::Private || vis == Visibility::Internal)
		return std::nullopt;

	// Public/external functions get ARC4 ABI method configs
	awst::ARC4ABIMethodConfig config;
	config.sourceLocation = _loc;
	// Distinguish fallback from receive: both have empty Solidity names,
	// but need different ARC4 method names for routing.
	if (_func.isFallback())
		config.name = "__fallback";
	else if (_func.isReceive())
		config.name = "__receive";
	else
		config.name = _func.name();
	config.allowedCompletionTypes = {0}; // NoOp
	config.create = 3; // Disallow

	// View functions are readonly
	if (_func.stateMutability() == StateMutability::View ||
		_func.stateMutability() == StateMutability::Pure)
	{
		config.readonly = true;
	}

	return awst::ARC4MethodConfig(config);
}

void ContractBuilder::inlineModifiers(
	solidity::frontend::FunctionDefinition const& _func,
	std::shared_ptr<awst::Block>& _body
)
{
	static int modCounter = 0;

	// For each modifier invocation, wrap the function body
	for (auto const& modInvocation: _func.modifiers())
	{
		auto const* modDef = dynamic_cast<solidity::frontend::ModifierDefinition const*>(
			modInvocation->name().annotation().referencedDeclaration
		);

		if (!modDef)
			continue;

		// Resolve virtual overrides — but NOT for explicit base modifier calls (A.m).
		{
			bool isExplicit = modInvocation->name().path().size() > 1;
			if (m_currentContract && !isExplicit)
			{
				std::string modName = modDef->name();
				for (auto const* base: m_currentContract->annotation().linearizedBaseContracts)
					for (auto const* mod: base->functionModifiers())
						if (mod->name() == modName) { modDef = mod; goto modResolved; }
				modResolved:;
			}
		}

		// Translate modifier body, replacing `_` (PlaceholderStatement) with the original body
		auto modBody = std::make_shared<awst::Block>();
		modBody->sourceLocation = makeLoc(modDef->location());

		if (modDef->body().statements().empty())
			continue;

		// Bind modifier arguments to unique local variables.
		// e.g. onlyRole(getRoleAdmin(role)) → __mod_role_N = getRoleAdmin(role)
		auto const* args = modInvocation->arguments();
		auto const& params = modDef->parameters();
		std::vector<int64_t> remappedDeclIds;

		if (args && !args->empty())
		{
			auto modLoc = makeLoc(modInvocation->location());
			for (size_t i = 0; i < args->size() && i < params.size(); ++i)
			{
				auto const& param = params[i];
				std::string uniqueName = "__mod_" + param->name() + "_" + std::to_string(modCounter++);
				auto* paramType = m_typeMapper.map(param->type());

				// Translate the argument expression (e.g. getRoleAdmin(role))
				auto argExpr = m_exprBuilder->build(*(*args)[i]);
				if (!argExpr)
					continue;

				// Cast to parameter type if needed
				argExpr = ExpressionBuilder::implicitNumericCast(
					std::move(argExpr), paramType, modLoc
				);

				// Create assignment: __mod_role_N = <evaluated arg>
				auto target = std::make_shared<awst::VarExpression>();
				target->sourceLocation = modLoc;
				target->name = uniqueName;
				target->wtype = paramType;

				auto assignment = std::make_shared<awst::AssignmentStatement>();
				assignment->sourceLocation = modLoc;
				assignment->target = target;
				assignment->value = std::move(argExpr);
				modBody->body.push_back(std::move(assignment));

				// Register remap so modifier body references resolve to the unique name
				m_exprBuilder->addParamRemap(param->id(), uniqueName, paramType);
				remappedDeclIds.push_back(param->id());
			}
		}

		// Separate the function body into:
		// 1. Named return var initializations (hoisted before modifier)
		// 2. Placeholder body (the `_;` replacement)
		// 3. Deferred return (appended after modifier)
		//
		// Named return vars must be initialized before the modifier body so
		// that multiple `_;` invocations (e.g. in loops) share the same
		// variable rather than re-initializing it each time.
		auto placeholderBody = std::make_shared<awst::Block>();
		placeholderBody->sourceLocation = _body->sourceLocation;
		std::shared_ptr<awst::Statement> deferredReturn;

		// Collect named return parameter names
		std::set<std::string> returnParamNames;
		for (auto const& retParam: _func.returnParameters())
			if (!retParam->name().empty())
				returnParamNames.insert(retParam->name());

		// Track which named return vars have already been hoisted (only hoist
		// the first assignment — the default init — not subsequent assignments
		// like `r = true` from `return true;`)
		std::set<std::string> hoistedReturnVars;

		for (auto const& bodyStmt: _body->body)
		{
			if (auto const* retStmt = dynamic_cast<awst::ReturnStatement const*>(bodyStmt.get()))
			{
				// For named returns: split `return expr;` into `r = expr;` (in
				// placeholder body) + `return r;` (deferred). This ensures the
				// modifier controls whether the assignment executes.
				if (returnParamNames.size() == 1 && retStmt->value)
				{
					auto const& retName = *returnParamNames.begin();
					// Check if the return value is NOT just a reference to the
					// named return var itself (avoid redundant r = r;)
					bool isJustRetVar = false;
					if (auto const* varRef = dynamic_cast<awst::VarExpression const*>(retStmt->value.get()))
						isJustRetVar = (varRef->name == retName);

					if (!isJustRetVar)
					{
						// Create assignment: r = expr
						auto target = std::make_shared<awst::VarExpression>();
						target->sourceLocation = retStmt->sourceLocation;
						target->wtype = retStmt->value->wtype;
						target->name = retName;

						auto assign = std::make_shared<awst::AssignmentStatement>();
						assign->sourceLocation = retStmt->sourceLocation;
						assign->target = std::move(target);
						assign->value = retStmt->value;
						placeholderBody->body.push_back(std::move(assign));
					}

					// Create deferred return: return r
					auto retVar = std::make_shared<awst::VarExpression>();
					retVar->sourceLocation = retStmt->sourceLocation;
					retVar->wtype = retStmt->value->wtype;
					retVar->name = retName;

					auto deferRet = std::make_shared<awst::ReturnStatement>();
					deferRet->sourceLocation = retStmt->sourceLocation;
					deferRet->value = std::move(retVar);
					deferredReturn = std::move(deferRet);
				}
				else
				{
					deferredReturn = bodyStmt;
				}
			}
			else if (!returnParamNames.empty())
			{
				// Check if this is a named return var initialization (first assignment only)
				auto const* assign = dynamic_cast<awst::AssignmentStatement const*>(bodyStmt.get());
				auto const* targetVar = assign
					? dynamic_cast<awst::VarExpression const*>(assign->target.get())
					: nullptr;
				if (targetVar && returnParamNames.count(targetVar->name)
					&& !hoistedReturnVars.count(targetVar->name))
				{
					// Hoist: put initialization before the modifier body
					modBody->body.push_back(bodyStmt);
					hoistedReturnVars.insert(targetVar->name);
				}
				else
				{
					placeholderBody->body.push_back(bodyStmt);
				}
			}
			else
			{
				placeholderBody->body.push_back(bodyStmt);
			}
		}

		// Translate the entire modifier body through the statement builder.
		// The `_;` (PlaceholderStatement) will be replaced with the function body
		// wherever it appears — including inside loops and conditionals.
		setPlaceholderBody(placeholderBody);
		auto translatedModBody = buildBlock(modDef->body());
		setPlaceholderBody(nullptr);

		if (translatedModBody)
		{
			for (auto& stmt: translatedModBody->body)
				modBody->body.push_back(std::move(stmt));
		}
		else
		{
			// Fallback: copy body as-is
			for (auto const& bodyStmt: _body->body)
				modBody->body.push_back(bodyStmt);
		}

		// Append the deferred return after the modifier body
		if (deferredReturn)
			modBody->body.push_back(std::move(deferredReturn));

		// Unregister remaps so they don't affect subsequent code
		for (auto declId: remappedDeclIds)
			m_exprBuilder->removeParamRemap(declId);

		_body = modBody;
	}
}

void ContractBuilder::buildModifierChain(
	solidity::frontend::FunctionDefinition const& _func,
	awst::ContractMethod& _method,
	std::string const& _contractName
)
{
	static int modChainCounter = 0;
	int chainId = modChainCounter++;

	auto const& modifiers = _func.modifiers();
	if (modifiers.empty())
		return;

	std::string baseName = _func.name();
	std::string cref = m_sourceFile + "." + _contractName;

	// Step 1: Create the innermost subroutine (function body without modifiers)
	std::string bodySubName = baseName + "__body_" + std::to_string(chainId);
	{
		awst::ContractMethod bodySub;
		bodySub.sourceLocation = _method.sourceLocation;
		bodySub.cref = cref;
		bodySub.memberName = bodySubName;
		bodySub.returnType = _method.returnType;
		bodySub.args = _method.args; // same params as outer function
		bodySub.body = _method.body; // move the original function body here
		bodySub.arc4MethodConfig = std::nullopt; // internal, not ABI-routable
		bodySub.pure = _method.pure;
		m_modifierSubroutines.push_back(std::move(bodySub));
	}

	// Step 2: Build modifier subroutines from innermost to outermost.
	// Each calls the next (or the body sub for the innermost modifier).
	std::string nextSubName = bodySubName;

	for (int i = static_cast<int>(modifiers.size()) - 1; i >= 0; --i)
	{
		auto const& modInvocation = modifiers[i];
		auto const* modDef = dynamic_cast<solidity::frontend::ModifierDefinition const*>(
			modInvocation->name().annotation().referencedDeclaration
		);
		if (!modDef)
		{
			// Constructor base call — skip, handled elsewhere
			continue;
		}

		// Resolve virtual overrides — but NOT for explicit base modifier calls (A.m).
		// Detect explicit base: the IdentifierPath has >1 component for A.m.
		// For inherited functions, the referencedDeclaration points to the base
		// modifier, but we still want the most-derived override.
		bool isExplicitBaseModifier = false;
		{
			// Check the IdentifierPath: "A.m" has path ["A", "m"], "m" has path ["m"]
			auto const& path = modInvocation->name().path();
			if (path.size() > 1)
				isExplicitBaseModifier = true;
		}

		if (m_currentContract && !isExplicitBaseModifier)
		{
			std::string modName = modDef->name();
			for (auto const* base: m_currentContract->annotation().linearizedBaseContracts)
				for (auto const* mod: base->functionModifiers())
					if (mod->name() == modName) { modDef = mod; goto foundMostDerived; }
			foundMostDerived:;
		}

		std::string modSubName = baseName + "__mod" + std::to_string(i) + "_" + std::to_string(chainId);

		// Create the modifier subroutine
		awst::ContractMethod modSub;
		modSub.sourceLocation = makeLoc(modDef->location());
		modSub.cref = cref;
		modSub.memberName = modSubName;
		modSub.returnType = _method.returnType;
		modSub.args = _method.args; // receives same params
		modSub.arc4MethodConfig = std::nullopt; // internal
		modSub.pure = _method.pure;

		// Build modifier body block
		auto modBody = std::make_shared<awst::Block>();
		modBody->sourceLocation = modSub.sourceLocation;

		// Evaluate modifier arguments → bind to local vars via paramRemaps
		auto const* args = modInvocation->arguments();
		auto const& params = modDef->parameters();
		std::vector<int64_t> remappedDeclIds;
		static int modArgCounter = 0;

		if (args && !args->empty())
		{
			auto modLoc = makeLoc(modInvocation->location());
			for (size_t pi = 0; pi < args->size() && pi < params.size(); ++pi)
			{
				auto const& param = params[pi];
				std::string uniqueName = "__mod_" + param->name() + "_" + std::to_string(modArgCounter++);
				auto* paramType = m_typeMapper.map(param->type());

				auto argExpr = m_exprBuilder->build(*(*args)[pi]);
				if (!argExpr) continue;
				argExpr = ExpressionBuilder::implicitNumericCast(std::move(argExpr), paramType, modLoc);

				auto target = std::make_shared<awst::VarExpression>();
				target->sourceLocation = modLoc;
				target->name = uniqueName;
				target->wtype = paramType;

				auto assignment = std::make_shared<awst::AssignmentStatement>();
				assignment->sourceLocation = modLoc;
				assignment->target = target;
				assignment->value = std::move(argExpr);
				modBody->body.push_back(std::move(assignment));

				m_exprBuilder->addParamRemap(param->id(), uniqueName, paramType);
				remappedDeclIds.push_back(param->id());
			}
		}

		// Set placeholder body: at `_;`, call the next subroutine
		// Build a block that calls nextSubName and assigns return value
		auto placeholderBlock = std::make_shared<awst::Block>();
		placeholderBlock->sourceLocation = modSub.sourceLocation;

		// Determine return variable name for this modifier sub
		std::string retVarName;
		auto const& retParams = _func.returnParameters();
		if (_method.returnType != awst::WType::voidType())
		{
			if (retParams.size() == 1 && !retParams[0]->name().empty())
				retVarName = retParams[0]->name();
			else
				retVarName = "__retval_" + std::to_string(chainId) + "_" + std::to_string(i);
		}

		{
			// Build: returnVar = nextSub(args...)
			auto call = std::make_shared<awst::SubroutineCallExpression>();
			call->sourceLocation = modSub.sourceLocation;
			call->wtype = _method.returnType;
			call->target = awst::InstanceMethodTarget{nextSubName};
			for (auto const& arg: _method.args)
			{
				awst::CallArg ca;
				ca.name = arg.name;
				auto varRef = std::make_shared<awst::VarExpression>();
				varRef->sourceLocation = modSub.sourceLocation;
				varRef->wtype = arg.wtype;
				varRef->name = arg.name;
				ca.value = std::move(varRef);
				call->args.push_back(std::move(ca));
			}

			if (!retVarName.empty())
			{
				// Assign call result to return variable
				auto retTarget = std::make_shared<awst::VarExpression>();
				retTarget->sourceLocation = modSub.sourceLocation;
				retTarget->wtype = _method.returnType;
				retTarget->name = retVarName;

				auto assign = std::make_shared<awst::AssignmentStatement>();
				assign->sourceLocation = modSub.sourceLocation;
				assign->target = std::move(retTarget);
				assign->value = std::move(call);
				placeholderBlock->body.push_back(std::move(assign));
			}
			else
			{
				auto stmt = std::make_shared<awst::ExpressionStatement>();
				stmt->sourceLocation = modSub.sourceLocation;
				stmt->expr = std::move(call);
				placeholderBlock->body.push_back(std::move(stmt));
			}
		}

		// Initialize return variable at the start of the modifier sub
		if (!retVarName.empty())
		{
			auto target = std::make_shared<awst::VarExpression>();
			target->sourceLocation = modSub.sourceLocation;
			target->wtype = _method.returnType;
			target->name = retVarName;
			auto zeroVal = StorageMapper::makeDefaultValue(_method.returnType, modSub.sourceLocation);
			auto assign = std::make_shared<awst::AssignmentStatement>();
			assign->sourceLocation = modSub.sourceLocation;
			assign->target = std::move(target);
			assign->value = std::move(zeroVal);
			modBody->body.push_back(std::move(assign));
		}

		// Translate modifier body with _;→placeholderBlock
		setPlaceholderBody(placeholderBlock);
		auto translatedBody = buildBlock(modDef->body());
		setPlaceholderBody(nullptr);

		if (translatedBody)
		{
			// Fix bare return statements in modifier body: `return;` → `return __retval;`
			// A modifier's bare `return` means "exit early", returning the current retval.
			if (!retVarName.empty())
			{
				std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> fixReturns;
				fixReturns = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
				{
					for (auto& stmt: stmts)
					{
						if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
						{
							if (!ret->value)
							{
								auto var = std::make_shared<awst::VarExpression>();
								var->sourceLocation = ret->sourceLocation;
								var->name = retVarName;
								var->wtype = _method.returnType;
								ret->value = std::move(var);
							}
						}
						if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get()))
						{
							if (ifElse->ifBranch) fixReturns(ifElse->ifBranch->body);
							if (ifElse->elseBranch) fixReturns(ifElse->elseBranch->body);
						}
						if (auto* block = dynamic_cast<awst::Block*>(stmt.get()))
							fixReturns(block->body);
						if (auto* loop = dynamic_cast<awst::WhileLoop*>(stmt.get()))
							if (loop->loopBody) fixReturns(loop->loopBody->body);
					}
				};
				fixReturns(translatedBody->body);
			}

			for (auto& stmt: translatedBody->body)
				modBody->body.push_back(std::move(stmt));
		}

		// Add return statement using the return variable
		{
			auto retStmt = std::make_shared<awst::ReturnStatement>();
			retStmt->sourceLocation = modSub.sourceLocation;
			if (!retVarName.empty())
			{
				auto var = std::make_shared<awst::VarExpression>();
				var->sourceLocation = modSub.sourceLocation;
				var->name = retVarName;
				var->wtype = _method.returnType;
				retStmt->value = std::move(var);
			}
			modBody->body.push_back(std::move(retStmt));
		}

		// Unregister remaps
		for (auto declId: remappedDeclIds)
			m_exprBuilder->removeParamRemap(declId);

		modSub.body = modBody;
		m_modifierSubroutines.push_back(std::move(modSub));
		nextSubName = modSubName;
	}

	// Step 3: Replace _method.body with a call to the outermost modifier subroutine
	auto entryBody = std::make_shared<awst::Block>();
	entryBody->sourceLocation = _method.sourceLocation;

	// Initialize named return vars to zero
	for (auto const& rp: _func.returnParameters())
	{
		if (rp->name().empty()) continue;
		auto* rpType = m_typeMapper.map(rp->type());
		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = _method.sourceLocation;
		target->wtype = rpType;
		target->name = rp->name();

		auto zeroVal = StorageMapper::makeDefaultValue(rpType, _method.sourceLocation);
		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _method.sourceLocation;
		assign->target = std::move(target);
		assign->value = std::move(zeroVal);
		entryBody->body.push_back(std::move(assign));
	}

	// Call outermost modifier sub
	auto call = std::make_shared<awst::SubroutineCallExpression>();
	call->sourceLocation = _method.sourceLocation;
	call->wtype = _method.returnType;
	call->target = awst::InstanceMethodTarget{nextSubName};
	for (auto const& arg: _method.args)
	{
		awst::CallArg ca;
		ca.name = arg.name;
		auto varRef = std::make_shared<awst::VarExpression>();
		varRef->sourceLocation = _method.sourceLocation;
		varRef->wtype = arg.wtype;
		varRef->name = arg.name;
		ca.value = std::move(varRef);
		call->args.push_back(std::move(ca));
	}

	if (_method.returnType != awst::WType::voidType())
	{
		auto retStmt = std::make_shared<awst::ReturnStatement>();
		retStmt->sourceLocation = _method.sourceLocation;
		retStmt->value = std::move(call);
		entryBody->body.push_back(std::move(retStmt));
	}
	else
	{
		auto stmt = std::make_shared<awst::ExpressionStatement>();
		stmt->sourceLocation = _method.sourceLocation;
		stmt->expr = std::move(call);
		entryBody->body.push_back(std::move(stmt));
	}

	_method.body = entryBody;
}

void ContractBuilder::buildStorageDispatch(
	solidity::frontend::ContractDefinition const& _contract,
	awst::Contract* _contractNode,
	std::string const& _contractName
)
{
	StorageLayout layout;
	layout.computeLayout(_contract, m_typeMapper);

	if (layout.totalSlots() == 0)
		return;

	std::string cref = m_sourceFile + "." + _contractName;
	awst::SourceLocation loc;
	loc.file = m_sourceFile;

	auto makeUint64 = [&](std::string const& val) {
		auto c = std::make_shared<awst::IntegerConstant>();
		c->sourceLocation = loc;
		c->wtype = awst::WType::uint64Type();
		c->value = val;
		return c;
	};

	auto makeBytes = [&](std::string const& s) {
		auto c = std::make_shared<awst::BytesConstant>();
		c->sourceLocation = loc;
		c->wtype = awst::WType::bytesType();
		c->encoding = awst::BytesEncoding::Utf8;
		c->value = std::vector<uint8_t>(s.begin(), s.end());
		return c;
	};

	// ── __storage_read(slot: uint64) -> biguint ──
	{
		awst::ContractMethod readSub;
		readSub.sourceLocation = loc;
		readSub.cref = cref;
		readSub.memberName = "__storage_read";
		readSub.returnType = awst::WType::biguintType();
		readSub.arc4MethodConfig = std::nullopt;
		readSub.pure = false;

		awst::SubroutineArgument slotArg;
		slotArg.name = "__slot";
		slotArg.wtype = awst::WType::uint64Type();
		slotArg.sourceLocation = loc;
		readSub.args.push_back(slotArg);

		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = loc;

		// Build if/else chain for known slots
		// Start from innermost (default case) and wrap outward
		// Default: return biguint(0) (unknown slot)
		auto defaultBlock = std::make_shared<awst::Block>();
		defaultBlock->sourceLocation = loc;
		{
			auto ret = std::make_shared<awst::ReturnStatement>();
			ret->sourceLocation = loc;
			auto zero = std::make_shared<awst::IntegerConstant>();
			zero->sourceLocation = loc;
			zero->wtype = awst::WType::biguintType();
			zero->value = "0";
			ret->value = std::move(zero);
			defaultBlock->body.push_back(std::move(ret));
		}

		std::shared_ptr<awst::Statement> current;
		// Wrap default in a ReturnStatement block
		{
			auto exprStmt = std::make_shared<awst::ExpressionStatement>();
			exprStmt->sourceLocation = loc;
			// Just use the default block directly
		}
		// Build the chain bottom-up
		std::shared_ptr<awst::Block> elseBlock = defaultBlock;

		for (auto const& sv: layout.variables())
		{
			if (!sv.wtype || sv.wtype == awst::WType::voidType()) continue;

			// Condition: __slot == slotNumber
			auto slotVar = std::make_shared<awst::VarExpression>();
			slotVar->sourceLocation = loc;
			slotVar->wtype = awst::WType::uint64Type();
			slotVar->name = "__slot";

			auto cmp = std::make_shared<awst::NumericComparisonExpression>();
			cmp->sourceLocation = loc;
			cmp->wtype = awst::WType::boolType();
			cmp->lhs = slotVar;
			cmp->op = awst::NumericComparison::Eq;
			cmp->rhs = makeUint64(std::to_string(sv.slot));

			// If branch: return app_global_get(varName) as biguint
			auto ifBlock = std::make_shared<awst::Block>();
			ifBlock->sourceLocation = loc;
			{
				auto get = std::make_shared<awst::IntrinsicCall>();
				get->sourceLocation = loc;
				get->wtype = awst::WType::bytesType();
				get->opCode = "app_global_get";
				get->stackArgs.push_back(makeBytes(sv.name));

				// Pad to 32 bytes: concat(bzero(32), value), take last 32
				auto bz = std::make_shared<awst::IntrinsicCall>();
				bz->sourceLocation = loc;
				bz->wtype = awst::WType::bytesType();
				bz->opCode = "bzero";
				bz->stackArgs.push_back(makeUint64("32"));

				auto cat = std::make_shared<awst::IntrinsicCall>();
				cat->sourceLocation = loc;
				cat->wtype = awst::WType::bytesType();
				cat->opCode = "concat";
				cat->stackArgs.push_back(std::move(bz));
				cat->stackArgs.push_back(std::move(get));

				// Extract last 32 bytes
				auto lenCall = std::make_shared<awst::IntrinsicCall>();
				lenCall->sourceLocation = loc;
				lenCall->wtype = awst::WType::uint64Type();
				lenCall->opCode = "len";
				lenCall->stackArgs.push_back(cat);

				auto sub = std::make_shared<awst::UInt64BinaryOperation>();
				sub->sourceLocation = loc;
				sub->wtype = awst::WType::uint64Type();
				sub->left = std::move(lenCall);
				sub->op = awst::UInt64BinaryOperator::Sub;
				sub->right = makeUint64("32");

				auto extract = std::make_shared<awst::IntrinsicCall>();
				extract->sourceLocation = loc;
				extract->wtype = awst::WType::bytesType();
				extract->opCode = "extract3";
				extract->stackArgs.push_back(cat);
				extract->stackArgs.push_back(std::move(sub));
				extract->stackArgs.push_back(makeUint64("32"));

				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = loc;
				cast->wtype = awst::WType::biguintType();
				cast->expr = std::move(extract);

				auto ret = std::make_shared<awst::ReturnStatement>();
				ret->sourceLocation = loc;
				ret->value = std::move(cast);
				ifBlock->body.push_back(std::move(ret));
			}

			auto ifElse = std::make_shared<awst::IfElse>();
			ifElse->sourceLocation = loc;
			ifElse->condition = std::move(cmp);
			ifElse->ifBranch = std::move(ifBlock);
			ifElse->elseBranch = std::move(elseBlock);

			auto newElse = std::make_shared<awst::Block>();
			newElse->sourceLocation = loc;
			newElse->body.push_back(std::move(ifElse));
			elseBlock = std::move(newElse);
		}

		// The outermost block is the body
		for (auto& stmt: elseBlock->body)
			body->body.push_back(std::move(stmt));

		readSub.body = body;
		_contractNode->methods.push_back(std::move(readSub));
	}

	// ── __storage_write(slot: uint64, value: biguint) -> void ──
	{
		awst::ContractMethod writeSub;
		writeSub.sourceLocation = loc;
		writeSub.cref = cref;
		writeSub.memberName = "__storage_write";
		writeSub.returnType = awst::WType::voidType();
		writeSub.arc4MethodConfig = std::nullopt;
		writeSub.pure = false;

		awst::SubroutineArgument slotArg;
		slotArg.name = "__slot";
		slotArg.wtype = awst::WType::uint64Type();
		slotArg.sourceLocation = loc;
		writeSub.args.push_back(slotArg);

		awst::SubroutineArgument valArg;
		valArg.name = "__value";
		valArg.wtype = awst::WType::biguintType();
		valArg.sourceLocation = loc;
		writeSub.args.push_back(valArg);

		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = loc;

		// Build if/else chain for known slots
		auto defaultBlock = std::make_shared<awst::Block>();
		defaultBlock->sourceLocation = loc;
		// Default: no-op (unknown slot, silently ignore)
		{
			auto ret = std::make_shared<awst::ReturnStatement>();
			ret->sourceLocation = loc;
			defaultBlock->body.push_back(std::move(ret));
		}

		std::shared_ptr<awst::Block> elseBlock = defaultBlock;

		for (auto const& sv: layout.variables())
		{
			if (!sv.wtype || sv.wtype == awst::WType::voidType()) continue;

			auto slotVar = std::make_shared<awst::VarExpression>();
			slotVar->sourceLocation = loc;
			slotVar->wtype = awst::WType::uint64Type();
			slotVar->name = "__slot";

			auto cmp = std::make_shared<awst::NumericComparisonExpression>();
			cmp->sourceLocation = loc;
			cmp->wtype = awst::WType::boolType();
			cmp->lhs = slotVar;
			cmp->op = awst::NumericComparison::Eq;
			cmp->rhs = makeUint64(std::to_string(sv.slot));

			auto ifBlock = std::make_shared<awst::Block>();
			ifBlock->sourceLocation = loc;
			{
				// app_global_put(varName, pad32(value_as_bytes))
				// Pad to 32 bytes to match EVM slot semantics
				auto valueVar = std::make_shared<awst::VarExpression>();
				valueVar->sourceLocation = loc;
				valueVar->wtype = awst::WType::biguintType();
				valueVar->name = "__value";

				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = loc;
				cast->wtype = awst::WType::bytesType();
				cast->expr = std::move(valueVar);

				// concat(bzero(32), bytes) → take last 32 bytes
				auto bz = std::make_shared<awst::IntrinsicCall>();
				bz->sourceLocation = loc;
				bz->wtype = awst::WType::bytesType();
				bz->opCode = "bzero";
				bz->stackArgs.push_back(makeUint64("32"));

				auto cat = std::make_shared<awst::IntrinsicCall>();
				cat->sourceLocation = loc;
				cat->wtype = awst::WType::bytesType();
				cat->opCode = "concat";
				cat->stackArgs.push_back(std::move(bz));
				cat->stackArgs.push_back(std::move(cast));

				auto lenCall = std::make_shared<awst::IntrinsicCall>();
				lenCall->sourceLocation = loc;
				lenCall->wtype = awst::WType::uint64Type();
				lenCall->opCode = "len";
				lenCall->stackArgs.push_back(cat);

				auto sub32 = std::make_shared<awst::UInt64BinaryOperation>();
				sub32->sourceLocation = loc;
				sub32->wtype = awst::WType::uint64Type();
				sub32->left = std::move(lenCall);
				sub32->op = awst::UInt64BinaryOperator::Sub;
				sub32->right = makeUint64("32");

				auto extract = std::make_shared<awst::IntrinsicCall>();
				extract->sourceLocation = loc;
				extract->wtype = awst::WType::bytesType();
				extract->opCode = "extract3";
				extract->stackArgs.push_back(cat);
				extract->stackArgs.push_back(std::move(sub32));
				extract->stackArgs.push_back(makeUint64("32"));

				auto put = std::make_shared<awst::IntrinsicCall>();
				put->sourceLocation = loc;
				put->wtype = awst::WType::voidType();
				put->opCode = "app_global_put";
				put->stackArgs.push_back(makeBytes(sv.name));
				put->stackArgs.push_back(std::move(extract));

				auto stmt = std::make_shared<awst::ExpressionStatement>();
				stmt->sourceLocation = loc;
				stmt->expr = std::move(put);
				ifBlock->body.push_back(std::move(stmt));

				auto ret = std::make_shared<awst::ReturnStatement>();
				ret->sourceLocation = loc;
				ifBlock->body.push_back(std::move(ret));
			}

			auto ifElse = std::make_shared<awst::IfElse>();
			ifElse->sourceLocation = loc;
			ifElse->condition = std::move(cmp);
			ifElse->ifBranch = std::move(ifBlock);
			ifElse->elseBranch = std::move(elseBlock);

			auto newElse = std::make_shared<awst::Block>();
			newElse->sourceLocation = loc;
			newElse->body.push_back(std::move(ifElse));
			elseBlock = std::move(newElse);
		}

		for (auto& stmt: elseBlock->body)
			body->body.push_back(std::move(stmt));

		writeSub.body = body;
		_contractNode->methods.push_back(std::move(writeSub));
	}

	Logger::instance().debug(
		"Generated __storage_read/__storage_write dispatch for "
		+ std::to_string(layout.totalSlots()) + " slots", loc);
}

} // namespace puyasol::builder
