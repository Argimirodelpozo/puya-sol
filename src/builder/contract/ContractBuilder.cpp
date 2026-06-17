#include "builder/contract/ContractBuilder.h"
#include "builder/NatSpecTags.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-ast/calls/SolNewExpression.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/itxn/FunctionPointerBuilder.h"
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

// Free functions shared by AWSTBuilder (library/free-function path) and
// ContractBuilder (contract-method path).

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

namespace {

/// Collect decl IDs of memory aggregate locals referenced as VALUES in any
/// inline-assembly block in a function body. In Yul such a reference is the
/// aggregate's memory pointer (a uint256 offset), so we promote these to
/// blob-backed (SolVariableDeclaration) and resolve them to a uint64 offset in
/// the assembly translator.
class AssemblyAggregateScanner: public solidity::frontend::ASTConstVisitor
{
public:
	std::set<int64_t>& ids;
	explicit AssemblyAggregateScanner(std::set<int64_t>& _ids): ids(_ids) {}

	bool visit(solidity::frontend::InlineAssembly const& _asm) override
	{
		for (auto const& ref: _asm.annotation().externalReferences)
		{
			auto const* vd = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
				ref.second.declaration);
			if (!vd
				|| vd->referenceLocation()
					!= solidity::frontend::VariableDeclaration::Location::Memory)
				continue;
			auto const* t = vd->type();
			// bytes/string keep their dedicated assembly handling (tryHandleBytes*);
			// promoting them to blob-backed breaks value uses (x[i]=, x.length,
			// return x). Real arrays + structs only (bytes-Yul-libs handled later).
			if (auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(t))
			{
				if (!at->isByteArrayOrString())
					ids.insert(vd->id());
			}
			else if (dynamic_cast<solidity::frontend::StructType const*>(t))
				ids.insert(vd->id());
		}
		return true;
	}
};

} // namespace

std::shared_ptr<awst::Block> buildBlock(
	FunctionTranslationCtx& _ctx,
	solidity::frontend::Block const& _block,
	std::shared_ptr<awst::Block> _placeholder)
{
	sol_ast::FunctionContext fn{_ctx.tr, _ctx.params, _ctx.returnType, _ctx.paramBitWidths};
	fn.inConstructor = _ctx.inConstructor;
	fn.frameIsProgram = _ctx.frameIsProgram;
	auto fnGuard = _ctx.exprBuilder.pushScopeRaii(&fn);
	auto blk = _placeholder
		? sol_ast::BlockContext::top(fn).withPlaceholder(_placeholder)
		: sol_ast::BlockContext::top(fn);
	auto blkGuard = _ctx.exprBuilder.pushScopeRaii(&blk);

	// Mapping storage-ref params: `m[k]` resolves the dynamic box-key prefix at runtime.
	for (auto const* mp: _ctx.mappingKeyParams)
		if (mp && !mp->name().empty())
			fn.setMappingKeyParam(mp->id(), mp->name());

	// Named returns >4 KB: blob-backed aggregates (pointer model) so `p.field[i]`
	// lowers to multi-slot blob word access. Base offset assigned + FMP bumped in
	// FunctionBuilder.
	for (auto const* rp: _ctx.namedReturns)
	{
		if (!rp || rp->name().empty()
			|| rp->referenceLocation() != solidity::frontend::VariableDeclaration::Location::Memory)
			continue;
		auto const* rpType = _ctx.typeMapper.map(rp->type());
		if (computeEncodedElementSize(rpType) > AssemblyBuilder::SLOT_SIZE)
			fn.setBlobAggregate(rp->id(), "__blobagg_off_" + std::to_string(rp->id()));
	}

	// Blob-agg params >4 KB: param's local IS the uint64 base offset (caller passed
	// it — see SolInternalCall/SolIdentifier); no FMP bump needed.
	for (auto const* p: _ctx.blobAggParams)
		if (p && !p->name().empty())
			fn.setBlobAggregate(p->id(), p->name());

	// Promote memory aggregates used as values in inline assembly to blob-backed
	// (Yul memory pointer). Must mark before body translation so SolVariableDeclaration
	// blob-backs them at their declaration. Not run during modifier re-entrancy
	// (_placeholder set) — pre-built placeholder contexts are unsafe to re-walk.
	if (!_placeholder)
	{
		std::set<int64_t> asmAggIds;
		AssemblyAggregateScanner scanner{asmAggIds};
		_block.accept(scanner);
		for (int64_t id: asmAggIds)
			fn.markAssemblyAggregate(id);
	}

	return sol_ast::buildBlock(blk, _block);
}

// ContractBuilder wrappers — route through free-function API.

awst::SourceLocation ContractBuilder::makeLoc(
	solidity::langutil::SourceLocation const& _solLoc
)
{
	return ::puyasol::builder::makeLoc(m_sourceFile, _solLoc);
}

FunctionTranslationCtx ContractBuilder::makeFunctionCtx()
{
	auto ctx = FunctionTranslationCtx{
		m_typeMapper,
		*m_exprBuilder,
		*m_tr,
		m_sourceFile,
		m_currentParams,
		m_currentReturnType,
		m_currentBitWidths,
		m_currentNamedReturns,
		m_currentMappingKeyParams,
		m_currentBlobAggParams,
		m_currentContract,
	};
	ctx.inConstructor = m_currentInConstructor;
	ctx.frameIsProgram = m_currentFrameIsProgram;
	return ctx;
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
	// Only ARC4-dispatched methods are externally callable.
	if (!_method.arc4MethodConfig.has_value())
		return;
	if (!_method.body)
		return;

	auto loc = _method.sourceLocation;

	auto groupIdx = awst::makeTxn(std::string("GroupIndex"), awst::WType::uint64Type(), loc);

	auto hasPayment = awst::makeNumericCompare(
		groupIdx, awst::NumericComparison::Gt,
		awst::makeIntegerConstant("0", loc), loc);

	auto groupIdx2 = awst::makeTxn(std::string("GroupIndex"), awst::WType::uint64Type(), loc);
	auto payIdx = awst::makeUInt64BinOp(
		std::move(groupIdx2), awst::UInt64BinaryOperator::Sub,
		awst::makeIntegerConstant("1", loc), loc);

	auto amount = awst::makeGtxns(
		"Amount", std::move(payIdx), awst::WType::uint64Type(), loc);

	// Mirrors msg.value shape — avoids GroupIndex-1 when GroupIndex==0 (underflow-safe).
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

	// Reset Yul subroutine sink (drained below).
	AssemblyBuilder::resetPendingSubroutines();

	// Collect transient state variables
	m_transientStorage.collectVars(_contract, m_typeMapper);
	// Note: setTransientStorage called after m_exprBuilder is created (below)

	// Overloaded names: true overloads (same name, different params) only;
	// virtual overrides occupy the same slot and don't count.
	// Must be computed before translator creation so ctor uses correct names.
	m_overloadedNames.clear();
	{
		std::set<int64_t> overriddenIds;
		forEachDefinedFunction(_contract, [&](auto const* func)
		{
			if (func->isConstructor() || !func->isImplemented())
				return;
			// Mark all base functions of this override as overridden
			for (auto const* baseFunc: func->annotation().baseFunctions)
				overriddenIds.insert(baseFunc->id());
		});

		std::unordered_map<std::string, int> nameCount;
		forEachDefinedFunction(_contract, [&](auto const* func)
		{
			if (func->isConstructor() || !func->isImplemented())
				return;
			// Skip functions that have been overridden by a more-derived version
			if (overriddenIds.count(func->id()))
				return;
			nameCount[func->name()]++;
		});
		for (auto const& [name, count]: nameCount)
		{
			if (count > 1)
			{
				m_overloadedNames.insert(name);
				Logger::instance().debug("Overloaded function: " + name + " (" + std::to_string(count) + " versions)");
			}
		}
	}

	m_exprBuilder = std::make_unique<eb::ContractContext>(
		m_typeMapper, m_storageMapper, m_sourceFile, contractName,
		m_libraryFunctionIds, m_overloadedNames, m_freeFunctionById
	);
	m_exprBuilder->currentContract = &_contract;

	// Pre-populate internalized library func map before translation so the call
	// resolver routes them as InstanceMethodTargets.
	for (auto const* libFunc : m_internalizableLibFuncs)
	{
		if (!libFunc) continue;
		auto const* libContract = libFunc->annotation().contract;
		std::string methodName = "__intlib_"
			+ (libContract ? libContract->name() : std::string("L"))
			+ "_" + libFunc->name();
		m_exprBuilder->internalizedLibFuncNames[libFunc->id()] = methodName;
	}

	// In-place emplace — TranslationContext caches a pointer to its own scopeState_;
	// copy/move construction would dangle that pointer.
	m_tr.emplace(*m_exprBuilder, m_typeMapper, m_sourceFile);
	m_exprBuilder->currentScope = &*m_tr;
	m_currentParams.clear();
	m_currentReturnType = nullptr;
	m_currentBitWidths.clear();
	m_currentPlaceholder.reset();
	m_currentNamedReturns.clear();
	m_currentMappingKeyParams.clear();
	m_currentBlobAggParams.clear();

	m_exprBuilder->transientStorage =
		m_transientStorage.hasTransientVars() ? &m_transientStorage : nullptr;
	// StorageBackend is per-contract (TransientStorage is per-contract).
	m_storageBackend.emplace(m_storageMapper, m_exprBuilder->transientStorage);
	m_exprBuilder->storageBackend = &*m_storageBackend;

	eb::FunctionPointerBuilder::setCurrentCref(contractId);

	auto contract = std::make_shared<awst::Contract>();
	contract->sourceLocation = makeLoc(_contract.location());
	contract->id = contractId;
	contract->name = contractName;

	if (_contract.documentation())
	{
		std::string const& doc = *_contract.documentation()->text();
		contract->description = doc;
		// uros splitter opt-in: `@custom:splitter <selector>` (e.g. "uros").
		contract->splitter = natSpecTagValue(doc, "custom:splitter");
	}

	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		if (base != &_contract)
			contract->methodResolutionOrder.push_back(
				m_sourceFile + "." + base->name()
			);
	}

	contract->appState = m_storageMapper.mapStateVariables(_contract, m_sourceFile);

	// EVM-memory scratch slots 0..MEMORY_SLOT_LAST (default 0-4; raisable via
	// --evm-memory-slots) plus transient + flash-accounting slots.
	contract->reservedScratchSpace = AssemblyBuilder::reservedScratchSlots();

	collectSuperCallMetadata(_contract);

	// Snapshot super targets so the ctor body (translated in buildApprovalProgram)
	// can resolve super.f() to f__super_N rather than the contract's own f.
	m_allSuperTargetNames = m_tr->allSuperTargets();

	// Approval and clear programs
	m_postInitMethod.reset();
	contract->approvalProgram = buildApprovalProgram(_contract, contractName);
	contract->clearProgram = buildClearProgram(_contract, contractName);

	if (m_postInitMethod)
	{
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


	std::set<std::string> translatedFunctions;
	for (auto const* func: _contract.definedFunctions())
	{
		if (func->isConstructor())
			continue;

		std::string key = func->name();
		if (m_overloadedNames.count(key))
			key += "#" + std::to_string(func->id());
		translatedFunctions.insert(key);
		clearSuperOverrides();
		applySuperOverridesFor(func->id());
		// fallback/receive have empty Solidity names; give explicit memberName.
		std::string nameOverride;
		if (func->isFallback())
			nameOverride = "__fallback";
		else if (func->isReceive())
			nameOverride = "__receive";
		auto method = buildFunction(*func, contractName, nameOverride);
		contract->methods.push_back(std::move(method));
		for (auto& sub: m_modifierSubroutines)
			contract->methods.push_back(std::move(sub));
		m_modifierSubroutines.clear();
	}

	// Getters before inherited functions so `uint256 public override test` beats
	// an inherited `function test()`.
	buildPublicStateVariableGetters(_contract, *contract, contractName, translatedFunctions);

	// Inherited functions (after getters — same precedence rule).
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

	// uros splitter: the backend requires EVERY ABI method to declare a chunk
	// when the contract opts in. User methods get theirs from @custom:uros-chunk,
	// but compiler-synthesized ABI methods (public-state-var getters, __postInit,
	// __fallback, __receive) have none. Assign any still-unchunked ABI method to
	// a default "shell" chunk so the backend can place them. No effect unless the
	// contract set @custom:splitter, so non-split contracts are unchanged.
	if (!contract->splitter.empty())
	{
		for (auto& m: contract->methods)
		{
			if (!m.arc4MethodConfig.has_value())
				continue;
			if (auto* abi = std::get_if<awst::ARC4ABIMethodConfig>(&*m.arc4MethodConfig))
			{
				if (abi->chunk.empty())
					abi->chunk = "shell";
			}
		}
	}

	return contract;
}



} // namespace puyasol::builder
