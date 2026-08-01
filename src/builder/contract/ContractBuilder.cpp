#include "builder/contract/ContractBuilder.h"
#include "awst/NameGen.h"
#include "builder/NatSpecTags.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-ast/calls/SolNewExpression.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/itxn/FunctionPointerBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/storage/StorageLayout.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>
#include <libyul/AST.h>

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
// True iff `_vd` (a local) is initialised by `new T(...)` — walk its scope block
// for the declaring statement. Used to gate blob-backing of bytes/string asm
// buffers: only a freshly-`new`ed buffer (the OZ Strings.toString idiom) is
// promoted to the memory-pointer model; a bytes/string VALUE used in asm
// (`ret := val`) stays value-model.
static bool _isNewAllocatedLocal(solidity::frontend::VariableDeclaration const* _vd)
{
	using namespace solidity::frontend;
	auto const* block = dynamic_cast<Block const*>(_vd->scope());
	if (!block)
		return false;
	for (auto const& stmt: block->statements())
	{
		auto const* vds = dynamic_cast<VariableDeclarationStatement const*>(stmt.get());
		if (!vds || !vds->initialValue())
			continue;
		bool declares = false;
		for (auto const& d: vds->declarations())
			if (d && d->id() == _vd->id())
				declares = true;
		if (!declares)
			continue;
		auto const* fc = dynamic_cast<FunctionCall const*>(vds->initialValue());
		return fc && dynamic_cast<NewExpression const*>(&fc->expression()) != nullptr;
	}
	return false;
}

// True iff any of `_targets` (Yul identifier nodes referencing the buffer)
// appears inside `_e` (a Yul expression subtree).
static bool _yulExprRefs(
	solidity::yul::Expression const& _e,
	std::set<solidity::yul::Identifier const*> const& _targets)
{
	using namespace solidity::yul;
	if (auto const* id = std::get_if<Identifier>(&_e))
		return _targets.count(id) != 0;
	if (auto const* fc = std::get_if<FunctionCall>(&_e))
		for (auto const& arg: fc->arguments)
			if (_yulExprRefs(arg, _targets))
				return true;
	return false;
}

// True iff the buffer's pointer ESCAPES into another Yul variable within `_b` —
// i.e. it feeds the RHS of an assignment (`ptr := add(buffer, k)`) or a `let`.
// This is the exact signal that the value-model store handlers (mstore/mstore8
// with the buffer directly in the address, e.g. `mstore(add(x,32), w)`) can NOT
// cover the writes: once the pointer lives in an opaque local, only the blob
// (memory-pointer) model tracks it. A buffer used solely as a direct store
// address is left value-model. Recurses into nested control-flow blocks.
static bool _yulBlockEscapes(
	solidity::yul::Block const& _b,
	std::set<solidity::yul::Identifier const*> const& _targets)
{
	using namespace solidity::yul;
	for (auto const& stmt: _b.statements)
	{
		bool esc = std::visit([&](auto const& s) -> bool {
			using T = std::decay_t<decltype(s)>;
			if constexpr (std::is_same_v<T, Assignment>)
				return s.value && _yulExprRefs(*s.value, _targets);
			else if constexpr (std::is_same_v<T, VariableDeclaration>)
				return s.value && _yulExprRefs(*s.value, _targets);
			else if constexpr (std::is_same_v<T, Block>)
				return _yulBlockEscapes(s, _targets);
			else if constexpr (std::is_same_v<T, If>)
				return _yulBlockEscapes(s.body, _targets);
			else if constexpr (std::is_same_v<T, Switch>)
			{
				for (auto const& c: s.cases)
					if (_yulBlockEscapes(c.body, _targets))
						return true;
				return false;
			}
			else if constexpr (std::is_same_v<T, ForLoop>)
				return _yulBlockEscapes(s.pre, _targets)
					|| _yulBlockEscapes(s.post, _targets)
					|| _yulBlockEscapes(s.body, _targets);
			else if constexpr (std::is_same_v<T, FunctionDefinition>)
				return _yulBlockEscapes(s.body, _targets);
			else
				return false;
		}, stmt);
		if (esc)
			return true;
	}
	return false;
}

// True iff `_vd`'s memory pointer escapes into a Yul local inside `_asm`.
static bool _bufferPointerEscapes(
	solidity::frontend::InlineAssembly const& _asm,
	solidity::frontend::VariableDeclaration const* _vd)
{
	std::set<solidity::yul::Identifier const*> targets;
	for (auto const& ref: _asm.annotation().externalReferences)
		if (ref.second.declaration == _vd)
			targets.insert(ref.first);
	if (targets.empty())
		return false;
	return _yulBlockEscapes(_asm.operations().root(), targets);
}

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
			if (auto const* at = dynamic_cast<solidity::frontend::ArrayType const*>(t))
			{
				// Real arrays: always blob-back. bytes/string keep the value model
				// (dedicated tryHandleBytes* handlers preserve x[i]=/x.length/return x,
				// incl. direct `mstore(add(x,32), w)` word writes) EXCEPT a freshly-
				// `new`ed buffer whose pointer ESCAPES into a Yul local — the OZ
				// Strings.toString idiom (`ptr := add(buffer, k)` + `mstore8(ptr,…)`
				// + `return buffer`). Only then do the value handlers lose the writes,
				// so blob-back it (memory-pointer model).
				if (!at->isByteArrayOrString()
					|| (_isNewAllocatedLocal(vd) && _bufferPointerEscapes(_asm, vd)))
					ids.insert(vd->id());
			}
			else if (dynamic_cast<solidity::frontend::StructType const*>(t))
				ids.insert(vd->id());
		}
		return true;
	}
};

} // namespace

void markAssemblyAggregates(
	sol_ast::FunctionContext& _fn,
	solidity::frontend::Block const& _block)
{
	std::set<int64_t> asmAggIds;
	AssemblyAggregateScanner scanner{asmAggIds};
	_block.accept(scanner);
	for (int64_t id: asmAggIds)
		_fn.markAssemblyAggregate(id);
}

std::shared_ptr<awst::Block> buildBlock(
	FunctionTranslationCtx& _ctx,
	solidity::frontend::Block const& _block,
	std::shared_ptr<awst::Block> _placeholder)
{
	sol_ast::FunctionContext fn{_ctx.tr, _ctx.params, _ctx.returnType, _ctx.paramBitWidths};
	fn.paramSolTypes = _ctx.paramSolTypes;
	fn.inConstructor = _ctx.inConstructor;
	fn.frameIsProgram = _ctx.frameIsProgram;
	fn.encodeReturnsAtBuildTime = _ctx.encodeReturnsAtBuildTime;
	fn.returnAsmWrap = _ctx.returnAsmWrap;
	fn.returnWirePlan = _ctx.returnWirePlan;
	if (_ctx.seededCalldataPointers)
		fn.seededCalldataPointers = _ctx.seededCalldataPointers;
	auto fnGuard = _ctx.exprBuilder.pushScopeRaii(&fn);
	auto blk = _placeholder
		? sol_ast::BlockContext::top(fn).withPlaceholder(_placeholder)
		: sol_ast::BlockContext::top(fn);
	auto blkGuard = _ctx.exprBuilder.pushScopeRaii(&blk);

	// Mapping storage-ref params: `m[k]` resolves the dynamic box-key prefix at runtime.
	for (auto const* mp: _ctx.mappingKeyParams)
		if (mp && !mp->name().empty())
			fn.setMappingKeyParam(mp->id(), mp->name());

	// Offset-convention struct-ref params (handle-model dual handle): register the companion
	// uint64 offset var so the body's `s.field` writes hit the element slice via
	// box_replace(key, offset+fieldOff). The offset param itself is in the subroutine signature
	// (FunctionBuilder) and supplied by the caller (SolInternalCall).
	for (auto const* mp: _ctx.mappingKeyParams)
		if (mp && !mp->name().empty() && structRefOffsetParamsRegistry().count(mp->id()))
			fn.setStructRefOffset(mp->id(), mp->name() + "__off");

	// Named returns >4 KB: blob-backed aggregates (pointer model) so `p.field[i]`
	// lowers to multi-slot blob word access. Base offset assigned + FMP bumped in
	// FunctionBuilder.
	for (auto const* rp: _ctx.namedReturns)
	{
		if (!rp || rp->name().empty()
			|| rp->referenceLocation() != solidity::frontend::VariableDeclaration::Location::Memory)
			continue;
		auto const* rpType = _ctx.typeMapper.map(rp->type());
		if (memoryUsesBlob(rpType))
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
		markAssemblyAggregates(fn, _block);

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
	ctx.encodeReturnsAtBuildTime = m_currentEncodeReturnsAtBuildTime;
	ctx.returnAsmWrap = m_currentReturnAsmWrap;
	ctx.returnWirePlan = m_currentReturnWirePlan;
	ctx.seededCalldataPointers = &m_currentSeededCalldataPointers;
	ctx.paramSolTypes = m_currentParamSolTypes;
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
	std::map<std::string, unsigned> const& _bitWidths,
	std::map<std::string, solidity::frontend::Type const*> const& _paramSolTypes)
{
	m_currentParams = _params;
	m_currentReturnType = _returnType;
	m_currentBitWidths = _bitWidths;
	m_currentParamSolTypes = _paramSolTypes;
	// Per-function reset: build-time return encoding is opt-in per function
	// (setReturnWirePlan). Clear here so a function that does NOT opt in never
	// inherits the previous function's plan.
	m_currentEncodeReturnsAtBuildTime = false;
	m_currentReturnAsmWrap = false;
	m_currentReturnWirePlan.clear();
	m_currentSeededCalldataPointers.clear();
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

	// Reset the generated-name counters: a contract's temp/subroutine names
	// (`__mod_retval_N`, `f__mod0_N`, …) must depend only on its own content,
	// not on how many contracts compiled before it in the batch (deterministic
	// multi-contract output; prerequisite for parallel per-contract compiles).
	awst::NameGen::resetAll();

	// Reset Yul subroutine sink (drained below).
	AssemblyBuilder::resetPendingSubroutines();

	// Collect transient state variables
	m_transientStorage.collectVars(_contract, m_typeMapper);
	// Note: setTransientStorage called after m_exprBuilder is created (below)

	// Overloaded names: true overloads (same name, different params) only;
	// virtual overrides occupy the same slot and don't count.
	// Must be computed before translator creation so ctor uses correct names.
	m_overloadedNames.clear();
	// Function ids that a more-derived contract overrides — computed here for
	// overload naming, reused below to skip re-emitting overridden inherited
	// functions.
	std::set<int64_t> overriddenIds;
	{
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
	m_exprBuilder->viaIRSequencing = m_viaIR;

	// --evm-storage-layout: expose the solc-exact layout to expression builders
	// so state access lowers to slot addresses (EvmSlotLowering).
	if (evmStorageLayout())
	{
		m_evmLayout = std::make_unique<StorageLayout>();
		m_evmLayout->computeLayout(_contract, m_typeMapper);
		m_exprBuilder->evmSlotLayout = m_evmLayout.get();
	}

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

	// --evm-storage-layout: state lives in opaque numbered slots — no per-var
	// ARC-56 declarations (the reason the mode is opt-in; see the design doc).
	if (!evmStorageLayout())
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

			// A base function overridden by a more-derived version must NOT be
			// re-emitted: the derived override already occupies the same ABI
			// route. The name#id dedup key alone let it through (different id),
			// producing a duplicate ABI method (stale base body) that routed
			// on the same selector — safe only by MRO emission order.
			if (overriddenIds.count(func->id()))
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
