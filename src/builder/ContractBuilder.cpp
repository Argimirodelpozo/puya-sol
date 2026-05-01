#include "builder/ContractBuilder.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-ast/calls/SolNewExpression.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-eb/FunctionPointerBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/storage/StorageLayout.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

#include <boost/multiprecision/cpp_int.hpp>
#include <map>
#include <set>

namespace puyasol::builder
{

/// Checks if a Solidity AST subtree references any state variable whose AST ID
/// is in the given set (i.e. box-stored state variables).

/// Collects local variable declarations inside a statement subtree (e.g. a
/// modifier body) so the inliner can rename them uniquely per application.
/// Without this, `modifier mod(uint x) { uint b = x; _; assert(b == x); }`
/// applied twice shares a single `b` slot across both instances.

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


ContractBuilder::ContractBuilder(
	TypeMapper& _typeMapper,
	StorageMapper& _storageMapper,
	std::string const& _sourceFile,
	LibraryFunctionIdMap const& _libraryFunctionIds,
	uint64_t _opupBudget,
	FreeFunctionIdMap const& _freeFunctionById,
	std::map<std::string, uint64_t> const& _ensureBudget,
	bool _viaIR
)
	: m_typeMapper(_typeMapper),
	  m_storageMapper(_storageMapper),
	  m_sourceFile(_sourceFile),
	  m_libraryFunctionIds(_libraryFunctionIds),
	  m_opupBudget(_opupBudget),
	  m_freeFunctionById(_freeFunctionById),
	  m_ensureBudget(_ensureBudget),
	  m_viaIR(_viaIR)
{
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

void ContractBuilder::prependNonPayableCheck(awst::ContractMethod& _method)
{
	// Only externally-callable methods get the check; others aren't reached
	// via ARC4 dispatch so a preceding Payment is irrelevant.
	if (!_method.arc4MethodConfig.has_value())
		return;
	if (!_method.body)
		return;

	auto loc = _method.sourceLocation;

	auto groupIdx = awst::makeIntrinsicCall("txn", awst::WType::uint64Type(), loc);
	groupIdx->immediates = {std::string("GroupIndex")};

	auto hasPayment = awst::makeNumericCompare(
		groupIdx, awst::NumericComparison::Gt,
		awst::makeIntegerConstant("0", loc), loc);

	auto groupIdx2 = awst::makeIntrinsicCall("txn", awst::WType::uint64Type(), loc);
	groupIdx2->immediates = {std::string("GroupIndex")};
	auto payIdx = awst::makeUInt64BinOp(
		std::move(groupIdx2), awst::UInt64BinaryOperator::Sub,
		awst::makeIntegerConstant("1", loc), loc);

	auto amount = awst::makeIntrinsicCall("gtxns", awst::WType::uint64Type(), loc);
	amount->immediates = {std::string("Amount")};
	amount->stackArgs.push_back(std::move(payIdx));

	// Match msg.value's ConditionalExpression shape — avoids evaluating
	// GroupIndex - 1 when GroupIndex == 0 (underflow-safe).
	auto msgValue = std::make_shared<awst::ConditionalExpression>();
	msgValue->sourceLocation = loc;
	msgValue->wtype = awst::WType::uint64Type();
	msgValue->condition = std::move(hasPayment);
	msgValue->trueExpr = std::move(amount);
	msgValue->falseExpr = awst::makeIntegerConstant("0", loc);

	auto isZero = awst::makeNumericCompare(
		std::move(msgValue), awst::NumericComparison::Eq,
		awst::makeIntegerConstant("0", loc), loc);

	auto assertStmt = awst::makeExpressionStatement(
		awst::makeAssert(std::move(isZero), loc, "not payable"), loc);
	_method.body->body.insert(_method.body->body.begin(), std::move(assertStmt));
}

std::shared_ptr<awst::Contract> ContractBuilder::build(
	solidity::frontend::ContractDefinition const& _contract
)
{
	m_currentContract = &_contract;
	std::string contractName = _contract.name();
	std::string contractId = m_sourceFile + "." + contractName;

	// Reset the recursive-Yul subroutine sink so assembly blocks within this
	// contract can register their emitted Subroutines and we drain them below.
	AssemblyBuilder::resetPendingSubroutines();

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
	m_exprBuilder = std::make_unique<eb::BuilderContext>(
		m_typeMapper, m_storageMapper, m_sourceFile, contractName,
		m_libraryFunctionIds, m_overloadedNames, m_freeFunctionById
	);
	m_exprBuilder->currentContract = &_contract;
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
		[this]() { return m_exprBuilder->takePrePending(); },
		[this]() { return m_exprBuilder->takePending(); },
		{}, nullptr, {}, nullptr, nullptr, nullptr,
	};

	m_exprBuilder->transientStorage =
		m_transientStorage.hasTransientVars() ? &m_transientStorage : nullptr;

	// Set the contract cref for function pointer dispatch resolution.
	// Library subroutines need this to construct SubroutineIDs.
	eb::FunctionPointerBuilder::setCurrentCref(contractId);

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
	m_superTargetFuncs.clear();
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
				m_superTargetFuncs[callerId] = mroTarget;
			}
		}

		// Constructor super.f(): the most-derived contract's constructor
		// sits conceptually one position above chain[0] in the MRO. Its
		// super target is whichever `f` appears first in the constructor
		// body's super.f() reference. Emit a caller-keyed subroutine whose
		// body is that target — the constructor call site will look up
		// `f__super_<ctor.id>` via m_perFuncSuperOverrides.
		if (auto const* ctor = _contract.constructor())
		{
			if (ctor->isImplemented())
			{
				SuperCallCollector ctorCollector;
				ctor->body().accept(ctorCollector);
				for (int64_t superCallTargetId: ctorCollector.superTargetIds)
				{
					// Find the target function in the MRO chain.
					solidity::frontend::FunctionDefinition const* target = nullptr;
					for (auto const& entry: chain)
						if (entry.func->id() == superCallTargetId)
						{ target = entry.func; break; }
					if (!target)
						continue;

					int64_t callerId = ctor->id();
					std::string name = fname;
					if (m_overloadedNames.count(name))
						name += "(" + std::to_string(target->parameters().size()) + ")";
					std::string superName = name + "__super_" + std::to_string(callerId);
					m_perFuncSuperOverrides[callerId].push_back({superCallTargetId, superName});
					m_superTargetFuncs[callerId] = target;
					m_exprBuilder->superTargetNames[superCallTargetId] = superName;
				}
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
						m_exprBuilder->superTargetNames[id] = superName;
					}
				}
			}
		}
	}

	// Handle explicit base class calls (Base.f() — NOT super.f()).
	// These always resolve to the specific base function, regardless of MRO.
	// Collect from ALL functions in the hierarchy.
	m_explicitBaseTargetFuncs.clear();
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
						m_explicitBaseTargetFuncs[id] = func;
						std::string name = func->name();
						if (m_overloadedNames.count(name))
							name += "(" + std::to_string(func->parameters().size()) + ")";
						std::string superName = name + "__super_" + std::to_string(id);
						// Register globally — explicit base calls don't need per-function context
						m_exprBuilder->superTargetNames[id] = superName;
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
				m_exprBuilder->superTargetNames[targetId] = superName;
		// Re-register fallback super targets (cross-function super calls)
		for (auto const& [id, func]: m_fallbackSuperFuncs)
		{
			std::string name = func->name();
			if (m_overloadedNames.count(name))
				name += "_" + std::to_string(func->parameters().size());
			m_exprBuilder->superTargetNames[id] = name + "__super_" + std::to_string(id);
		}
		// Re-register explicit base targets (they're fixed, not MRO-dependent)
		for (auto const& [id, func]: m_explicitBaseTargetFuncs)
		{
			std::string name = func->name();
			if (m_overloadedNames.count(name))
				name += "(" + std::to_string(func->parameters().size()) + ")";
			m_exprBuilder->superTargetNames[id] = name + "__super_" + std::to_string(id);
		}
	};

	// Helper: clear super targets (to avoid cross-contamination between functions)
	auto clearSuperOverrides = [&]() {
		m_exprBuilder->superTargetNames.clear();
	};

	// Snapshot super target registrations so the constructor body —
	// translated inside buildApprovalProgram below — can resolve `super.f()`
	// to the eventually-emitted `f__super_N` subroutine instead of falling
	// back to the current contract's own `f`.
	m_allSuperTargetNames = m_exprBuilder->superTargetNames;

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
		ctorPendingState.key = awst::makeUtf8BytesConstant(
			"__ctor_pending", ctorPendingState.sourceLocation);
		contract->appState.push_back(std::move(ctorPendingState));

		contract->methods.push_back(std::move(*m_postInitMethod));
		m_postInitMethod.reset();
	}


	// m_allSuperTargetNames was pre-populated before buildApprovalProgram
	// so constructor bodies could resolve `super.f()`. Nothing to do here.

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
		// Fallback/receive functions have empty names in Solidity. Give them
		// explicit memberName so we can reference them from the approval
		// program's custom dispatch (InstanceMethodTarget).
		std::string nameOverride;
		if (func->isFallback())
			nameOverride = "__fallback";
		else if (func->isReceive())
			nameOverride = "__receive";
		auto method = buildFunction(*func, contractName, nameOverride);
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
					readExpr = TypeCoercion::implicitNumericCast(
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
							auto cast = awst::makeReinterpretCast(std::move(readExpr), getter.returnType, loc);
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

					// Transient state vars: route through TransientStorage
					// (scratch slot TRANSIENT_SLOT packed blob) so the getter
					// sees the same storage as direct named-var reads/writes.
					if (var->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Transient
						&& m_transientStorage.isTransient(*var))
					{
						readExpr = m_transientStorage.buildRead(var->name(), readType, loc);
					}
					if (!readExpr)
						readExpr = m_storageMapper.createStateRead(
							var->name(), readType, storageKind, loc
						);
				}
			}
			else if (dynamic_cast<solidity::frontend::ArrayType const*>(var->type())
				&& !dynamic_cast<solidity::frontend::ArrayType const*>(var->type())->isByteArrayOrString()
				&& getter.args.size() == 1)
			{
				// Array state var `T[]` / `T[N] public array` → getter(uint256 i).
				// The backing store is a single state slot (box for dynamic /
				// oversized static, AppGlobal otherwise) containing the packed
				// ARC4 array; reading element i uses IndexExpression on that
				// slot, NOT a sha256-based mapping key.
				auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(var->type());
				auto* arrWType = m_typeMapper.map(arrType);
				auto* elemARC4 = m_typeMapper.mapSolTypeToARC4(arrType->baseType());

				auto storageKind = StorageMapper::shouldUseBoxStorage(*var)
					? awst::AppStorageKind::Box
					: awst::AppStorageKind::AppGlobal;

				auto arrayRead = m_storageMapper.createStateRead(
					var->name(), arrWType, storageKind, loc
				);

				auto idxRef = awst::makeVarExpression(getter.args[0].name, getter.args[0].wtype, loc);
				auto idx = TypeCoercion::implicitNumericCast(
					idxRef, awst::WType::uint64Type(), loc);

				auto indexExpr = std::make_shared<awst::IndexExpression>();
				indexExpr->sourceLocation = loc;
				indexExpr->wtype = elemARC4;
				indexExpr->base = std::move(arrayRead);
				indexExpr->index = std::move(idx);

				// Decode ARC4 element back to native type (e.g. arc4.uint256 → biguint)
				auto* nativeElem = m_typeMapper.map(arrType->baseType());
				std::shared_ptr<awst::Expression> result = std::move(indexExpr);

				// Struct element with multi-field getter signature: decompose
				// the ARC4 struct into a tuple of non-mapping, non-dynamic-array
				// fields. Matches Solidity's public-accessor behavior for
				// `Struct[N] public p` where the getter returns the primitive
				// fields flat rather than the struct itself.
				auto const* solStructElem = dynamic_cast<solidity::frontend::StructType const*>(arrType->baseType());
				if (solStructElem && solReturnTypes.size() > 1)
				{
					auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(elemARC4);
					auto tuple = std::make_shared<awst::TupleExpression>();
					tuple->sourceLocation = loc;
					tuple->wtype = getter.returnType;

					for (auto const& member: solStructElem->members(nullptr))
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
						fieldExpr->base = result;
						fieldExpr->name = member.name;

						auto* nativeFieldType = m_typeMapper.map(member.type);
						if (arc4FieldType && arc4FieldType != nativeFieldType)
						{
							auto decode = std::make_shared<awst::ARC4Decode>();
							decode->sourceLocation = loc;
							decode->wtype = nativeFieldType;
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
					if (elemARC4 != nativeElem && elemARC4->name() != nativeElem->name())
					{
						auto decode = std::make_shared<awst::ARC4Decode>();
						decode->sourceLocation = loc;
						decode->wtype = nativeElem;
						decode->value = std::move(result);
						result = std::move(decode);
					}

					readExpr = std::move(result);
				}
			}
			else
			{
				// Mapping/array getter — build box read with key from mapping arguments,
				// then index into the stored value for each nested array dimension.
				// Walk mappings first to determine how many args feed the box key.
				solidity::frontend::Type const* valueType = var->type();
				size_t keyArgCount = 0;
				while (keyArgCount < getter.args.size())
				{
					if (auto const* mt = dynamic_cast<solidity::frontend::MappingType const*>(valueType))
					{
						valueType = mt->valueType();
						keyArgCount++;
					}
					else
						break;
				}

				// The type stored in the box (may be a nested array/struct).
				// Remaining args index into this value after the box read.
				solidity::frontend::Type const* storedValueType = valueType;

				// Walk nested arrays for index args.
				size_t indexArgCount = 0;
				solidity::frontend::Type const* elemType = storedValueType;
				while (keyArgCount + indexArgCount < getter.args.size())
				{
					if (auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(elemType))
					{
						if (at->isByteArrayOrString())
							break;
						elemType = at->baseType();
						indexArgCount++;
					}
					else
						break;
				}

				// Map the box-stored type (before indexing).
				awst::WType const* storedWType = m_typeMapper.map(storedValueType);
				// The unwound value type used for struct-field decomposition below
				// is the element type after indexing.
				valueType = elemType;

				std::shared_ptr<awst::Expression> storageRead;
				if (keyArgCount == 0)
				{
					// No mapping keys — the state var is a plain multi-dim array.
					// Use a regular state read (box/app-global) of the whole value.
					auto storageKind = StorageMapper::shouldUseBoxStorage(*var)
						? awst::AppStorageKind::Box
						: awst::AppStorageKind::AppGlobal;
					storageRead = m_storageMapper.createStateRead(
						var->name(), storedWType, storageKind, loc);
				}
				else
				{
				// Build the box key from the getter arguments.
				// Each arg is converted to bytes, concatenated, then sha256-hashed.
				auto prefix = awst::makeUtf8BytesConstant(
					var->name(), loc, awst::WType::boxKeyType());

				std::vector<std::shared_ptr<awst::Expression>> keyParts;
				for (size_t i = 0; i < keyArgCount; ++i)
				{
					auto argRef = awst::makeVarExpression(getter.args[i].name, getter.args[i].wtype, loc);

					std::shared_ptr<awst::Expression> keyBytes;
					if (argRef->wtype == awst::WType::uint64Type())
					{
						auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), loc);
						itob->stackArgs.push_back(std::move(argRef));
						keyBytes = std::move(itob);
					}
					else if (argRef->wtype == awst::WType::biguintType())
					{
						// Normalize biguint to exactly 32 bytes before hashing.
						auto reinterpret = awst::makeReinterpretCast(std::move(argRef), awst::WType::bytesType(), loc);

						auto padWidth = awst::makeIntegerConstant("32", loc);

						auto pad = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), loc);
						pad->stackArgs.push_back(std::move(padWidth));

						auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), loc);
						cat->stackArgs.push_back(std::move(pad));
						cat->stackArgs.push_back(std::move(reinterpret));

						auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), loc);
						lenCall->stackArgs.push_back(cat);

						auto width32 = awst::makeIntegerConstant("32", loc);

						auto offset = awst::makeIntrinsicCall("-", awst::WType::uint64Type(), loc);
						offset->stackArgs.push_back(std::move(lenCall));
						offset->stackArgs.push_back(std::move(width32));

						auto width32b = awst::makeIntegerConstant("32", loc);

						auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), loc);
						extract->stackArgs.push_back(std::move(cat));
						extract->stackArgs.push_back(std::move(offset));
						extract->stackArgs.push_back(std::move(width32b));

						keyBytes = std::move(extract);
					}
					else
					{
						// string / bytes / address → ReinterpretCast to bytes
						auto reinterpret = awst::makeReinterpretCast(std::move(argRef), awst::WType::bytesType(), loc);
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
						auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), loc);
						concat->stackArgs.push_back(std::move(compositeKey));
						concat->stackArgs.push_back(std::move(keyParts[i]));
						compositeKey = std::move(concat);
					}
				}

				// Hash the composite key
				auto hashCall = awst::makeIntrinsicCall("sha256", awst::WType::bytesType(), loc);
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

				storageRead = std::move(stateGet);
				} // end keyArgCount > 0 branch

				// Apply index accesses for any array dimensions nested inside the box value
				// (e.g. `mapping(K => T[N])` keys by K, then indexes into T[N]).
				std::shared_ptr<awst::Expression> indexed = std::move(storageRead);
				{
					solidity::frontend::Type const* walkType = storedValueType;
					for (size_t i = 0; i < indexArgCount; ++i)
					{
						auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(walkType);
						if (!at)
							break;
						auto* elemARC4 = m_typeMapper.mapSolTypeToARC4(at->baseType());

						auto idxRef = awst::makeVarExpression(
							getter.args[keyArgCount + i].name,
							getter.args[keyArgCount + i].wtype, loc);
						auto idx = TypeCoercion::implicitNumericCast(
							idxRef, awst::WType::uint64Type(), loc);

						auto indexExpr = std::make_shared<awst::IndexExpression>();
						indexExpr->sourceLocation = loc;
						indexExpr->wtype = elemARC4;
						indexExpr->base = std::move(indexed);
						indexExpr->index = std::move(idx);
						indexed = std::move(indexExpr);

						walkType = at->baseType();
					}
				}

				// If the stored type is a struct but the getter returns a tuple
				// of selected fields, extract and ARC4-decode each field.
				if (auto const* structType = dynamic_cast<solidity::frontend::StructType const*>(valueType))
				{
					if (solReturnTypes.size() > 1)
					{
						// indexed returns the full ARC4Struct; extract fields.
						std::shared_ptr<awst::Expression> fullStruct = std::move(indexed);
						auto tuple = std::make_shared<awst::TupleExpression>();
						tuple->sourceLocation = loc;
						tuple->wtype = getter.returnType;

						// Get the ARC4Struct type's field types for FieldExpression
						auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(fullStruct->wtype);

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
						readExpr = std::move(indexed);
					}
				}
				else
				{
					readExpr = std::move(indexed);

					// Decode ARC4 element to native getter return type if needed
					// (e.g. indexing into ARC4StaticArray<uint8,N> gives arc4.uint8 → uint64).
					if (readExpr && readExpr->wtype && readExpr->wtype != getter.returnType)
					{
						auto const* arc4Elem = dynamic_cast<awst::ARC4UIntN const*>(readExpr->wtype);
						if (arc4Elem && (getter.returnType == awst::WType::uint64Type()
							|| getter.returnType == awst::WType::biguintType()))
						{
							auto decode = std::make_shared<awst::ARC4Decode>();
							decode->sourceLocation = loc;
							decode->wtype = getter.returnType;
							decode->value = std::move(readExpr);
							readExpr = std::move(decode);
						}
					}
				}
			}

			// Sign-extend getter return for signed integer types
			if (signedGetterBits > 0 && readExpr)
			{
				readExpr = TypeCoercion::signExtendToUint256(std::move(readExpr), signedGetterBits, loc);
			}

			// ABI v2 validation for getter params (enum keys in mappings)
			bool getterV2 = true;
			{
				auto const& ann = _contract.sourceUnit().annotation();
				if (ann.useABICoderV2.set())
					getterV2 = *ann.useABICoderV2;
			}
			if (getterV2)
			{
				for (size_t pi = 0; pi < solParamTypes.size(); ++pi)
				{
					auto const* pt = solParamTypes[pi];
					// Enum validation
					if (auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(pt))
					{
						unsigned memberCount = enumType->numberOfMembers();
						std::string pname = (pi < solParamNames.size() && !solParamNames[pi].empty())
							? solParamNames[pi] : "key" + std::to_string(pi);

						auto pv = awst::makeVarExpression(pname, awst::WType::uint64Type(), loc);

						auto mv = awst::makeIntegerConstant(std::to_string(memberCount - 1), loc);

						auto cmp = awst::makeNumericCompare(pv, awst::NumericComparison::Lte, std::move(mv), loc);

						auto as = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), loc, "enum validation"), loc);
						body->body.push_back(std::move(as));
					}
				}
			}

			auto ret = awst::makeReturnStatement(std::move(readExpr), loc);
			body->body.push_back(std::move(ret));

			getter.body = body;

			// Remap biguint (uint256) getter parameters to ARC4UIntN(256) so the
			// ABI selector encodes as "uint256" (not "uint512"). Rename the arg to
			// __arc4_<name> and insert a decode statement at the top of the body.
			{
				std::vector<std::shared_ptr<awst::Statement>> decodeStmts;
				for (auto& garg: getter.args)
				{
					if (garg.wtype != awst::WType::biguintType())
						continue;
					auto const* arc4Type = m_typeMapper.createType<awst::ARC4UIntN>(256);
					std::string origName = garg.name;
					std::string arc4Name = "__arc4_" + origName;
					garg.wtype = arc4Type;
					garg.name = arc4Name;

					auto arc4Var = awst::makeVarExpression(arc4Name, arc4Type, loc);

					auto decode = std::make_shared<awst::ARC4Decode>();
					decode->sourceLocation = loc;
					decode->wtype = awst::WType::biguintType();
					decode->value = std::move(arc4Var);

					auto target = awst::makeVarExpression(origName, awst::WType::biguintType(), loc);

					auto assign = awst::makeAssignmentStatement(std::move(target), std::move(decode), loc);
					decodeStmts.push_back(std::move(assign));
				}
				if (!decodeStmts.empty())
					getter.body->body.insert(
						getter.body->body.begin(),
						std::make_move_iterator(decodeStmts.begin()),
						std::make_move_iterator(decodeStmts.end())
					);
			}

			// Remap biguint return type to ARC4UIntN(N) so the ABI selector
			// uses Solidity's declared "uintN" signature, not the internal
			// "uint512" marker. Matches the wrap applied to regular method
			// returns; without this, external callers of `c.x()` compute
			// selector sha512_256("x()uint256") but the contract's dispatch
			// emitted sha512_256("x()uint512"), producing a `match` miss.
			//
			// Only applied to UNSIGNED integer returns: signed returns
			// (including signedGetterBits sign-extension) encode as two's
			// complement in biguint and need a different ARC4 path. Wrapping
			// a two's-complement biguint as ARC4UIntN would report overflow.
			bool isUnsignedIntReturn = false;
			unsigned retBits = 256;
			if (getter.returnType == awst::WType::biguintType()
				&& solReturnTypes.size() == 1)
			{
				auto const* retSolType = solReturnTypes[0];
				if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(retSolType))
					retSolType = &udvt->underlyingType();
				if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(retSolType))
				{
					if (!intType->isSigned())
					{
						isUnsignedIntReturn = true;
						retBits = intType->numBits();
					}
				}
			}
			if (isUnsignedIntReturn)
			{
				auto const* arc4RetType = m_typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(retBits));
				std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> wrap;
				wrap = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts) {
					for (auto& stmt : stmts) {
						if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get())) {
							if (ret->value && ret->value->wtype == awst::WType::biguintType()) {
								auto encode = std::make_shared<awst::ARC4Encode>();
								encode->sourceLocation = ret->value->sourceLocation;
								encode->wtype = arc4RetType;
								encode->value = std::move(ret->value);
								ret->value = std::move(encode);
							}
						} else if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get())) {
							if (ifElse->ifBranch) wrap(ifElse->ifBranch->body);
							if (ifElse->elseBranch) wrap(ifElse->elseBranch->body);
						} else if (auto* block = dynamic_cast<awst::Block*>(stmt.get())) {
							wrap(block->body);
						}
					}
				};
				wrap(getter.body->body);
				getter.returnType = arc4RetType;
			}

			// Non-payable check for public state-variable getters
			// (auto-generated getters are always view, never payable).
			prependNonPayableCheck(getter);

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
			std::string nameOverride2;
			if (func->isFallback())
				nameOverride2 = "__fallback";
			else if (func->isReceive())
				nameOverride2 = "__receive";
			auto method = buildFunction(*func, contractName, nameOverride2);
			contract->methods.push_back(std::move(method));
			for (auto& sub: m_modifierSubroutines)
				contract->methods.push_back(std::move(sub));
			m_modifierSubroutines.clear();
		}
	}

	// Clear super overrides before emitting super subroutines
	clearSuperOverrides();

	// Emit MRO-dependent super subroutines (keyed by caller func AST ID)
	for (auto const& [callerFuncId, targetFunc]: m_superTargetFuncs)
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
	for (auto const& [targetId, func]: m_explicitBaseTargetFuncs)
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

	// Generate function pointer dispatch tables
	{
		// Set subroutine IDs for library/free function targets so dispatch
		// uses SubroutineID (resolvable by puya) instead of InstanceMethodTarget.
		eb::FunctionPointerBuilder::setSubroutineIds(m_freeFunctionById);

		std::string cref = m_sourceFile + "." + contractName;
		awst::SourceLocation loc;
		loc.file = m_sourceFile;
		auto& dispCtx = *m_exprBuilder;
		auto dispatchMethods = eb::FunctionPointerBuilder::generateDispatchMethods(
			dispCtx, cref, loc, &m_dispatchSubroutines);
		for (auto& m : dispatchMethods)
			contract->methods.push_back(std::move(m));
		eb::FunctionPointerBuilder::reset();
	}

	// Drain any Subroutines emitted for recursive Yul functions so the
	// contract-builder caller picks them up alongside fn-ptr dispatchers.
	{
		auto yulSubs = AssemblyBuilder::takePendingSubroutines();
		for (auto& sub: yulSubs)
			m_dispatchSubroutines.push_back(std::move(sub));
	}

	return contract;
}



} // namespace puyasol::builder
