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
	bool _viaIR,
	std::vector<solidity::frontend::FunctionDefinition const*> const& _internalizableLibFuncs
)
	: m_typeMapper(_typeMapper),
	  m_storageMapper(_storageMapper),
	  m_sourceFile(_sourceFile),
	  m_libraryFunctionIds(_libraryFunctionIds),
	  m_opupBudget(_opupBudget),
	  m_freeFunctionById(_freeFunctionById),
	  m_ensureBudget(_ensureBudget),
	  m_viaIR(_viaIR),
	  m_internalizableLibFuncs(_internalizableLibFuncs)
{
}

// ── Shared free-function API ────────────────────────────────────────────
// `makeLoc` / `buildBlock` are exposed as free functions so the
// library/free-function path (in AWSTBuilder) and the contract-method path
// (in ContractBuilder) can share the same translation primitives.

awst::SourceLocation makeLoc(
	std::string const& _sourceFile,
	solidity::langutil::SourceLocation const& _solLoc)
{
	awst::SourceLocation loc;
	loc.file = _sourceFile;
	loc.line = _solLoc.start >= 0 ? _solLoc.start : 0;
	loc.endLine = _solLoc.end >= 0 ? _solLoc.end : 0;
	return loc;
}

std::shared_ptr<awst::Block> buildBlock(
	FunctionTranslationCtx& _ctx,
	solidity::frontend::Block const& _block,
	std::shared_ptr<awst::Block> _placeholder)
{
	// Build a fresh per-function context, then a top BlockContext (optionally
	// with the placeholder body for modifier inlining), then dispatch.
	sol_ast::FunctionContext fn{_ctx.tr, _ctx.params, _ctx.returnType, _ctx.paramBitWidths};
	auto fnGuard = _ctx.exprBuilder.pushScopeRaii(&fn);
	auto blk = _placeholder
		? sol_ast::BlockContext::top(fn).withPlaceholder(_placeholder)
		: sol_ast::BlockContext::top(fn);
	auto blkGuard = _ctx.exprBuilder.pushScopeRaii(&blk);

	// Register named return parameters so inner-block declarations of the
	// same name get the unique-name shadow rename. Must happen *after* the
	// BlockContext is pushed — `resolveVarName` writes into the innermost
	// enclosing block via `nearestBlock(currentScope)`.
	for (auto const* rp: _ctx.namedReturns)
		if (rp && !rp->name().empty())
			blk.resolveVarName(rp->name(), rp->id());

	// Register mapping-storage-ref params on the FunctionContext so that
	// `m[k]` inside the body resolves the dynamic box-key prefix at
	// runtime. Same scope-push ordering constraint as the named returns.
	for (auto const* mp: _ctx.mappingKeyParams)
		if (mp && !mp->name().empty())
			fn.setMappingKeyParam(mp->id(), mp->name());

	return sol_ast::buildBlock(blk, _block);
}

// ── ContractBuilder thin wrappers (route through free-function API) ─────

awst::SourceLocation ContractBuilder::makeLoc(
	solidity::langutil::SourceLocation const& _solLoc
)
{
	return ::puyasol::builder::makeLoc(m_sourceFile, _solLoc);
}

FunctionTranslationCtx ContractBuilder::makeFunctionCtx()
{
	return FunctionTranslationCtx{
		m_typeMapper,
		*m_exprBuilder,
		*m_tr,
		m_sourceFile,
		m_currentParams,
		m_currentReturnType,
		m_currentBitWidths,
		m_currentNamedReturns,
		m_currentMappingKeyParams,
		m_currentContract,
	};
}

std::shared_ptr<awst::Block> ContractBuilder::buildBlock(
	solidity::frontend::Block const& _block)
{
	auto ctx = makeFunctionCtx();
	return ::puyasol::builder::buildBlock(ctx, _block, m_currentPlaceholder);
}

void ContractBuilder::setFunctionContext(
	std::vector<std::pair<std::string, awst::WType const*>> const& _params,
	awst::WType const* _returnType,
	std::map<std::string, unsigned> const& _bitWidths)
{
	m_currentParams = _params;
	m_currentReturnType = _returnType;
	m_currentBitWidths = _bitWidths;
}

void ContractBuilder::setPlaceholderBody(std::shared_ptr<awst::Block> _body)
{
	m_currentPlaceholder = std::move(_body);
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
	auto msgValue = awst::makeConditional(
		std::move(hasPayment), std::move(amount),
		awst::makeIntegerConstant("0", loc),
		awst::WType::uint64Type(), loc);

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
	m_exprBuilder = std::make_unique<eb::ContractContext>(
		m_typeMapper, m_storageMapper, m_sourceFile, contractName,
		m_libraryFunctionIds, m_overloadedNames, m_freeFunctionById
	);
	m_exprBuilder->currentContract = &_contract;

	// Pre-populate the internalized library function map BEFORE translating any
	// methods, so call resolver routes calls to these funcs as InstanceMethodTargets.
	for (auto const* libFunc : m_internalizableLibFuncs)
	{
		if (!libFunc) continue;
		auto const* libContract = libFunc->annotation().contract;
		std::string methodName = "__intlib_"
			+ (libContract ? libContract->name() : std::string("L"))
			+ "_" + libFunc->name();
		m_exprBuilder->internalizedLibFuncNames[libFunc->id()] = methodName;
	}

	// Build the per-contract TranslationContext. FunctionContext + BlockContext
	// are constructed locally (in buildBlock and similar) on top of this.
	// In-place emplace (forwarded args, no temporary) — TranslationContext
	// caches a pointer to its own scopeState_ member, which would dangle if
	// the object were copy/move-constructed from a temporary.
	m_tr.emplace(*m_exprBuilder, m_typeMapper, m_sourceFile);
	m_exprBuilder->currentScope = &*m_tr;
	m_currentParams.clear();
	m_currentReturnType = nullptr;
	m_currentBitWidths.clear();
	m_currentPlaceholder.reset();
	m_currentNamedReturns.clear();
	m_currentMappingKeyParams.clear();

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
	m_allSuperTargetNames = m_tr->allSuperTargets();

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

	// Emit internalized library functions as internal methods of this contract.
	// These are library funcs with internal function-pointer params: their body
	// invokes the funcptr dispatcher which case-branches to contract instance
	// methods, so they must live in the contract's scope (puya rejects calling
	// instance methods from root-level subroutines).
	for (auto const* libFunc : m_internalizableLibFuncs)
	{
		if (!libFunc || !libFunc->isImplemented()) continue;
		auto nameIt = m_exprBuilder->internalizedLibFuncNames.find(libFunc->id());
		if (nameIt == m_exprBuilder->internalizedLibFuncNames.end()) continue;
		clearSuperOverrides();
		auto method = buildFunction(*libFunc, contractName, nameIt->second);
		contract->methods.push_back(std::move(method));
		for (auto& sub: m_modifierSubroutines)
			contract->methods.push_back(std::move(sub));
		m_modifierSubroutines.clear();
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
