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

/// Collects local variable declarations inside a statement subtree (e.g. a
/// modifier body) so the inliner can rename them uniquely per application.
/// Without this, `modifier mod(uint x) { uint b = x; _; assert(b == x); }`
/// applied twice shares a single `b` slot across both instances.
class LocalVarDeclCollector: public solidity::frontend::ASTConstVisitor
{
public:
	std::vector<solidity::frontend::VariableDeclaration const*> decls;

	bool visit(solidity::frontend::VariableDeclaration const& _node) override
	{
		if (!_node.isStateVariable() && !_node.isConstant())
			decls.push_back(&_node);
		return true;
	}
};

/// Detects whether a statement subtree contains any `assembly { ... }` block.
/// Used to skip modifier local-var renaming when the body has inline assembly:
/// Yul identifiers reference their original Solidity names, and renaming would
/// produce a mismatch between the declaration slot and the assembly slot.
class InlineAssemblyDetector: public solidity::frontend::ASTConstVisitor
{
public:
	bool found = false;
	bool visit(solidity::frontend::InlineAssembly const&) override
	{
		found = true;
		return false;
	}
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
	if (auto const* inner = dynamic_cast<awst::Block const*>(last.get()))
		return blockAlwaysTerminates(*inner);
	return false;
}

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
	m_exprBuilder->setCurrentContract(&_contract);
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
					m_exprBuilder->addSuperTarget(superCallTargetId, superName);
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
						m_exprBuilder->addSuperTarget(id, superName);
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
		for (auto const& [id, func]: m_explicitBaseTargetFuncs)
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

	// Snapshot super target registrations so the constructor body —
	// translated inside buildApprovalProgram below — can resolve `super.f()`
	// to the eventually-emitted `f__super_N` subroutine instead of falling
	// back to the current contract's own `f`.
	m_allSuperTargetNames = m_exprBuilder->superTargetNames();

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
				auto idx = ExpressionBuilder::implicitNumericCast(
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
						auto idx = ExpressionBuilder::implicitNumericCast(
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
		auto dispatchMethods = eb::FunctionPointerBuilder::generateDispatchMethods(
			cref, loc, &m_dispatchSubroutines);
		for (auto& m : dispatchMethods)
			contract->methods.push_back(std::move(m));
		eb::FunctionPointerBuilder::reset();
	}

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

	// Force __postInit if the constructor (or state var initializers)
	// contains `new C()` — the inner create/fund txns need the parent
	// to already have balance, which only happens after deployment.
	if (!needsPostInit)
	{
		struct NewExprChecker: public solidity::frontend::ASTConstVisitor
		{
			bool found = false;
			bool visit(solidity::frontend::NewExpression const&) override
			{ found = true; return false; }
		};
		NewExprChecker newChecker;
		// Check state variable initializers
		for (auto const* base: _contract.annotation().linearizedBaseContracts)
			for (auto const* var: base->stateVariables())
				if (var->value())
					var->value()->accept(newChecker);
		// Check constructor body
		if (auto const* ctor = _contract.constructor())
			if (ctor->isImplemented())
				ctor->body().accept(newChecker);
		if (newChecker.found)
		{
			needsPostInit = true;
			Logger::instance().debug("Forcing __postInit: constructor/state-init deploys child contracts via new C()");
		}
	}

	// Force __postInit when any state-var initializer or constructor body
	// references msg.value / msg.sender / msg.data. At AppCreate time these
	// read from the caller's group context (e.g. msg.value sees Amount of the
	// preceding group txn), which is correct when the contract is deployed
	// by a PaymentTxn-preceded ApplicationCreateTxn. But when this contract
	// is deployed as a CHILD via `new C{value: N}()`, the parent's
	// SolNewExpression groups the Payment+__postInit call (not the Payment+
	// AppCreate), so msg.value is only visible inside __postInit. Deferring
	// the initializer is the simplest way to make `new C{value:N}(...)`
	// with msg.value semantics work.
	if (!needsPostInit)
	{
		struct MsgRefChecker: public solidity::frontend::ASTConstVisitor
		{
			bool found = false;
			bool visit(solidity::frontend::MemberAccess const& _ma) override
			{
				if (auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(&_ma.expression()))
				{
					if (id->name() == "msg"
						&& (_ma.memberName() == "value"
							|| _ma.memberName() == "sender"
							|| _ma.memberName() == "data"))
						found = true;
				}
				return !found;
			}
		};
		MsgRefChecker msgChecker;
		for (auto const* base: _contract.annotation().linearizedBaseContracts)
			for (auto const* var: base->stateVariables())
				if (var->value())
					var->value()->accept(msgChecker);
		if (auto const* ctor = _contract.constructor())
			if (ctor->isImplemented())
				ctor->body().accept(msgChecker);
		if (msgChecker.found)
		{
			needsPostInit = true;
			Logger::instance().debug("Forcing __postInit: constructor/state-init references msg.*");
		}
	}

	// Create-time check: if (Txn.ApplicationID == 0) { base_ctors; ctor_body; return true; }
	{
		auto appIdCheck = std::make_shared<awst::IntrinsicCall>();
		appIdCheck->sourceLocation = method.sourceLocation;
		appIdCheck->opCode = "txn";
		appIdCheck->immediates = {std::string("ApplicationID")};
		appIdCheck->wtype = awst::WType::uint64Type();

		auto zero = awst::makeIntegerConstant("0", method.sourceLocation);

		auto isCreate = awst::makeNumericCompare(appIdCheck, awst::NumericComparison::Eq, zero, method.sourceLocation);

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

				auto* wtype = m_typeMapper.map(var->type());

				// Box-stored ARC4 struct with explicit initializer: encode
				// the initializer and box_put it. Box arrays/bytes/dyn
				// arrays are handled by the dedicated m_boxArrayVarNames
				// loop above, so skip those kinds here.
				if (kind == awst::AppStorageKind::Box)
				{
					if (!var->value())
						continue;
					bool isStructBox = wtype
						&& wtype->kind() == awst::WTypeKind::ARC4Struct;
					if (!isStructBox)
						continue;
					auto initVal = m_exprBuilder->build(*var->value());
					if (!initVal)
						continue;
					initVal = TypeCoercion::coerceForAssignment(
						std::move(initVal), wtype, method.sourceLocation);
					for (auto& preStmt: m_exprBuilder->takePrePendingStatements())
						targetBody.push_back(std::move(preStmt));
					for (auto& postStmt: m_exprBuilder->takePendingStatements())
						targetBody.push_back(std::move(postStmt));
					auto boxKey = awst::makeUtf8BytesConstant(
						var->name(), method.sourceLocation);
					auto put = awst::makeIntrinsicCall(
						"box_put", awst::WType::voidType(), method.sourceLocation);
					put->stackArgs.push_back(std::move(boxKey));
					put->stackArgs.push_back(std::move(initVal));
					targetBody.push_back(awst::makeExpressionStatement(
						std::move(put), method.sourceLocation));
					continue;
				}

				// Only zero-initialize global state (not box storage)
				if (kind != awst::AppStorageKind::AppGlobal)
					continue;

				// Build key
				auto key = awst::makeUtf8BytesConstant(var->name(), method.sourceLocation);

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
					// Flush any prePending statements (e.g. `new C()` emits an
					// inner-txn create + fund before referencing __new_app_id_N)
					// into the target body so the referenced vars are bound
					// before the state-var assignment.
					for (auto& preStmt: m_exprBuilder->takePrePendingStatements())
						targetBody.push_back(std::move(preStmt));
					for (auto& postStmt: m_exprBuilder->takePendingStatements())
						targetBody.push_back(std::move(postStmt));
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
					auto val = awst::makeIntegerConstant("0", method.sourceLocation, awst::WType::biguintType());
					defaultVal = val;
				}
				else if (wtype == awst::WType::boolType()
					|| wtype == awst::WType::uint64Type())
				{
					auto val = awst::makeIntegerConstant("0", method.sourceLocation);
					defaultVal = val;
				}
				else if (wtype->kind() == awst::WTypeKind::ReferenceArray
					|| wtype->kind() == awst::WTypeKind::ARC4StaticArray
					|| wtype->kind() == awst::WTypeKind::ARC4DynamicArray)
				{
					defaultVal = StorageMapper::makeDefaultValue(wtype, method.sourceLocation);
				}
				else if (wtype->kind() == awst::WTypeKind::ARC4Struct
					|| wtype->kind() == awst::WTypeKind::WTuple)
				{
					// Struct → use StorageMapper's default
					defaultVal = StorageMapper::makeDefaultValue(wtype, method.sourceLocation);
				}
				else
				{
					// Fixed-size bytes (bytes1..bytes32) → N zero bytes so the
					// auto-getter ABI emits the declared width. Dynamic bytes /
					// string keep the empty default.
					int bytesLen = 0;
					if (auto const* bw = dynamic_cast<awst::BytesWType const*>(wtype))
						if (bw->length().has_value() && *bw->length() > 0)
							bytesLen = static_cast<int>(*bw->length());
					defaultVal = awst::makeBytesConstant(
						std::vector<uint8_t>(static_cast<size_t>(bytesLen), 0),
						method.sourceLocation,
						awst::BytesEncoding::Base16,
						wtype && wtype->kind() == awst::WTypeKind::Bytes
							? wtype : awst::WType::bytesType());
				}
				} // end if (!defaultVal)

				// app_global_put(key, defaultVal)
				auto put = awst::makeIntrinsicCall("app_global_put", awst::WType::voidType(), method.sourceLocation);
				put->stackArgs.push_back(key);
				put->stackArgs.push_back(defaultVal);

				auto stmt = awst::makeExpressionStatement(put, method.sourceLocation);
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
					if (!wtype)
						continue;
					// Collect dynamic arrays AND dynamic bytes for box creation,
					// PLUS fixed-size ARC4 static arrays (uint[N]) which are
					// stored in a single box of fixed length and need box_create
					// at deploy time so the contract can write to slots without
					// hitting "no such box" at runtime.
					bool isBoxType = wtype->kind() == awst::WTypeKind::ReferenceArray
						|| wtype->kind() == awst::WTypeKind::ARC4DynamicArray
						|| wtype->kind() == awst::WTypeKind::ARC4StaticArray
						|| wtype == awst::WType::bytesType()
						|| (wtype->kind() == awst::WTypeKind::Bytes
							&& !dynamic_cast<awst::BytesWType const*>(wtype)->length().has_value());
					if (!isBoxType)
						continue;

					// Skip box_create for oversized static arrays (e.g.
					// `uint[2 ether]` which would need 2^63 bytes). The
					// array's `.length` is a compile-time literal so reads
					// keep working; element writes would fail at runtime,
					// but such arrays are almost always declared and never
					// written in tests.
					if (wtype->kind() == awst::WTypeKind::ARC4StaticArray)
					{
						auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(wtype);
						if (sa && sa->arraySize() > 0)
						{
							uint64_t elemSize = 32;
							if (auto const* elemT = sa->elementType())
							{
								if (auto const* uintN = dynamic_cast<awst::ARC4UIntN const*>(elemT))
									elemSize = std::max<uint64_t>(1u, static_cast<uint64_t>(uintN->n() / 8));
								else if (elemT->kind() == awst::WTypeKind::Bytes)
									if (auto const* bw = dynamic_cast<awst::BytesWType const*>(elemT))
										if (bw->length().has_value())
											elemSize = *bw->length();
							}
							uint64_t sz = elemSize * static_cast<uint64_t>(sa->arraySize());
							if (sz > 32768)
							{
								Logger::instance().warning(
									"state array '" + var->name() + "' has declared size "
									+ std::to_string(sa->arraySize())
									+ " which exceeds the 32KB box limit — skipping box_create. "
									"Element writes will fail at runtime but .length reads "
									"still return the declared size.",
									method.sourceLocation);
								continue;
							}
						}
					}

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
					auto cast = awst::makeReinterpretCast(std::move(readArg), awst::WType::accountType(), method.sourceLocation);
					paramVal = std::move(cast);
				}
				else if (paramType == awst::WType::biguintType())
				{
					// bytes → biguint via ReinterpretCast (big-endian, no-op on AVM)
					auto cast = awst::makeReinterpretCast(std::move(readArg), awst::WType::biguintType(), method.sourceLocation);
					paramVal = std::move(cast);
				}
				else if (paramType == awst::WType::uint64Type()
					|| paramType == awst::WType::boolType())
				{
					// Constructor args come as 32-byte big-endian (EVM ABI encoding).
					// Extract last 8 bytes, then btoi to native uint64/bool.
					auto len = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), method.sourceLocation);
					len->stackArgs.push_back(readArg);

					auto eight = awst::makeIntegerConstant("8", method.sourceLocation);

					auto offset = awst::makeUInt64BinOp(std::move(len), awst::UInt64BinaryOperator::Sub, eight, method.sourceLocation);

					auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), method.sourceLocation);
					extract->stackArgs.push_back(std::move(readArg));
					extract->stackArgs.push_back(std::move(offset));
					auto eight2 = awst::makeIntegerConstant("8", method.sourceLocation);
					extract->stackArgs.push_back(std::move(eight2));

					auto btoi = awst::makeIntrinsicCall("btoi", paramType, method.sourceLocation);
					btoi->stackArgs.push_back(std::move(extract));
					paramVal = std::move(btoi);
				}
				else if (paramType == awst::WType::stringType())
				{
					// bytes → string via ReinterpretCast
					auto cast = awst::makeReinterpretCast(std::move(readArg), awst::WType::stringType(), method.sourceLocation);
					paramVal = std::move(cast);
				}
				else if (paramType->kind() == awst::WTypeKind::ReferenceArray)
				{
					// Array params: ReinterpretCast to ARC4 type, then ARC4Decode
					auto const* arc4Type = m_typeMapper.mapToARC4Type(paramType);
					auto cast = awst::makeReinterpretCast(std::move(readArg), arc4Type, method.sourceLocation);

					auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(paramType);
					if (refArr && !refArr->arraySize().has_value())
					{
						auto convert = std::make_shared<awst::ConvertArray>();
						convert->sourceLocation = method.sourceLocation;
						convert->wtype = paramType;
						convert->expr = std::move(cast);
						paramVal = std::move(convert);
					}
					else
					{
						auto decode = std::make_shared<awst::ARC4Decode>();
						decode->sourceLocation = method.sourceLocation;
						decode->wtype = paramType;
						decode->value = std::move(cast);
						paramVal = std::move(decode);
					}
				}
				else if (paramType->kind() == awst::WTypeKind::ARC4StaticArray
					|| paramType->kind() == awst::WTypeKind::ARC4DynamicArray)
				{
					// ARC4 array params: just ReinterpretCast raw bytes to ARC4 type
					auto cast = awst::makeReinterpretCast(std::move(readArg), paramType, method.sourceLocation);
					paramVal = std::move(cast);
				}
				else if (paramType->kind() == awst::WTypeKind::Bytes
					&& dynamic_cast<awst::BytesWType const*>(paramType)
					&& dynamic_cast<awst::BytesWType const*>(paramType)->length().has_value())
				{
					// bytes[N] params: ReinterpretCast from raw bytes
					auto cast = awst::makeReinterpretCast(std::move(readArg), paramType, method.sourceLocation);
					paramVal = std::move(cast);
				}
				else if (dynamic_cast<awst::ARC4Struct const*>(paramType))
				{
					// Struct params: ReinterpretCast raw bytes to ARC4 struct type
					auto cast = awst::makeReinterpretCast(std::move(readArg), paramType, method.sourceLocation);
					paramVal = std::move(cast);
				}
				else
				{
					// bytes, etc. → use raw bytes directly
					paramVal = std::move(readArg);
				}

				auto target = awst::makeVarExpression(param->name(), paramType, method.sourceLocation);

				auto assignment = awst::makeAssignmentStatement(target, std::move(paramVal), method.sourceLocation);
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
			// All init code deferred to __postInit (state var defaults + constructor body).
			// Create call only sets the pending flag.
			// Set __ctor_pending = 1 in create block.
			auto pendingKey = awst::makeUtf8BytesConstant("__ctor_pending", method.sourceLocation);

			auto one = awst::makeIntegerConstant("1", method.sourceLocation);

			auto setPending = awst::makeIntrinsicCall("app_global_put", awst::WType::voidType(), method.sourceLocation);
			setPending->stackArgs.push_back(pendingKey);
			setPending->stackArgs.push_back(one);

			auto setPendingStmt = awst::makeExpressionStatement(setPending, method.sourceLocation);
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

			// Remap aggregate types (arrays, tuples) to ARC4 encoding for __postInit args,
			// plus biguint uintN to ARC4UIntN so the ABI signature and the stored value
			// both use Solidity's declared bit width (matches regular method-param remap).
			// Biguint remap tracks (orig name, arc4 name) so we can emit ARC4Decode
			// statements at the top of __postInit body below.
			struct PostInitDecode { std::string origName; std::string arc4Name; awst::WType const* arc4Type; awst::WType const* origType; };
			std::vector<PostInitDecode> postInitDecodes;
			for (size_t pi = 0; pi < postInit.args.size(); ++pi)
			{
				auto& arg = postInit.args[pi];

				if (arg.wtype == awst::WType::biguintType() && constructor
					&& pi < constructor->parameters().size())
				{
					auto const* solType = constructor->parameters()[pi]->annotation().type;
					auto const* intType = solType ? dynamic_cast<solidity::frontend::IntegerType const*>(solType) : nullptr;
					if (!intType && solType)
						if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
							intType = dynamic_cast<solidity::frontend::IntegerType const*>(&udvt->underlyingType());
					// Only unsigned — signed uses two's-complement in biguint which ARC4UIntN would reject.
					if (intType && !intType->isSigned())
					{
						unsigned bits = intType->numBits();
						auto const* arc4Type = m_typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(bits));
						std::string origName = arg.name;
						std::string arc4Name = "__arc4_" + origName;
						postInitDecodes.push_back({origName, arc4Name, arc4Type, arg.wtype});
						arg.name = arc4Name;
						arg.wtype = arc4Type;
						continue;
					}
				}

				bool isAggregate = arg.wtype
					&& (arg.wtype->kind() == awst::WTypeKind::ReferenceArray
						|| arg.wtype->kind() == awst::WTypeKind::ARC4StaticArray
						|| arg.wtype->kind() == awst::WTypeKind::ARC4DynamicArray
						|| arg.wtype->kind() == awst::WTypeKind::WTuple);
				if (isAggregate)
				{
					awst::WType const* arc4Type = m_typeMapper.mapToARC4Type(arg.wtype);
					if (arc4Type != arg.wtype)
						arg.wtype = arc4Type;
				}
			}

			// Set function context so constructor body can reference params by name.
			// For biguint args remapped to ARC4UIntN, use the ORIGINAL name + biguint
			// type so the body looks them up via the decoded local (emitted below).
			{
				std::vector<std::pair<std::string, awst::WType const*>> paramContext;
				std::set<std::string> arc4Names;
				for (auto const& d: postInitDecodes)
					arc4Names.insert(d.arc4Name);
				for (auto const& arg: postInit.args)
				{
					if (arc4Names.count(arg.name)) continue; // skip the __arc4_ shim
					paramContext.emplace_back(arg.name, arg.wtype);
				}
				for (auto const& d: postInitDecodes)
					paramContext.emplace_back(d.origName, d.origType);
				setFunctionContext(paramContext, postInit.returnType);
			}

			auto postInitBody = std::make_shared<awst::Block>();
			postInitBody->sourceLocation = method.sourceLocation;

			// Guard: assert(__ctor_pending == 1)
			auto readPending = awst::makeIntrinsicCall("app_global_get", awst::WType::uint64Type(), method.sourceLocation);
			readPending->stackArgs.push_back(
				awst::makeUtf8BytesConstant("__ctor_pending", method.sourceLocation));

			auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(
				readPending, method.sourceLocation, "__postInit already called"), method.sourceLocation);
			postInitBody->body.push_back(std::move(assertStmt));

			// Clear flag: __ctor_pending = 0
			auto clearKey = awst::makeUtf8BytesConstant("__ctor_pending", method.sourceLocation);

			auto zeroVal = awst::makeIntegerConstant("0", method.sourceLocation);

			auto clearPending = awst::makeIntrinsicCall("app_global_put", awst::WType::voidType(), method.sourceLocation);
			clearPending->stackArgs.push_back(clearKey);
			clearPending->stackArgs.push_back(zeroVal);

			auto clearStmt = awst::makeExpressionStatement(clearPending, method.sourceLocation);
			postInitBody->body.push_back(std::move(clearStmt));

			// Emit ARC4Decode statements for biguint uintN args remapped to ARC4UIntN.
			// `<origName> = ARC4Decode(<__arc4_origName>)` — constructor body then
			// references the original name as biguint, matching pre-remap semantics.
			for (auto const& decode: postInitDecodes)
			{
				auto arc4Var = awst::makeVarExpression(decode.arc4Name, decode.arc4Type, method.sourceLocation);

				auto decodeExpr = std::make_shared<awst::ARC4Decode>();
				decodeExpr->sourceLocation = method.sourceLocation;
				decodeExpr->wtype = decode.origType;
				decodeExpr->value = std::move(arc4Var);

				auto target = awst::makeVarExpression(decode.origName, decode.origType, method.sourceLocation);

				auto assign = awst::makeAssignmentStatement(std::move(target), std::move(decodeExpr), method.sourceLocation);
				postInitBody->body.push_back(std::move(assign));
			}

			// Create boxes for dynamic array state variables
			for (auto const& varName: m_boxArrayVarNames)
			{
				auto boxKey = awst::makeUtf8BytesConstant(varName, method.sourceLocation);

				// Uninitialised dynamic `bytes` state vars: skip the
				// box_create so the reader's `box_get → select` fallback
				// returns zero-length bytes. The raw box content is the
				// bytes value stripped of the ARC4 length header, so
				// pre-creating with 2 zero bytes would mean the reader
				// sees 2 bytes of raw data and wraps them with a fresh
				// length header — producing `0x0002 0x0000` instead of
				// an empty `0x0000`. Deferring box creation to the first
				// write fixes the empty case without breaking writes.
				bool isDynamicBytesWithoutInit = false;
				{
					auto const& lin = _contract.annotation().linearizedBaseContracts;
					for (auto const* base: lin)
					{
						for (auto const* var: base->stateVariables())
						{
							if (var->name() != varName || var->isConstant())
								continue;
							auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(var->type());
							if (arrType && arrType->isByteArrayOrString() && !var->value())
								isDynamicBytesWithoutInit = true;
						}
					}
				}
				if (isDynamicBytesWithoutInit)
					continue;

				// Compute box size: 2 bytes for ARC4 length header (empty dynamic array),
				// or string literal size for bytes/string initializers, or
				// elementSize*N for fixed-size ARC4 static arrays (e.g. uint[20]).
				unsigned boxSizeVal = 2; // ARC4 dynamic array length header
				std::shared_ptr<awst::Expression> boxInitVal;
				{
					auto const& lin = _contract.annotation().linearizedBaseContracts;
					for (auto const* base: lin)
						for (auto const* var: base->stateVariables())
						{
							if (var->name() != varName || var->isConstant())
								continue;
							// ARC4StaticArray (uint[N], int[N], etc.): allocate
							// elementSize * arraySize bytes so the contract can
							// write to slot indices without "no such box".
							auto* varWtype = m_typeMapper.map(var->type());
							if (varWtype && varWtype->kind() == awst::WTypeKind::ARC4StaticArray)
							{
								auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(varWtype);
								if (sa && sa->arraySize() > 0)
								{
									uint64_t elemSize = 32; // default for uint256
									auto const* elemT = sa->elementType();
									if (elemT)
									{
										if (auto const* uintN = dynamic_cast<awst::ARC4UIntN const*>(elemT))
											elemSize = std::max<uint64_t>(1u, static_cast<uint64_t>(uintN->n() / 8));
										else if (elemT->kind() == awst::WTypeKind::Bytes)
										{
											auto const* bw = dynamic_cast<awst::BytesWType const*>(elemT);
											if (bw && bw->length().has_value())
												elemSize = *bw->length();
										}
									}
									// AVM box max is 32768 bytes. Cap huge declared
									// sizes (e.g. `uint[2 ether]`) at the max so the
									// contract still deploys; element access beyond
									// the cap will fail at runtime, but `.length`
									// (a compile-time constant) keeps working.
									uint64_t size = elemSize * static_cast<uint64_t>(sa->arraySize());
									if (size > 32768)
										size = 32768;
									boxSizeVal = static_cast<unsigned>(size);
								}
							}
							if (!var->value())
								continue;
							auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(var->type());
							if (arrType && arrType->isByteArrayOrString())
							{
								if (auto const* lit = dynamic_cast<solidity::frontend::Literal const*>(var->value().get()))
									boxSizeVal = static_cast<unsigned>(lit->value().size());
								if (boxSizeVal > 0)
								{
									boxInitVal = m_exprBuilder->build(*var->value());
									if (boxInitVal && boxInitVal->wtype == awst::WType::stringType())
									{
										auto cast = awst::makeReinterpretCast(std::move(boxInitVal), awst::WType::bytesType(), method.sourceLocation);
										boxInitVal = std::move(cast);
									}
								}
							}
						}
				}
				auto boxSize = awst::makeIntegerConstant(std::to_string(boxSizeVal), method.sourceLocation);

				auto boxCreate = awst::makeIntrinsicCall("box_create", awst::WType::boolType(), method.sourceLocation);
				boxCreate->stackArgs.push_back(std::move(boxKey));
				boxCreate->stackArgs.push_back(std::move(boxSize));

				auto boxStmt = awst::makeExpressionStatement(std::move(boxCreate), method.sourceLocation);
				postInitBody->body.push_back(std::move(boxStmt));

				// Write initial value for bytes vars with initializers
				if (boxInitVal)
				{
					auto putKey = awst::makeUtf8BytesConstant(varName, method.sourceLocation);
					auto put = awst::makeIntrinsicCall("box_put", awst::WType::voidType(), method.sourceLocation);
					put->stackArgs.push_back(std::move(putKey));
					put->stackArgs.push_back(std::move(boxInitVal));
					auto putStmt = awst::makeExpressionStatement(std::move(put), method.sourceLocation);
					postInitBody->body.push_back(std::move(putStmt));
				}
			}

			// Initialize all state variable defaults in __postInit
			// (after boxes are created, before constructor bodies run)
			{
				auto const& lin = _contract.annotation().linearizedBaseContracts;
				for (auto it2 = lin.rbegin(); it2 != lin.rend(); ++it2)
					emitStateVarInit(**it2, postInitBody->body);
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

						auto target = awst::makeVarExpression(params[i]->name(), m_typeMapper.map(params[i]->type()), makeLoc(args[i]->location()));

						argExpr = ExpressionBuilder::implicitNumericCast(
							std::move(argExpr), target->wtype, target->sourceLocation
						);

						auto assignment = awst::makeAssignmentStatement(target, std::move(argExpr), target->sourceLocation);
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
		// Constructor body is inlined into the bool-returning approval program.
		// Assembly `return(offset, size)` inside the ctor needs to emit a bool
		// return (handled by AssemblyBuilder::handleReturn when m_returnType is
		// bool). Set stmtCtx.returnType accordingly; restore at the end of the
		// else branch.
		auto const* savedReturnType = m_stmtCtx.returnType;
		m_stmtCtx.returnType = awst::WType::boolType();

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

					auto target = awst::makeVarExpression(params[i]->name(), targetType, makeLoc(args[i]->location()));

					auto assignment = awst::makeAssignmentStatement(target, std::move(argExpr), target->sourceLocation);
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

					auto target = awst::makeVarExpression(params[i]->name(), targetType, makeLoc(args[i]->location()));

					auto assignment = awst::makeAssignmentStatement(target, std::move(argExpr), target->sourceLocation);
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

					auto target = awst::makeVarExpression(params[i]->name(), m_typeMapper.map(params[i]->type()), method.sourceLocation);

					auto assignment = awst::makeAssignmentStatement(target, evaledArgs[i], method.sourceLocation);
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
		m_stmtCtx.returnType = savedReturnType;
		} // end else (no postInit needed)

		// Return true to complete the create transaction
		auto createReturn = awst::makeReturnStatement(awst::makeBoolConstant(true, method.sourceLocation), method.sourceLocation);
		createBlock->body.push_back(createReturn);

		// Initialize the transient-storage blob in scratch slot TRANSIENT_SLOT
		// before the create/dispatch split, so the constructor body can also
		// use tload/tstore (the create branch returns before reaching the
		// post-dispatch preamble below). Scratch slots are per-txn on AVM, so
		// a fresh bzero per app call matches EIP-1153 per-tx transient
		// semantics; writes persist across callsub within an app call because
		// scratch slots do. Size covers all declared transient vars (packed)
		// plus at least one slot to back asm tload/tstore when no named vars
		// are declared.
		{
			unsigned blobBytes = m_transientStorage.blobSize();
			if (blobBytes < AssemblyBuilder::SLOT_SIZE)
				blobBytes = AssemblyBuilder::SLOT_SIZE;
			auto blobSize = awst::makeIntegerConstant(std::to_string(blobBytes), method.sourceLocation);

			auto bzeroCall = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), method.sourceLocation);
			bzeroCall->stackArgs.push_back(std::move(blobSize));

			auto storeOp = awst::makeIntrinsicCall("store", awst::WType::voidType(), method.sourceLocation);
			storeOp->immediates = {AssemblyBuilder::TRANSIENT_SLOT};
			storeOp->stackArgs.push_back(std::move(bzeroCall));

			auto exprStmt = awst::makeExpressionStatement(std::move(storeOp), method.sourceLocation);
			body->body.push_back(std::move(exprStmt));
		}

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
		auto blobSize = awst::makeIntegerConstant(std::to_string(AssemblyBuilder::SLOT_SIZE), method.sourceLocation);

		auto bzeroCall = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), method.sourceLocation);
		bzeroCall->stackArgs.push_back(std::move(blobSize));

		auto storeOp = awst::makeIntrinsicCall("store", awst::WType::voidType(), method.sourceLocation);
		storeOp->immediates = {AssemblyBuilder::MEMORY_SLOT_FIRST};
		storeOp->stackArgs.push_back(std::move(bzeroCall));

		auto exprStmt = awst::makeExpressionStatement(std::move(storeOp), method.sourceLocation);
		body->body.push_back(std::move(exprStmt));

		// Write the free memory pointer (FMP) at offset 0x40 = 0x80.
		// This must be done once in the preamble, not in each assembly block,
		// so that subsequent assembly blocks see any FMP updates from earlier blocks.
		// Pattern: store 0, replace3(load(0), 64, pad32_0x80)
		auto loadBlob = awst::makeIntrinsicCall("load", awst::WType::bytesType(), method.sourceLocation);
		loadBlob->immediates = {AssemblyBuilder::MEMORY_SLOT_FIRST};

		auto fmpOffset = std::make_shared<awst::IntegerConstant>();
		fmpOffset->sourceLocation = method.sourceLocation;
		fmpOffset->wtype = awst::WType::uint64Type();
		fmpOffset->value = "64"; // 0x40

		// 32-byte big-endian 0x80 = 0x00...0080
		std::vector<uint8_t> fmpBytesVal(31, 0);
		fmpBytesVal.push_back(0x80);
		auto fmpBytes = awst::makeBytesConstant(
			std::move(fmpBytesVal), method.sourceLocation, awst::BytesEncoding::Unknown);

		auto replaceOp = awst::makeIntrinsicCall("replace3", awst::WType::bytesType(), method.sourceLocation);
		replaceOp->stackArgs.push_back(std::move(loadBlob));
		replaceOp->stackArgs.push_back(std::move(fmpOffset));
		replaceOp->stackArgs.push_back(std::move(fmpBytes));

		auto storeFmpOp = awst::makeIntrinsicCall("store", awst::WType::voidType(), method.sourceLocation);
		storeFmpOp->immediates = {AssemblyBuilder::MEMORY_SLOT_FIRST};
		storeFmpOp->stackArgs.push_back(std::move(replaceOp));

		auto fmpStmt = awst::makeExpressionStatement(std::move(storeFmpOp), method.sourceLocation);
		body->body.push_back(std::move(fmpStmt));
	}

	// Transient state vars live in scratch slot TRANSIENT_SLOT (packed blob,
	// shared with asm tload/tstore). Scratch is per-txn on AVM, so the
	// scratch bzero in the preamble above already satisfies EIP-1153 per-tx
	// reset — no per-call app_global reset needed.

	// Detect fallback and receive functions across the MRO.
	// Solidity allows only one of each in the linearized hierarchy.
	solidity::frontend::FunctionDefinition const* fallbackFunc = nullptr;
	solidity::frontend::FunctionDefinition const* receiveFunc = nullptr;
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		for (auto const* func: base->definedFunctions())
		{
			if (!func->isImplemented())
				continue;
			if (func->isFallback() && !fallbackFunc)
				fallbackFunc = func;
			else if (func->isReceive() && !receiveFunc)
				receiveFunc = func;
		}
		if (fallbackFunc && receiveFunc)
			break;
	}

	if (!fallbackFunc && !receiveFunc)
	{
		// No fallback/receive: use the normal pattern `return ARC4Router()`
		// which triggers puya's can_exit_early=True (rejects on no selector match).
		auto routerExpr = std::make_shared<awst::ARC4Router>();
		routerExpr->sourceLocation = method.sourceLocation;
		routerExpr->wtype = awst::WType::boolType();

		auto routerReturn = awst::makeReturnStatement(routerExpr, method.sourceLocation);
		body->body.push_back(routerReturn);
	}
	else
	{
		// Custom dispatch for fallback/receive.
		// Pattern:
		//   if (NumAppArgs == 0) {
		//     if (receive) call receive; else call fallback;
		//     return true;
		//   }
		//   __did_match = ARC4Router();
		//   if (!__did_match) {
		//     call fallback;  // or reject if no fallback
		//     __did_match = true;
		//   }
		//   return __did_match;
		//
		// Using ARC4Router as an assignment value forces puya's
		// can_exit_early=False mode, so the router returns false on no-match
		// instead of calling err.

		// isBareCall=true → pass empty bytes as the fallback argument
		// isBareCall=false → pass ApplicationArgs[0] (the unmatched data)
		auto makeCall = [&](std::string const& _name,
			solidity::frontend::FunctionDefinition const* _func,
			bool _isBareCall)
			-> std::shared_ptr<awst::Statement>
		{
			auto call = std::make_shared<awst::SubroutineCallExpression>();
			call->sourceLocation = method.sourceLocation;
			call->wtype = awst::WType::voidType();
			call->target = awst::InstanceMethodTarget{_name};
			// If the function takes a bytes parameter, pass the calldata.
			// Fallback may take `bytes calldata _input`.
			if (_func && _func->parameters().size() == 1)
			{
				std::shared_ptr<awst::Expression> argExpr;
				if (_isBareCall)
				{
					// No calldata in bare calls — pass empty bytes
					argExpr = awst::makeBytesConstant({}, method.sourceLocation);
				}
				else
				{
					auto argBytes = awst::makeIntrinsicCall("txna", awst::WType::bytesType(), method.sourceLocation);
					argBytes->immediates = {std::string("ApplicationArgs"), 0};
					argExpr = std::move(argBytes);
				}

				awst::CallArg ca;
				ca.name = std::nullopt;
				ca.value = std::move(argExpr);
				call->args.push_back(std::move(ca));
			}

			auto stmt = awst::makeExpressionStatement(call, method.sourceLocation);
			return stmt;
		};

		auto makeTrueLit = [&]() {
			return awst::makeBoolConstant(true, method.sourceLocation);
		};

		auto makeReturnTrue = [&]() -> std::shared_ptr<awst::Statement> {
			auto r = awst::makeReturnStatement(makeTrueLit(), method.sourceLocation);
			return r;
		};

		// Step 1: Bare call check (NumAppArgs == 0).
		// Call receive/fallback and return true — no selector to match.
		{
			auto numAppArgs = awst::makeIntrinsicCall("txn", awst::WType::uint64Type(), method.sourceLocation);
			numAppArgs->immediates = {std::string("NumAppArgs")};

			auto zero = awst::makeIntegerConstant("0", method.sourceLocation);

			auto isBareCall = awst::makeNumericCompare(std::move(numAppArgs), awst::NumericComparison::Eq, std::move(zero), method.sourceLocation);

			auto bareBlock = std::make_shared<awst::Block>();
			bareBlock->sourceLocation = method.sourceLocation;
			if (receiveFunc)
				bareBlock->body.push_back(makeCall("__receive", receiveFunc, true));
			else if (fallbackFunc)
				bareBlock->body.push_back(makeCall("__fallback", fallbackFunc, true));
			bareBlock->body.push_back(makeReturnTrue());

			auto ifBare = std::make_shared<awst::IfElse>();
			ifBare->sourceLocation = method.sourceLocation;
			ifBare->condition = std::move(isBareCall);
			ifBare->ifBranch = std::move(bareBlock);
			body->body.push_back(std::move(ifBare));
		}

		// Step 2: Non-bare call — run the ARC4 router.
		// Assign result to var (triggers can_exit_early=False in puya).
		std::string matchVarName = "__did_match_routing";
		{
			auto matchVar = awst::makeVarExpression(matchVarName, awst::WType::boolType(), method.sourceLocation);

			auto routerExpr = std::make_shared<awst::ARC4Router>();
			routerExpr->sourceLocation = method.sourceLocation;
			routerExpr->wtype = awst::WType::boolType();

			auto assignMatch = awst::makeAssignmentStatement(std::move(matchVar), std::move(routerExpr), method.sourceLocation);
			body->body.push_back(std::move(assignMatch));
		}

		// Step 3: If no match AND fallback exists, call fallback.
		if (fallbackFunc)
		{
			auto matchVarRead = awst::makeVarExpression(matchVarName, awst::WType::boolType(), method.sourceLocation);

			auto notMatch = std::make_shared<awst::Not>();
			notMatch->sourceLocation = method.sourceLocation;
			notMatch->wtype = awst::WType::boolType();
			notMatch->expr = std::move(matchVarRead);

			auto dispatchBlock = std::make_shared<awst::Block>();
			dispatchBlock->sourceLocation = method.sourceLocation;
			dispatchBlock->body.push_back(makeCall("__fallback", fallbackFunc, false));

			// Set __did_match = true so the approval returns true.
			auto matchVarWrite = awst::makeVarExpression(matchVarName, awst::WType::boolType(), method.sourceLocation);

			auto assignTrue = awst::makeAssignmentStatement(std::move(matchVarWrite), makeTrueLit(), method.sourceLocation);
			dispatchBlock->body.push_back(std::move(assignTrue));

			auto ifNoMatch = std::make_shared<awst::IfElse>();
			ifNoMatch->sourceLocation = method.sourceLocation;
			ifNoMatch->condition = std::move(notMatch);
			ifNoMatch->ifBranch = std::move(dispatchBlock);
			body->body.push_back(std::move(ifNoMatch));
		}

		// Step 4: return __did_match_routing
		auto finalRead = awst::makeVarExpression(matchVarName, awst::WType::boolType(), method.sourceLocation);

		auto retStmt = awst::makeReturnStatement(std::move(finalRead), method.sourceLocation);
		body->body.push_back(std::move(retStmt));
	}

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
	auto ret = awst::makeReturnStatement(awst::makeBoolConstant(true, method.sourceLocation), method.sourceLocation);

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
	// Scope guard for per-method state (varNameToId, funcPtrTargets, etc.)
	auto methodScope = m_exprBuilder->pushScope();

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
		// Function pointer parameters: override type to uint64 (internal) or bytes[12] (external)
		if (auto const* funcType = dynamic_cast<solidity::frontend::FunctionType const*>(param->type()))
		{
			if (funcType->kind() == solidity::frontend::FunctionType::Kind::Internal)
				arg.wtype = awst::WType::uint64Type();
			else if (funcType->kind() == solidity::frontend::FunctionType::Kind::External
				|| funcType->kind() == solidity::frontend::FunctionType::Kind::DelegateCall)
				arg.wtype = m_typeMapper.createType<awst::BytesWType>(12);
		}
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
	// Track unsigned sub-word return params for cleanup masking
	struct UnsignedMaskInfo {
		unsigned bits;
		size_t index;
	};
	std::vector<UnsignedMaskInfo> unsignedMasks;

	if (returnParams.empty())
		method.returnType = awst::WType::voidType();
	else if (returnParams.size() == 1)
	{
		method.returnType = m_typeMapper.map(returnParams[0]->type());
		// Storage reference returns (from functions with .slot assembly):
		// return type is biguint (slot number), not the array type.
		if (returnParams[0]->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
			&& _func.isImplemented()
			&& std::any_of(_func.body().statements().begin(), _func.body().statements().end(),
				[](auto const& s) { return dynamic_cast<solidity::frontend::InlineAssembly const*>(s.get()); }))
			method.returnType = awst::WType::biguintType();
		// For signed integer returns ≤64 bits, promote to biguint for proper
		// 256-bit two's complement ARC4 encoding.
		// Unwrap UserDefinedValueType/EnumType to find the underlying IntegerType.
		auto const* retSolType = returnParams[0]->type();
		if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(retSolType))
			retSolType = &udvt->underlyingType();
		auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(retSolType);
		// Enums are uint8 in ABI — treat as unsigned 8-bit
		if (!intType)
			if (auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(retSolType))
				intType = dynamic_cast<solidity::frontend::IntegerType const*>(
					enumType->encodingType());
		if (intType && intType->isSigned())
		{
			if (intType->numBits() <= 64)
				method.returnType = awst::WType::biguintType();
			signedReturns.push_back({intType->numBits(), 0});
		}
		else if (intType && !intType->isSigned() && intType->numBits() < 64)
		{
			unsignedMasks.push_back({intType->numBits(), 0});
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
			// Detect signed/narrow integer elements for sign-extension/masking
			auto const* retSolType = rp->type();
			if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(retSolType))
				retSolType = &udvt->underlyingType();
			auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(retSolType);
			if (!intType)
				if (auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(retSolType))
					intType = dynamic_cast<solidity::frontend::IntegerType const*>(
						enumType->encodingType());
			if (intType)
			{
				if (intType->isSigned())
				{
					if (intType->numBits() <= 64)
						mappedType = awst::WType::biguintType();
					signedReturns.push_back({intType->numBits(), ri});
				}
				else if (!intType->isSigned() && intType->numBits() < 64)
				{
					unsignedMasks.push_back({intType->numBits(), ri});
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
	// Detect inline assembly early — needed to skip ARC4 param wrapping
	// which would break assembly variable references.
	bool funcHasInlineAssembly = false;
	if (_func.isImplemented())
	{
		for (auto const& stmt: _func.body().statements())
			if (dynamic_cast<solidity::frontend::InlineAssembly const*>(stmt.get()))
			{ funcHasInlineAssembly = true; break; }
	}

	if (method.arc4MethodConfig.has_value())
	{
		// Remap types to ARC4 encoding for ABI-exposed methods.
		// This ensures correct ABI signatures (e.g., uint256 not uint512 for biguint).
		auto const& solParams = _func.parameters();
		for (size_t pi = 0; pi < method.args.size(); ++pi)
		{
			auto& arg = method.args[pi];

			// Remap biguint args to ARC4UIntN with the original Solidity bit width.
			// Without this, puya maps biguint→uint512 (AVM max) instead of uint256.
			// Signed integers use the same 256-bit two's complement encoding —
			// we keep ARC4UIntN(256) and let the test runner's _abi_safe_type
			// helper map int<N>→uint<N> so encode/decode line up.
			// Skip when function has modifiers or inline assembly — both reference
			// params by their original names and would break on rename.
			if (arg.wtype == awst::WType::biguintType() && pi < solParams.size()
				&& _func.modifiers().empty() && !funcHasInlineAssembly)
			{
				auto const* solType = solParams[pi]->annotation().type;
				auto const* intType = solType ? dynamic_cast<solidity::frontend::IntegerType const*>(solType) : nullptr;
				if (!intType && solType)
					if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
						intType = dynamic_cast<solidity::frontend::IntegerType const*>(&udvt->underlyingType());
				unsigned bits = intType ? intType->numBits() : 256;
				auto const* arc4Type = m_typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(bits));
				paramDecodes.push_back({arg.name, arg.wtype, arc4Type, arg.sourceLocation});
				arg.wtype = arc4Type;
				continue;
			}

			// Remap aggregate types (arrays, tuples) and external fn-ptr
			// bytes[12] to ARC4 encoding. General bytes/bytes[N] params
			// are NOT remapped — only fn-ptr-specific bytes[12].
			bool isAggregate = arg.wtype
				&& (arg.wtype->kind() == awst::WTypeKind::ReferenceArray
					|| arg.wtype->kind() == awst::WTypeKind::ARC4StaticArray
					|| arg.wtype->kind() == awst::WTypeKind::ARC4DynamicArray
					|| arg.wtype->kind() == awst::WTypeKind::WTuple);
			// External fn-ptr: bytes[12] needs ARC4 remapping to byte[12]
			if (!isAggregate && pi < solParams.size())
			{
				if (dynamic_cast<solidity::frontend::FunctionType const*>(solParams[pi]->type())
					&& arg.wtype && arg.wtype->kind() == awst::WTypeKind::Bytes)
					isAggregate = true;
			}
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

		// Register named return variable names so inner scoping detects shadowing
		for (auto const& rp: returnParams)
			if (!rp->name().empty())
				m_exprBuilder->resolveVarName(rp->name(), rp->id());

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

				auto target = awst::makeVarExpression(rp->name(), rpType, method.sourceLocation);

				auto zeroVal = StorageMapper::makeDefaultValue(rpType, method.sourceLocation);

				auto assign = awst::makeAssignmentStatement(std::move(target), std::move(zeroVal), method.sourceLocation);
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
			&& !blockAlwaysTerminates(*method.body))
		{
			auto const& retParams = _func.returnParameters();
			bool hasNamedReturns = false;
			for (auto const& rp: retParams)
				if (!rp->name().empty())
					hasNamedReturns = true;

			auto retStmt = awst::makeReturnStatement(nullptr, method.sourceLocation);

			if (hasNamedReturns)
			{
				if (retParams.size() == 1)
				{
					auto var = awst::makeVarExpression(retParams[0]->name(), m_typeMapper.map(retParams[0]->type()), method.sourceLocation);
					retStmt->value = std::move(var);
				}
				else
				{
					auto tuple = std::make_shared<awst::TupleExpression>();
					tuple->sourceLocation = method.sourceLocation;
					for (auto const& rp: retParams)
					{
						auto var = awst::makeVarExpression(rp->name(), m_typeMapper.map(rp->type()), method.sourceLocation);
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

			// Enum range validation for implicit return of named return variables
			if (hasNamedReturns && retParams.size() == 1)
			{
				if (auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(retParams[0]->type()))
				{
					unsigned numMembers = enumType->numberOfMembers();
					auto var = awst::makeVarExpression(retParams[0]->name(), awst::WType::uint64Type(), method.sourceLocation);

					auto maxVal = awst::makeIntegerConstant(std::to_string(numMembers), method.sourceLocation);

					auto cmp = awst::makeNumericCompare(std::move(var), awst::NumericComparison::Lt, std::move(maxVal), method.sourceLocation);

					auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), method.sourceLocation, "enum out of range"), method.sourceLocation);
					method.body->body.push_back(std::move(assertStmt));
				}
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

		// For ARC4 methods returning biguint, wrap return values in ARC4Encode
		// with the correct bit width (e.g., uint256 not uint512).
		// Skip signed returns, functions with modifiers, and functions with inline assembly.
		if (method.arc4MethodConfig.has_value() && method.returnType == awst::WType::biguintType()
			&& signedReturns.empty() && _func.modifiers().empty() && !funcHasInlineAssembly)
		{
			// Get original Solidity bit width for the return type
			unsigned retBits = 256;
			if (returnParams.size() == 1)
			{
				auto const* retSolType = returnParams[0]->type();
				if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(retSolType))
					retSolType = &udvt->underlyingType();
				if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(retSolType))
					retBits = intType->numBits();
				else if (auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(retSolType))
					if (auto const* encType = dynamic_cast<solidity::frontend::IntegerType const*>(
						enumType->encodingType()))
						retBits = encType->numBits();
			}
			auto const* arc4RetType = m_typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(retBits));

			std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> wrapBiguintReturns;
			wrapBiguintReturns = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
			{
				for (auto& stmt: stmts)
				{
					if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
					{
						if (ret->value && ret->value->wtype == awst::WType::biguintType())
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
						if (ifElse->ifBranch) wrapBiguintReturns(ifElse->ifBranch->body);
						if (ifElse->elseBranch) wrapBiguintReturns(ifElse->elseBranch->body);
					}
					else if (auto* block = dynamic_cast<awst::Block*>(stmt.get()))
						wrapBiguintReturns(block->body);
					else if (auto* loop = dynamic_cast<awst::WhileLoop*>(stmt.get()))
						if (loop->loopBody) wrapBiguintReturns(loop->loopBody->body);
				}
			};
			wrapBiguintReturns(method.body->body);
			method.returnType = arc4RetType;
		}

		// For ARC4 methods returning tuples with biguint elements,
		// wrap each biguint element in ARC4Encode with correct bit width.
		if (method.arc4MethodConfig.has_value() && method.returnType
			&& method.returnType->kind() == awst::WTypeKind::WTuple
			&& signedReturns.empty() && _func.modifiers().empty() && !funcHasInlineAssembly)
		{
			auto const* tupleType = static_cast<awst::WTuple const*>(method.returnType);
			// Only wrap when ALL elements are biguint or uint64/bool (simple scalars).
			// Mixed tuples with arrays/structs/strings need different handling.
			bool allScalar = true;
			bool hasBiguintElement = false;
			for (auto const* t : tupleType->types())
			{
				if (t == awst::WType::biguintType())
					hasBiguintElement = true;
				else if (t != awst::WType::uint64Type() && t != awst::WType::boolType())
					allScalar = false;
			}

			if (hasBiguintElement && allScalar)
			{
				// Build ARC4 type for each element
				std::vector<awst::WType const*> arc4Types;
				for (size_t ri = 0; ri < returnParams.size() && ri < tupleType->types().size(); ++ri)
				{
					auto const* elemType = tupleType->types()[ri];
					if (elemType == awst::WType::biguintType())
					{
						auto const* retSolType = returnParams[ri]->type();
						if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(retSolType))
							retSolType = &udvt->underlyingType();
						unsigned bits = 256;
						if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(retSolType))
							bits = intType->numBits();
						arc4Types.push_back(m_typeMapper.createType<awst::ARC4UIntN>(static_cast<int>(bits)));
					}
					else
						arc4Types.push_back(elemType);
				}

				// Helper: wrap biguint items inside a single TupleExpression with
				// ARC4Encode, and update the tuple's wtype to the ARC4 tuple type.
				auto wrapTupleItems = [&](awst::TupleExpression* tuple)
				{
					if (!tuple) return;
					for (size_t i = 0; i < tuple->items.size() && i < arc4Types.size(); ++i)
					{
						if (tuple->items[i]->wtype == awst::WType::biguintType()
							&& arc4Types[i]->kind() == awst::WTypeKind::ARC4UIntN)
						{
							auto encode = std::make_shared<awst::ARC4Encode>();
							encode->sourceLocation = tuple->items[i]->sourceLocation;
							encode->wtype = arc4Types[i];
							encode->value = std::move(tuple->items[i]);
							tuple->items[i] = std::move(encode);
						}
					}
					tuple->wtype = new awst::WTuple(
						std::vector<awst::WType const*>(arc4Types));
				};

				// Walk the body and wrap biguint tuple elements in ARC4Encode.
				// Handles direct tuple returns and conditional expressions whose
				// branches are tuple literals.
				static int retTmpCounter = 0;
				std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> wrapTupleReturns;
				wrapTupleReturns = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
				{
					for (size_t si = 0; si < stmts.size(); ++si)
					{
						auto& stmt = stmts[si];
						if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
						{
							if (!ret->value) continue;
							if (auto* tuple = dynamic_cast<awst::TupleExpression*>(ret->value.get()))
								wrapTupleItems(tuple);
							else if (auto* cond = dynamic_cast<awst::ConditionalExpression*>(ret->value.get()))
							{
								wrapTupleItems(dynamic_cast<awst::TupleExpression*>(cond->trueExpr.get()));
								wrapTupleItems(dynamic_cast<awst::TupleExpression*>(cond->falseExpr.get()));
								cond->wtype = new awst::WTuple(
									std::vector<awst::WType const*>(arc4Types));
							}
							else if (ret->value->wtype
								&& ret->value->wtype->kind() == awst::WTypeKind::WTuple)
							{
								// Non-literal tuple expression (e.g. `return fu()`):
								// spill into a local, then build a TupleExpression of
								// ARC4-encoded TupleItemExpressions so each biguint
								// element is properly widened to its ARC4UIntN width.
								auto const* subTupleType = static_cast<awst::WTuple const*>(ret->value->wtype);
								bool needsWrap = false;
								for (auto const* t : subTupleType->types())
									if (t == awst::WType::biguintType()) { needsWrap = true; break; }
								if (!needsWrap) continue;

								std::string tmpName = "__ret_tmp_" + std::to_string(retTmpCounter++);
								auto tmpVar = awst::makeVarExpression(tmpName, ret->value->wtype, ret->sourceLocation);

								auto assign = awst::makeAssignmentStatement(tmpVar, std::move(ret->value), ret->sourceLocation);

								auto newTuple = std::make_shared<awst::TupleExpression>();
								newTuple->sourceLocation = assign->sourceLocation;
								for (size_t i = 0; i < arc4Types.size() && i < subTupleType->types().size(); ++i)
								{
									auto item = std::make_shared<awst::TupleItemExpression>();
									item->sourceLocation = assign->sourceLocation;
									item->base = tmpVar;
									item->index = static_cast<int>(i);
									item->wtype = subTupleType->types()[i];
									if (subTupleType->types()[i] == awst::WType::biguintType()
										&& arc4Types[i]->kind() == awst::WTypeKind::ARC4UIntN)
									{
										auto encode = std::make_shared<awst::ARC4Encode>();
										encode->sourceLocation = assign->sourceLocation;
										encode->wtype = arc4Types[i];
										encode->value = std::move(item);
										newTuple->items.push_back(std::move(encode));
									}
									else
										newTuple->items.push_back(std::move(item));
								}
								newTuple->wtype = new awst::WTuple(
									std::vector<awst::WType const*>(arc4Types));
								ret->value = std::move(newTuple);

								stmts.insert(stmts.begin() + si, std::move(assign));
								++si; // skip the newly-inserted assign
							}
						}
						else if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get()))
						{
							if (ifElse->ifBranch) wrapTupleReturns(ifElse->ifBranch->body);
							if (ifElse->elseBranch) wrapTupleReturns(ifElse->elseBranch->body);
						}
						else if (auto* block = dynamic_cast<awst::Block*>(stmt.get()))
							wrapTupleReturns(block->body);
						else if (auto* loop = dynamic_cast<awst::WhileLoop*>(stmt.get()))
							if (loop->loopBody) wrapTupleReturns(loop->loopBody->body);
					}
				};
				wrapTupleReturns(method.body->body);
				method.returnType = new awst::WTuple(std::vector<awst::WType const*>(arc4Types));
			}
		}

		// Sign-extend return values for signed integer types ≤64 bits, and
		// for ≤256-bit signed returns wrap the result in an ARC4Encode of
		// ARC4UIntN(256) so the ABI output is uint256 (32 bytes) rather
		// than puya's default biguint→uint512 (64 bytes).
		if (!signedReturns.empty() && method.arc4MethodConfig.has_value())
		{
			// All signed returns are wrapped to 256 bits by signExtendToUint256,
			// so the ABI element is uint256 in every case.
			auto const* arc4SignedType =
				m_typeMapper.createType<awst::ARC4UIntN>(256);

			auto wrapArc4 = [&](std::shared_ptr<awst::Expression> val,
				awst::SourceLocation const& loc) -> std::shared_ptr<awst::Expression> {
				if (val->wtype != awst::WType::biguintType())
					return val;
				auto encode = std::make_shared<awst::ARC4Encode>();
				encode->sourceLocation = loc;
				encode->wtype = arc4SignedType;
				encode->value = std::move(val);
				return encode;
			};

			bool wrapSingleReturn = (signedReturns.size() == 1
				&& signedReturns[0].index == 0
				&& returnParams.size() == 1
				&& method.returnType == awst::WType::biguintType()
				&& _func.modifiers().empty()
				&& !funcHasInlineAssembly);

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
							ret->value = TypeCoercion::signExtendToUint256(
								std::move(ret->value), signedReturns[0].bits, srcLoc);
							if (wrapSingleReturn)
								ret->value = wrapArc4(std::move(ret->value), srcLoc);
						}
						else if (auto* tuple = dynamic_cast<awst::TupleExpression*>(ret->value.get()))
						{
							// Tuple return — sign-extend individual elements
							for (auto const& sr: signedReturns)
							{
								if (sr.index < tuple->items.size())
								{
									tuple->items[sr.index] = TypeCoercion::signExtendToUint256(
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

			if (wrapSingleReturn)
				method.returnType = arc4SignedType;
		}

		// Mask unsigned sub-word return values to their declared bit width.
		// EVM implicitly cleans values on ABI encoding; AVM preserves full uint64.
		if (!unsignedMasks.empty() && method.arc4MethodConfig.has_value())
		{
			auto maskValue = [&](std::shared_ptr<awst::Expression> val,
				unsigned bits, awst::SourceLocation const& loc)
				-> std::shared_ptr<awst::Expression>
			{
				uint64_t mask = (uint64_t(1) << bits) - 1;
				auto maskConst = awst::makeIntegerConstant(std::to_string(mask), loc);
				auto bitAnd = awst::makeUInt64BinOp(std::move(val), awst::UInt64BinaryOperator::BitAnd, std::move(maskConst), loc);
				return bitAnd;
			};

			std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> walkMask;
			walkMask = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts)
			{
				for (auto& stmt: stmts)
				{
					if (auto* ret = dynamic_cast<awst::ReturnStatement*>(stmt.get()))
					{
						if (!ret->value) continue;
						auto srcLoc = ret->value->sourceLocation;
						if (unsignedMasks.size() == 1 && unsignedMasks[0].index == 0
							&& returnParams.size() == 1)
						{
							ret->value = maskValue(std::move(ret->value),
								unsignedMasks[0].bits, srcLoc);
						}
						else if (auto* tuple = dynamic_cast<awst::TupleExpression*>(ret->value.get()))
						{
							for (auto const& um: unsignedMasks)
							{
								if (um.index < tuple->items.size())
									tuple->items[um.index] = maskValue(
										std::move(tuple->items[um.index]), um.bits, srcLoc);
							}
						}
					}
					else if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmt.get()))
					{
						if (ifElse->ifBranch) walkMask(ifElse->ifBranch->body);
						if (ifElse->elseBranch) walkMask(ifElse->elseBranch->body);
					}
					else if (auto* block = dynamic_cast<awst::Block*>(stmt.get()))
						walkMask(block->body);
				}
			};
			walkMask(method.body->body);
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
				auto arc4Var = awst::makeVarExpression(arc4Name, pd.arc4Type, pd.loc);

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

				auto target = awst::makeVarExpression(pd.name, pd.nativeType, pd.loc);

				auto assign = awst::makeAssignmentStatement(std::move(target), std::move(decodeExpr), pd.loc);
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
				// Enums have uint8 ABI encoding
				if (!intType)
					if (auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(solType))
						intType = dynamic_cast<solidity::frontend::IntegerType const*>(
							enumType->encodingType());
				if (!intType || intType->numBits() >= 64)
					continue;

				unsigned bits = intType->numBits();
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

				if (intType->isSigned())
				{
					// Signed sub-64-bit types: validate range but don't mask
					// Valid: value <= maxPos || value >= minNeg
					// maxPos = 2^(n-1) - 1, minNeg = 2^64 - 2^(n-1)
					if (useV2)
					{
						uint64_t maxPos = (uint64_t(1) << (bits - 1)) - 1;
						uint64_t minNeg = ~((uint64_t(1) << (bits - 1)) - 1); // 2^64 - 2^(n-1)

						auto paramCheck1 = awst::makeVarExpression(param->name(), awst::WType::uint64Type(), loc);
						auto maxPosConst = awst::makeIntegerConstant(std::to_string(maxPos), loc);
						auto cmpPos = awst::makeNumericCompare(paramCheck1, awst::NumericComparison::Lte, std::move(maxPosConst), loc);

						auto paramCheck2 = awst::makeVarExpression(param->name(), awst::WType::uint64Type(), loc);
						auto minNegConst = awst::makeIntegerConstant(std::to_string(minNeg), loc);
						auto cmpNeg = awst::makeNumericCompare(paramCheck2, awst::NumericComparison::Gte, std::move(minNegConst), loc);

						// OR the two conditions
						auto orExpr = std::make_shared<awst::BooleanBinaryOperation>();
						orExpr->sourceLocation = loc;
						orExpr->wtype = awst::WType::boolType();
						orExpr->left = std::move(cmpPos);
						orExpr->right = std::move(cmpNeg);
						orExpr->op = awst::BinaryBooleanOperator::Or;

						auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(orExpr), loc, "ABI validation"), loc);
						maskStmts.push_back(std::move(assertStmt));
					}
					// No masking for signed types
					continue;
				}

				uint64_t mask = (uint64_t(1) << bits) - 1;

				if (useV2)
				{
					auto paramCheck = awst::makeVarExpression(param->name(), awst::WType::uint64Type(), loc);

					auto maxVal = awst::makeIntegerConstant(std::to_string(mask), loc);

					auto cmp = awst::makeNumericCompare(paramCheck, awst::NumericComparison::Lte, std::move(maxVal), loc);

					auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), loc, "ABI validation"), loc);
					maskStmts.push_back(std::move(assertStmt));
				}

				auto paramVar = awst::makeVarExpression(param->name(), awst::WType::uint64Type(), loc);

				auto maskConst = awst::makeIntegerConstant(std::to_string(mask), loc);

				auto bitAnd = awst::makeUInt64BinOp(paramVar, awst::UInt64BinaryOperator::BitAnd, std::move(maskConst), loc);

				auto target = awst::makeVarExpression(param->name(), awst::WType::uint64Type(), loc);

				auto assign = awst::makeAssignmentStatement(std::move(target), std::move(bitAnd), loc);
				maskStmts.push_back(std::move(assign));
			}
			// ABI v2 validation for bool params: assert value <= 1
			bool useV2ForBool = true;
			if (m_currentContract)
			{
				auto const& ann = m_currentContract->sourceUnit().annotation();
				if (ann.useABICoderV2.set())
					useV2ForBool = *ann.useABICoderV2;
			}
			if (useV2ForBool)
			{
				for (size_t pi = 0; pi < _func.parameters().size(); ++pi)
				{
					auto const& param = _func.parameters()[pi];
					auto const* solType = param->annotation().type;
					if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
						solType = &udvt->underlyingType();
					if (!dynamic_cast<solidity::frontend::BoolType const*>(solType))
						continue;
					auto loc = makeLoc(param->location());

					auto paramVar = awst::makeVarExpression(param->name().empty()
						? "_param" + std::to_string(pi)
						: param->name(), awst::WType::uint64Type(), loc);

					auto one = awst::makeIntegerConstant("1", loc);

					auto cmp = awst::makeNumericCompare(paramVar, awst::NumericComparison::Lte, std::move(one), loc);

					auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), loc, "ABI bool validation"), loc);
					maskStmts.push_back(std::move(assertStmt));
				}

				// ABI v2 validation for enum params: assert value < member count
				for (size_t pi = 0; pi < _func.parameters().size(); ++pi)
				{
					auto const& param = _func.parameters()[pi];
					auto const* solType = param->annotation().type;
					auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(solType);
					if (!enumType)
						continue;
					auto loc = makeLoc(param->location());
					unsigned memberCount = enumType->numberOfMembers();

					auto paramVar = awst::makeVarExpression(param->name().empty()
						? "_param" + std::to_string(pi)
						: param->name(), awst::WType::uint64Type(), loc);

					auto maxVal = awst::makeIntegerConstant(std::to_string(memberCount - 1), loc);

					auto cmp = awst::makeNumericCompare(paramVar, awst::NumericComparison::Lte, std::move(maxVal), loc);

					auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), loc, "ABI enum validation"), loc);
					maskStmts.push_back(std::move(assertStmt));
				}
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

		// Transient-storage blob init lives in the approval-program preamble
		// (scratch slot TRANSIENT_SLOT, bzero(SLOT_SIZE)). Per-method init
		// would reset the blob mid-dispatch, clobbering writes made by
		// earlier callsub frames in the same app call.


		// Modifier inlining strategy depends on codegen mode:
		// - Legacy (default): textual _ expansion, shared local variables
		// - Via IR: separate subroutines per modifier, fresh vars per _ invocation
		if (!_func.modifiers().empty())
		{
			if (m_viaIR)
				buildModifierChain(_func, method, _contractName);
			else
				inlineModifiers(_func, method.body);
		}

		// Inject ensure_budget for opup budget padding
		// Check per-function map first, then global opup budget
		uint64_t budgetForFunc = 0;
		if (auto it = m_ensureBudget.find(_func.name()); it != m_ensureBudget.end())
			budgetForFunc = it->second;
		else if (m_opupBudget > 0 && method.arc4MethodConfig.has_value())
			budgetForFunc = m_opupBudget;

		if (budgetForFunc > 0)
		{
			auto budgetVal = awst::makeIntegerConstant(std::to_string(budgetForFunc), method.sourceLocation);

			auto feeSource = awst::makeIntegerConstant("0", method.sourceLocation);

			auto call = std::make_shared<awst::PuyaLibCall>();
			call->sourceLocation = method.sourceLocation;
			call->wtype = awst::WType::voidType();
			call->func = "ensure_budget";
			call->args = {
				awst::CallArg{std::string("required_budget"), budgetVal},
				awst::CallArg{std::string("fee_source"), feeSource}
			};

			auto stmt = awst::makeExpressionStatement(std::move(call), method.sourceLocation);

			method.body->body.insert(method.body->body.begin(), std::move(stmt));
		}

		// Non-payable check: for public/external functions not marked `payable`,
		// assert that no preceding PaymentTxn in the group carries a non-zero
		// amount to this contract. Mirrors Solidity's `callvalue` check that
		// reverts non-payable calls receiving ether.
		//
		// Skipped for internal/private (not externally callable) and for the
		// receive() function (implicitly payable).
		bool isPayable =
			_func.stateMutability() == solidity::frontend::StateMutability::Payable;
		if (!isPayable && !_func.isReceive())
			prependNonPayableCheck(method);
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
	static int modRetvalCounter = 0;

	// Extract named return var init statements from the body and hoist them
	// BEFORE modifier arg evaluation. Without this, modifier args like
	// m1(x = 2) m2(y = 3) would have y overwritten by the "y = 0" init
	// that's inside the innermost placeholder body.
	std::set<std::string> returnParamNames;
	for (auto const& rp : _func.returnParameters())
		if (!rp->name().empty())
			returnParamNames.insert(rp->name());

	// Unnamed single-return: synthesise a return var so `return expr;` can be
	// rewritten into `__mod_retval_N = expr;` (in placeholder) + deferred
	// `return __mod_retval_N;` — otherwise the expr would evaluate *after*
	// post-`_` modifier code has run (e.g. `a -= b;` in `mod(x)`), returning
	// a stale value.
	awst::WType const* syntheticRetType = nullptr;
	std::string syntheticRetName;
	if (returnParamNames.empty()
		&& _func.returnParameters().size() == 1
		&& _func.returnParameters()[0]->name().empty())
	{
		syntheticRetType = m_typeMapper.map(_func.returnParameters()[0]->type());
		syntheticRetName = "__mod_retval_" + std::to_string(modRetvalCounter++);
		returnParamNames.insert(syntheticRetName);
	}

	std::vector<std::shared_ptr<awst::Statement>> hoistedInits;
	if (!returnParamNames.empty() && !_body->body.empty())
	{
		// Scan the prefix of the body for compiler-inserted named-return inits.
		// Skip benign non-return-var prefix stmts (ExpressionStatement from
		// ensure_budget/ABI asserts, and parameter narrowing like `a = a & 0xff`)
		// but stop at control flow or any assignment that references a return
		// var in its value to avoid reordering user-written logic.
		std::set<std::string> seen;
		auto it = _body->body.begin();
		while (it != _body->body.end())
		{
			if (auto* assign = dynamic_cast<awst::AssignmentStatement*>(it->get()))
			{
				auto* target = dynamic_cast<awst::VarExpression*>(assign->target.get());
				bool isZeroInit = false;
				auto const* val = assign->value.get();
				if (auto* intConst = dynamic_cast<awst::IntegerConstant const*>(val))
					isZeroInit = (intConst->value == "0");
				else if (auto* boolConst = dynamic_cast<awst::BoolConstant const*>(val))
					isZeroInit = !boolConst->value;
				else if (dynamic_cast<awst::BytesConstant const*>(val))
					isZeroInit = true;
				else if (dynamic_cast<awst::NewStruct const*>(val)
					|| dynamic_cast<awst::NewArray const*>(val)
					|| dynamic_cast<awst::TupleExpression const*>(val))
					isZeroInit = true;

				if (target && returnParamNames.count(target->name) && isZeroInit
					&& !seen.count(target->name))
				{
					seen.insert(target->name);
					hoistedInits.push_back(std::move(*it));
					it = _body->body.erase(it);
					continue;
				}
				// Assignment to a non-return-var (e.g. parameter narrowing
				// `a = a & 0xff`): skip, keep scanning.
				if (target && !returnParamNames.count(target->name))
				{
					++it;
					continue;
				}
				break;
			}
			if (dynamic_cast<awst::ExpressionStatement*>(it->get()))
			{
				++it;
				continue;
			}
			break;
		}
	}
	// Default-init the synthetic return var so the deferred `return` always
	// reads a valid value, even on execution paths that don't reach the split
	// assignment (e.g. early revert inside the modifier).
	if (!syntheticRetName.empty())
	{
		auto synthInit = std::make_shared<awst::AssignmentStatement>();
		synthInit->sourceLocation = makeLoc(_func.location());
		auto target = awst::makeVarExpression(syntheticRetName, syntheticRetType, synthInit->sourceLocation);
		synthInit->target = std::move(target);
		synthInit->value = StorageMapper::makeDefaultValue(syntheticRetType, synthInit->sourceLocation);
		hoistedInits.push_back(std::move(synthInit));
	}

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
				auto target = awst::makeVarExpression(uniqueName, paramType, modLoc);

				auto assignment = awst::makeAssignmentStatement(target, std::move(argExpr), modLoc);
				modBody->body.push_back(std::move(assignment));

				// Register remap so modifier body references resolve to the unique name
				m_exprBuilder->addParamRemap(param->id(), uniqueName, paramType);
				remappedDeclIds.push_back(param->id());
			}
		}

		// Rename modifier-body local variables uniquely per application so that
		// the same modifier applied multiple times (e.g. `mod(2) mod(5)`) does
		// not share storage slots for its own local variables.
		//
		// Skip the rename when the modifier body contains inline assembly:
		// Yul identifiers reference their original Solidity names (see
		// SolInlineAssembly.cpp and AssemblyBuilder::buildIdentifier), so a
		// renamed declaration would decouple the assembly's writes from the
		// surrounding Solidity reads.
		{
			InlineAssemblyDetector asmDetector;
			modDef->body().accept(asmDetector);
			if (!asmDetector.found)
			{
				LocalVarDeclCollector localCollector;
				modDef->body().accept(localCollector);
				for (auto const* localDecl: localCollector.decls)
				{
					std::string uniqueName
						= "__mod_local_" + localDecl->name() + "_" + std::to_string(modCounter++);
					auto* localType = m_typeMapper.map(localDecl->type());
					m_exprBuilder->addParamRemap(localDecl->id(), uniqueName, localType);
					remappedDeclIds.push_back(localDecl->id());
				}
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
						auto target = awst::makeVarExpression(retName, retStmt->value->wtype, retStmt->sourceLocation);

						auto assign = awst::makeAssignmentStatement(std::move(target), retStmt->value, retStmt->sourceLocation);
						placeholderBody->body.push_back(std::move(assign));
					}

					// Create deferred return: return r
					auto retVar = awst::makeVarExpression(retName, retStmt->value->wtype, retStmt->sourceLocation);

					auto deferRet = awst::makeReturnStatement(std::move(retVar), retStmt->sourceLocation);
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

		// Modifier `return` should exit the modifier scope, not the function.
		// Strategy: use a flag __mod_exit_N. Replace return → { flag=true; break; }
		// After each inner loop, check flag and break again if set.
		// Wrap entire modifier body in while(true){...;break;} so outer break works.
		if (translatedModBody)
		{
			static int modExitCounter = 0;
			std::string flagName = "__mod_exit_" + std::to_string(modExitCounter++);
			auto flagLoc = translatedModBody->sourceLocation;

			// Helper: create flag = true assignment
			auto makeFlagSet = [&]() -> std::shared_ptr<awst::Statement> {
				auto target = awst::makeVarExpression(flagName, awst::WType::boolType(), flagLoc);
				auto assign = awst::makeAssignmentStatement(std::move(target), awst::makeBoolConstant(true, flagLoc), flagLoc);
				return assign;
			};

			// Helper: create LoopExit
			auto makeBreak = [&]() -> std::shared_ptr<awst::Statement> {
				auto le = std::make_shared<awst::LoopExit>();
				le->sourceLocation = flagLoc;
				return le;
			};

			// Helper: create if(flag) break;
			auto makeFlagCheck = [&]() -> std::shared_ptr<awst::Statement> {
				auto cond = awst::makeVarExpression(flagName, awst::WType::boolType(), flagLoc);
				auto branchBody = std::make_shared<awst::Block>();
				branchBody->sourceLocation = flagLoc;
				branchBody->body.push_back(makeBreak());
				auto ifStmt = std::make_shared<awst::IfElse>();
				ifStmt->sourceLocation = flagLoc;
				ifStmt->condition = std::move(cond);
				ifStmt->ifBranch = std::move(branchBody);
				return ifStmt;
			};

			// Replace ReturnStatement → { flag=true; break; }
			// Inside loops: also add flag check after the loop
			bool hasReturnInLoop = false;
			std::function<void(std::vector<std::shared_ptr<awst::Statement>>&, bool)> replaceReturns;
			replaceReturns = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts, bool inLoop) {
				for (size_t i = 0; i < stmts.size(); ++i)
				{
					auto& s = stmts[i];
					if (dynamic_cast<awst::ReturnStatement*>(s.get()))
					{
						// Replace return with { flag=true; break; }
						auto block = std::make_shared<awst::Block>();
						block->sourceLocation = s->sourceLocation;
						block->body.push_back(makeFlagSet());
						block->body.push_back(makeBreak());
						s = std::move(block);
						if (inLoop) hasReturnInLoop = true;
					}
					else if (auto* ifElse = dynamic_cast<awst::IfElse*>(s.get()))
					{
						if (ifElse->ifBranch) replaceReturns(ifElse->ifBranch->body, inLoop);
						if (ifElse->elseBranch) replaceReturns(ifElse->elseBranch->body, inLoop);
					}
					else if (auto* block = dynamic_cast<awst::Block*>(s.get()))
						replaceReturns(block->body, inLoop);
					else if (auto* whileLoop = dynamic_cast<awst::WhileLoop*>(s.get()))
					{
						if (whileLoop->loopBody)
							replaceReturns(whileLoop->loopBody->body, /*inLoop=*/true);
					}
				}
			};
			replaceReturns(translatedModBody->body, false);

			// After each WhileLoop that contains a return, insert flag check
			if (hasReturnInLoop)
			{
				std::function<void(std::vector<std::shared_ptr<awst::Statement>>&)> insertFlagChecks;
				insertFlagChecks = [&](std::vector<std::shared_ptr<awst::Statement>>& stmts) {
					for (size_t i = 0; i < stmts.size(); ++i)
					{
						if (dynamic_cast<awst::WhileLoop*>(stmts[i].get()))
						{
							// Insert flag check after the loop
							stmts.insert(stmts.begin() + i + 1, makeFlagCheck());
							++i; // skip inserted
						}
						else if (auto* ifElse = dynamic_cast<awst::IfElse*>(stmts[i].get()))
						{
							if (ifElse->ifBranch) insertFlagChecks(ifElse->ifBranch->body);
							if (ifElse->elseBranch) insertFlagChecks(ifElse->elseBranch->body);
						}
						else if (auto* block = dynamic_cast<awst::Block*>(stmts[i].get()))
							insertFlagChecks(block->body);
					}
				};
				insertFlagChecks(translatedModBody->body);
			}

			// Initialize flag: __mod_exit_N = false
			auto flagInit = std::make_shared<awst::AssignmentStatement>();
			flagInit->sourceLocation = flagLoc;
			auto flagTarget = awst::makeVarExpression(flagName, awst::WType::boolType(), flagLoc);
			flagInit->target = std::move(flagTarget);
			flagInit->value = awst::makeBoolConstant(false, flagLoc);
			modBody->body.push_back(std::move(flagInit));

			// Wrap in while(true) { ...body...; break; }
			auto wrapperLoop = std::make_shared<awst::WhileLoop>();
			wrapperLoop->sourceLocation = flagLoc;
			wrapperLoop->condition = awst::makeBoolConstant(true, flagLoc);

			auto loopBody = std::make_shared<awst::Block>();
			loopBody->sourceLocation = flagLoc;
			for (auto& stmt: translatedModBody->body)
				loopBody->body.push_back(std::move(stmt));
			loopBody->body.push_back(makeBreak()); // always exit after one pass
			wrapperLoop->loopBody = std::move(loopBody);

			modBody->body.push_back(std::move(wrapperLoop));
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

	// Prepend hoisted return var inits before the modifier chain
	if (!hoistedInits.empty())
	{
		_body->body.insert(
			_body->body.begin(),
			std::make_move_iterator(hoistedInits.begin()),
			std::make_move_iterator(hoistedInits.end()));
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

				auto target = awst::makeVarExpression(uniqueName, paramType, modLoc);

				auto assignment = awst::makeAssignmentStatement(target, std::move(argExpr), modLoc);
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
				auto varRef = awst::makeVarExpression(arg.name, arg.wtype, modSub.sourceLocation);
				ca.value = std::move(varRef);
				call->args.push_back(std::move(ca));
			}

			if (!retVarName.empty())
			{
				// Assign call result to return variable
				auto retTarget = awst::makeVarExpression(retVarName, _method.returnType, modSub.sourceLocation);

				auto assign = awst::makeAssignmentStatement(std::move(retTarget), std::move(call), modSub.sourceLocation);
				placeholderBlock->body.push_back(std::move(assign));
			}
			else
			{
				auto stmt = awst::makeExpressionStatement(std::move(call), modSub.sourceLocation);
				placeholderBlock->body.push_back(std::move(stmt));
			}
		}

		// Initialize return variable at the start of the modifier sub
		if (!retVarName.empty())
		{
			auto target = awst::makeVarExpression(retVarName, _method.returnType, modSub.sourceLocation);
			auto zeroVal = StorageMapper::makeDefaultValue(_method.returnType, modSub.sourceLocation);
			auto assign = awst::makeAssignmentStatement(std::move(target), std::move(zeroVal), modSub.sourceLocation);
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
								auto var = awst::makeVarExpression(retVarName, _method.returnType, ret->sourceLocation);
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
			auto retStmt = awst::makeReturnStatement(nullptr, modSub.sourceLocation);
			if (!retVarName.empty())
			{
				auto var = awst::makeVarExpression(retVarName, _method.returnType, modSub.sourceLocation);
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
		auto target = awst::makeVarExpression(rp->name(), rpType, _method.sourceLocation);

		auto zeroVal = StorageMapper::makeDefaultValue(rpType, _method.sourceLocation);
		auto assign = awst::makeAssignmentStatement(std::move(target), std::move(zeroVal), _method.sourceLocation);
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
		auto varRef = awst::makeVarExpression(arg.name, arg.wtype, _method.sourceLocation);
		ca.value = std::move(varRef);
		call->args.push_back(std::move(ca));
	}

	if (_method.returnType != awst::WType::voidType())
	{
		auto retStmt = awst::makeReturnStatement(std::move(call), _method.sourceLocation);
		entryBody->body.push_back(std::move(retStmt));
	}
	else
	{
		auto stmt = awst::makeExpressionStatement(std::move(call), _method.sourceLocation);
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

	// Check if any function has inline assembly (might use .slot / sload / sstore)
	bool hasInlineAsm = false;
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
		for (auto const* func: base->definedFunctions())
			if (func->isImplemented())
				for (auto const& stmt: func->body().statements())
					if (dynamic_cast<solidity::frontend::InlineAssembly const*>(stmt.get()))
					{ hasInlineAsm = true; goto asmCheckDone; }
	asmCheckDone:

	if (layout.totalSlots() == 0 && !hasInlineAsm)
		return;

	std::string cref = m_sourceFile + "." + _contractName;
	awst::SourceLocation loc;
	loc.file = m_sourceFile;

	auto makeUint64 = [&](std::string const& val) {
		auto c = awst::makeIntegerConstant(val, loc);
		return c;
	};

	auto makeBytes = [&](std::string const& s) {
		return awst::makeUtf8BytesConstant(s, loc);
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
		// Default: read from global state using slot key "s" + itob(slot)
		// This supports dynamic slot-based storage references (assembly .slot)
		auto defaultBlock = std::make_shared<awst::Block>();
		defaultBlock->sourceLocation = loc;
		{
			// Use a single large box "__dyn_storage" for all dynamic slots.
			// Each slot occupies 32 bytes at offset (slot % 256) * 32.
			// This avoids per-slot box reference limits (max 8 per txn).
			auto boxKey = makeBytes("__dyn_storage");

			// Compute offset: (__slot % 256) * 32
			auto slotVar = awst::makeVarExpression("__slot", awst::WType::uint64Type(), loc);

			auto mod256 = awst::makeUInt64BinOp(std::move(slotVar), awst::UInt64BinaryOperator::Mod, makeUint64("256"), loc);

			auto offset = awst::makeUInt64BinOp(std::move(mod256), awst::UInt64BinaryOperator::Mult, makeUint64("32"), loc);

			// box_create("__dyn_storage", 8192) — 256 slots * 32 bytes
			auto boxCreate = awst::makeIntrinsicCall("box_create", awst::WType::boolType(), loc);
			boxCreate->stackArgs.push_back(boxKey);
			boxCreate->stackArgs.push_back(makeUint64("8192"));

			auto popStmt = awst::makeExpressionStatement(std::move(boxCreate), loc);
			defaultBlock->body.push_back(std::move(popStmt));

			// box_extract("__dyn_storage", offset, 32)
			auto boxExtract = awst::makeIntrinsicCall("box_extract", awst::WType::bytesType(), loc);
			boxExtract->stackArgs.push_back(std::move(boxKey));
			boxExtract->stackArgs.push_back(std::move(offset));
			boxExtract->stackArgs.push_back(makeUint64("32"));

			auto cast = awst::makeReinterpretCast(std::move(boxExtract), awst::WType::biguintType(), loc);

			auto ret = awst::makeReturnStatement(std::move(cast), loc);
			defaultBlock->body.push_back(std::move(ret));
		}

		std::shared_ptr<awst::Statement> current;
		// Build the chain bottom-up
		std::shared_ptr<awst::Block> elseBlock = defaultBlock;

		for (auto const& sv: layout.variables())
		{
			if (!sv.wtype || sv.wtype == awst::WType::voidType()) continue;

			// Condition: __slot == slotNumber
			auto slotVar = awst::makeVarExpression("__slot", awst::WType::uint64Type(), loc);

			auto cmp = awst::makeNumericCompare(slotVar, awst::NumericComparison::Eq, makeUint64(std::to_string(sv.slot)), loc);

			// If branch: return app_global_get(varName) as biguint
			auto ifBlock = std::make_shared<awst::Block>();
			ifBlock->sourceLocation = loc;
			{
				auto get = awst::makeIntrinsicCall("app_global_get", awst::WType::bytesType(), loc);
				get->stackArgs.push_back(makeBytes(sv.name));

				// Pad to 32 bytes: concat(bzero(32), value), take last 32
				auto bz = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), loc);
				bz->stackArgs.push_back(makeUint64("32"));

				auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), loc);
				cat->stackArgs.push_back(std::move(bz));
				cat->stackArgs.push_back(std::move(get));

				// Extract last 32 bytes
				auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), loc);
				lenCall->stackArgs.push_back(cat);

				auto sub = awst::makeUInt64BinOp(std::move(lenCall), awst::UInt64BinaryOperator::Sub, makeUint64("32"), loc);

				auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), loc);
				extract->stackArgs.push_back(cat);
				extract->stackArgs.push_back(std::move(sub));
				extract->stackArgs.push_back(makeUint64("32"));

				auto cast = awst::makeReinterpretCast(std::move(extract), awst::WType::biguintType(), loc);

				auto ret = awst::makeReturnStatement(std::move(cast), loc);
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
		// Default: write to global state using slot key "s" + itob(slot)
		{
			// Build key: concat("s", itob(__slot))
			auto prefix = makeBytes("s");
			auto slotItob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), loc);
			auto slotVar = awst::makeVarExpression("__slot", awst::WType::uint64Type(), loc);
			slotItob->stackArgs.push_back(std::move(slotVar));

			auto key = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), loc);
			key->stackArgs.push_back(std::move(prefix));
			key->stackArgs.push_back(std::move(slotItob));

			// Use single "__dyn_storage" box, same as read
			auto boxKey = makeBytes("__dyn_storage");

			// Compute offset: (__slot % 256) * 32
			auto slotVar2 = awst::makeVarExpression("__slot", awst::WType::uint64Type(), loc);

			auto mod256 = awst::makeUInt64BinOp(std::move(slotVar2), awst::UInt64BinaryOperator::Mod, makeUint64("256"), loc);

			auto offset = awst::makeUInt64BinOp(std::move(mod256), awst::UInt64BinaryOperator::Mult, makeUint64("32"), loc);

			// value as bytes (pad to 32)
			auto valueVar = awst::makeVarExpression("__value", awst::WType::biguintType(), loc);

			auto valBytes = awst::makeReinterpretCast(std::move(valueVar), awst::WType::bytesType(), loc);

			// Pad to 32 bytes
			auto bz = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), loc);
			bz->stackArgs.push_back(makeUint64("32"));

			auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), loc);
			cat->stackArgs.push_back(std::move(bz));
			cat->stackArgs.push_back(std::move(valBytes));

			auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), loc);
			lenCall->stackArgs.push_back(cat);

			auto sub32 = awst::makeUInt64BinOp(std::move(lenCall), awst::UInt64BinaryOperator::Sub, makeUint64("32"), loc);

			auto paddedVal = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), loc);
			paddedVal->stackArgs.push_back(cat);
			paddedVal->stackArgs.push_back(std::move(sub32));
			paddedVal->stackArgs.push_back(makeUint64("32"));

			// box_create("__dyn_storage", 8192) — ensure box exists
			auto boxCreate = awst::makeIntrinsicCall("box_create", awst::WType::boolType(), loc);
			boxCreate->stackArgs.push_back(boxKey);
			boxCreate->stackArgs.push_back(makeUint64("8192"));

			auto createStmt = awst::makeExpressionStatement(std::move(boxCreate), loc);
			defaultBlock->body.push_back(std::move(createStmt));

			// box_replace("__dyn_storage", offset, padded_value)
			auto boxReplace = awst::makeIntrinsicCall("box_replace", awst::WType::voidType(), loc);
			boxReplace->stackArgs.push_back(std::move(boxKey));
			boxReplace->stackArgs.push_back(std::move(offset));
			boxReplace->stackArgs.push_back(std::move(paddedVal));

			auto replaceStmt = awst::makeExpressionStatement(std::move(boxReplace), loc);
			defaultBlock->body.push_back(std::move(replaceStmt));

			auto ret = awst::makeReturnStatement(nullptr, loc);
			defaultBlock->body.push_back(std::move(ret));
		}

		std::shared_ptr<awst::Block> elseBlock = defaultBlock;

		for (auto const& sv: layout.variables())
		{
			if (!sv.wtype || sv.wtype == awst::WType::voidType()) continue;

			auto slotVar = awst::makeVarExpression("__slot", awst::WType::uint64Type(), loc);

			auto cmp = awst::makeNumericCompare(slotVar, awst::NumericComparison::Eq, makeUint64(std::to_string(sv.slot)), loc);

			auto ifBlock = std::make_shared<awst::Block>();
			ifBlock->sourceLocation = loc;
			{
				// app_global_put(varName, pad32(value_as_bytes))
				// Pad to 32 bytes to match EVM slot semantics
				auto valueVar = awst::makeVarExpression("__value", awst::WType::biguintType(), loc);

				auto cast = awst::makeReinterpretCast(std::move(valueVar), awst::WType::bytesType(), loc);

				// concat(bzero(32), bytes) → take last 32 bytes
				auto bz = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), loc);
				bz->stackArgs.push_back(makeUint64("32"));

				auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), loc);
				cat->stackArgs.push_back(std::move(bz));
				cat->stackArgs.push_back(std::move(cast));

				auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), loc);
				lenCall->stackArgs.push_back(cat);

				auto sub32 = awst::makeUInt64BinOp(std::move(lenCall), awst::UInt64BinaryOperator::Sub, makeUint64("32"), loc);

				auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), loc);
				extract->stackArgs.push_back(cat);
				extract->stackArgs.push_back(std::move(sub32));
				extract->stackArgs.push_back(makeUint64("32"));

				auto put = awst::makeIntrinsicCall("app_global_put", awst::WType::voidType(), loc);
				put->stackArgs.push_back(makeBytes(sv.name));
				put->stackArgs.push_back(std::move(extract));

				auto stmt = awst::makeExpressionStatement(std::move(put), loc);
				ifBlock->body.push_back(std::move(stmt));

				auto ret = awst::makeReturnStatement(nullptr, loc);
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
