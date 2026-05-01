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

	// Collect super.f() and Base.f() target metadata across the MRO and the
	// contract's constructor (see contract/SuperCallResolution.cpp).
	collectSuperCallMetadata(_contract);

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
		applySuperOverridesFor(func->id());
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
	buildPublicStateVariableGetters(_contract, *contract, contractName, translatedFunctions);

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
			applySuperOverridesFor(func->id());
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

	// Emit MRO / fallback / explicit-base super subroutines now that all
	// regular method bodies are translated.
	emitSuperSubroutines(*contract, contractName);

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
