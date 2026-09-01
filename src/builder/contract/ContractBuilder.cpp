#include "builder/SourceLocConvert.h"
#include "builder/contract/RouterConditions.h"
#include "builder/ProgramAnalysis.h"
#include <variant>
#include "builder/contract/ContractBuilder.h"
#include "builder/contract/SelectorRouter.h"
#include "builder/contract/EvmMemoryCodec.h"
#include "awst/NameGen.h"
#include "awst/Visit.h"
#include "builder/NatSpecTags.h"
#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/itxn/FunctionPointerBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/storage/StorageLayout.h"
#include "builder/BuildArtifacts.h"
#include "Logger.h"
#include "builder/proxies/Erc1967Lowering.h"
#include "builder/proxies/UupsLowering.h"

#include <libsolidity/ast/ASTVisitor.h>

#include <boost/multiprecision/cpp_int.hpp>
#include <map>
#include <set>
// yul nodes BY VALUE (the AST aliases are std::variant, which needs
// complete types). Kept out of AssemblyBuilder.h so only the TUs that
// actually instantiate them pay the ~223k lines.
#include <libyul/AST.h>
#include <libyul/Dialect.h>

namespace puyasol::builder
{

/// --child-programs-via-box: the deployer streams each child approval program
/// into its "__cp_<Child>" box through this synthesized ABI method (creator-
/// gated; the name-prefix assert keeps it off storage boxes) before the
/// `new C()` site box_extracts the pages. One method serves every child.
static awst::ContractMethod makeProvisionChildProgMethod(
	TypeMapper& _typeMapper, std::string const& _cref,
	awst::SourceLocation const& _loc)
{
	awst::ContractMethod method;
	method.sourceLocation = _loc;
	method.cref = _cref;
	method.memberName = "__provisionChildProg";
	method.returnType = awst::WType::voidType();

	auto addArg = [&](char const* name, awst::WType const* wtype) {
		awst::SubroutineArgument arg;
		arg.name = name;
		arg.sourceLocation = _loc;
		arg.wtype = wtype;
		method.args.push_back(std::move(arg));
	};
	addArg("name", awst::WType::bytesType());
	addArg("total", awst::WType::uint64Type());
	addArg("offset", awst::WType::uint64Type());
	addArg("chunk", awst::WType::bytesType());

	awst::ARC4ABIMethodConfig config;
	config.name = "__provisionChildProg";
	config.sourceLocation = _loc;
	config.allowedCompletionTypes = {0}; // NoOp
	config.create = 3;                   // Disallow
	config.readonly = false;
	method.arc4MethodConfig = config;

	auto arg = [&](char const* name, awst::WType const* wtype) {
		return awst::makeVarExpression(name, wtype, _loc);
	};
	auto body = awst::makeBlock(_loc);
	{
		auto sender = awst::makeAsBytes(
			awst::makeTxn("Sender", awst::WType::accountType(), _loc), _loc);
		auto creator = awst::makeAsBytes(
			awst::makeGlobal(std::string("CreatorAddress"),
				awst::WType::accountType(), _loc), _loc);
		body->body.push_back(awst::makeExpressionStatement(
			awst::makeAssert(
				awst::makeBytesComparison(std::move(sender),
					awst::EqualityComparison::Eq, std::move(creator), _loc),
				_loc, "__provisionChildProg callable only by the app creator"),
			_loc));
	}
	body->body.push_back(awst::makeExpressionStatement(
		awst::makeAssert(
			awst::makeBytesComparison(
				awst::makeExtract3(arg("name", awst::WType::bytesType()),
					awst::makeIntegerConstant("0", _loc),
					awst::makeIntegerConstant("5", _loc), _loc),
				awst::EqualityComparison::Eq,
				awst::makeUtf8BytesConstant("__cp_", _loc), _loc),
			_loc, "__provisionChildProg targets child-program boxes only"),
		_loc));
	{
		auto* lenTupleT = _typeMapper.createType<awst::WTuple>(
			std::vector<awst::WType const*>{
				awst::WType::uint64Type(), awst::WType::boolType()});
		auto exists = awst::makeTupleItem(
			awst::makeBoxLen(arg("name", awst::WType::bytesType()),
				lenTupleT, _loc),
			1, awst::WType::boolType(), _loc);
		auto createBlock = awst::makeBlock(_loc);
		createBlock->body.push_back(awst::makeExpressionStatement(
			awst::makeBoxCreate(arg("name", awst::WType::bytesType()),
				arg("total", awst::WType::uint64Type()), _loc),
			_loc));
		body->body.push_back(awst::makeIfElse(
			awst::makeNot(std::move(exists), _loc), std::move(createBlock),
			nullptr, _loc));
	}
	body->body.push_back(awst::makeExpressionStatement(
		awst::makeBoxReplace(arg("name", awst::WType::bytesType()),
			arg("offset", awst::WType::uint64Type()),
			arg("chunk", awst::WType::bytesType()), _loc),
		_loc));
	method.body = std::move(body);
	return method;
}

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
	eb::FunctionPointerRegistry& _functionPointers,
	std::string const& _sourceFile,
	FunctionSymbolTable const& _functionSymbols,
	uint64_t _opupBudget,
	std::map<std::string, uint64_t> const& _ensureBudget,
	bool _viaIR,
	std::vector<solidity::frontend::FunctionDefinition const*> const& _hostBoundFunctions
)
	: m_typeMapper(_typeMapper),
	  m_storageMapper(_storageMapper),
	  m_functionPointers(_functionPointers),
	  m_sourceFile(_sourceFile),
	  m_functionSymbols(_functionSymbols),
	  m_opupBudget(_opupBudget),
	  m_ensureBudget(_ensureBudget),
	  m_viaIR(_viaIR),
	  m_hostBoundFunctions(_hostBoundFunctions)
{
}

// Free functions shared by AWSTBuilder (library/free-function path) and
// ContractBuilder (contract-method path).

awst::SourceLocation makeLoc(
	TypeMapper const& _typeMapper,
	std::string const& _sourceFile,
	solidity::langutil::SourceLocation const& _solLoc)
{
	return _typeMapper.sourceMap().toAwstLoc(_sourceFile, _solLoc);
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
	explicit AssemblyAggregateScanner(std::set<int64_t>& _ids)
		: ids(_ids)
	{}

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
				// A Solidity memory-reference read in Yul is always its numeric EVM
				// memory pointer. Blob-back every array shape, including bytes/string,
				// instead of trying to infer pointer semantics from the initializer or
				// from a few recognized expression shapes. That keeps `add(data, 32)`
				// correct for values produced by abi.encode*, function calls, casts,
				// parameters, and arbitrarily nested aggregates.
				// Whole aggregate assignment is pointer assignment too:
				// `result := store` repoints result's blob-offset local to store's
				// offset (AssemblyBuilder::emitPlainYulAssignment). Both operands
				// therefore stay in this same model; no pun-shape exception is needed.
				ids.insert(vd->id());
			}
			else if (dynamic_cast<solidity::frontend::StructType const*>(t))
				ids.insert(vd->id());
		}
		return true;
	}
};

/// Internal/private code outside solc's creation + deployed call graphs is not
/// part of the contract bytecode. Public/external methods are deliberately
/// retained even if an absent/malformed graph ever omits one: the ABI surface
/// is a stronger contract than this optimization.
bool isUnreachableInternalFunction(
	solidity::frontend::FunctionDefinition const& _fn,
	solidity::frontend::ContractDefinition const& _contract,
	ProgramAnalysis const& _analysis)
{
	if (!_analysis.hasContractReachability(_contract.id()))
		return false;
	// LogicSig entry selection happens after contract translation and is not an
	// EVM call-graph root. Keep an explicitly marked entry even when it is
	// internal/private so reachability pruning cannot remove the AVM program.
	for (auto const& modifier: _fn.modifiers())
	{
		auto const& path = modifier->name().path();
		if (!path.empty() && path.back() == "logicsig")
			return false;
	}
	using solidity::frontend::Visibility;
	if (_fn.visibility() != Visibility::Internal
		&& _fn.visibility() != Visibility::Private)
		return false;
	return !_analysis.isFunctionReachable(_contract.id(), _fn.id());
}
} // namespace

std::shared_ptr<awst::Expression> materializeBlobValue(
	TypeMapper& _typeMapper,
	solidity::frontend::Type const* _solType,
	awst::WType const* _wtype,
	std::string const& _offVar,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	return materializeEvmMemoryValue(
		_typeMapper, _solType, _wtype,
		awst::makeVarExpression(
			_offVar, awst::WType::uint64Type(), _loc),
		_loc, _out);
}

void emitAsmParamSpills(
	TypeMapper& _typeMapper,
	sol_ast::FunctionContext& _fn,
	solidity::frontend::Block const& _block,
	std::string const& _sourceFile,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	// collect the DECLS asm references (the aggregate scanner only keeps ids)
	struct DeclScan: solidity::frontend::ASTConstVisitor
	{
		std::map<int64_t, solidity::frontend::VariableDeclaration const*> decls;
		bool visit(solidity::frontend::InlineAssembly const& _asm) override
		{
			for (auto const& ref: _asm.annotation().externalReferences)
				if (auto const* vd = dynamic_cast<
						solidity::frontend::VariableDeclaration const*>(
						ref.second.declaration))
					decls[vd->id()] = vd;
			return true;
		}
	} scan;
	_block.accept(scan);
	for (auto const& [id, vd]: scan.decls)
	{
		// In default mode the scanner marks exactly the declarations whose Yul
		// references require pointer semantics. In universal-memory mode it marks
		// every referenced aggregate. Do not spill unrelated memory parameters.
		if (!_fn.isAssemblyAggregate(id))
			continue;
		if (!vd->isCallableOrCatchParameter()
			|| vd->referenceLocation()
				!= solidity::frontend::VariableDeclaration::Location::Memory
			|| vd->name().empty())
			continue;
		auto const* t = vd->type();
		bool aggregate = dynamic_cast<solidity::frontend::ArrayType const*>(t)
			|| dynamic_cast<solidity::frontend::StructType const*>(t);
		if (!aggregate)
			continue;
		if (!_fn.findBlobAggregate(id).empty())
			continue;   // already pointer-modeled (>4KB path)
		auto const* wt = _typeMapper.map(t);
		std::string offN = "__blobagg_off_" + std::to_string(id);
		awst::SourceLocation loc0 = makeLoc(_typeMapper, _sourceFile, vd->location());
		if (emitBlobBackValue(_typeMapper, t, wt,
				awst::makeVarExpression(vd->name(), wt, loc0),
				offN, static_cast<int>(id), loc0, _out))
			_fn.setBlobAggregate(id, offN);
	}
}

bool emitBlobBackValue(
	TypeMapper& typeMapper,
	solidity::frontend::Type const* declType,
	awst::WType const* wtype,
	std::shared_ptr<awst::Expression> value,
	std::string const& offVar,
	int uniqueId,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out)
{
	return spillEvmMemoryValue(typeMapper, declType, wtype, std::move(value),
		offVar, uniqueId, loc, out);
}

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
	sol_ast::FunctionContext& _ctx,
	solidity::frontend::Block const& _block,
	std::shared_ptr<awst::Block> _placeholder)
{
	auto& fn = _ctx;
	auto& exprBuilder = _ctx.tr.contractCtx;
	auto& typeMapper = _ctx.tr.typeMapper;
	auto const& sourceFile = _ctx.tr.sourceFile;
	auto fnGuard = exprBuilder.pushScopeRaii(&fn);
	auto blk = _placeholder
		? sol_ast::BlockContext::top(fn).withPlaceholder(_placeholder)
		: sol_ast::BlockContext::top(fn);
	auto blkGuard = exprBuilder.pushScopeRaii(&blk);

	// Mapping storage-ref params: `m[k]` resolves the dynamic box-key prefix at runtime.
	for (auto const* mp: _ctx.mappingKeyParams)
		if (mp && !mp->name().empty())
			fn.setMappingKeyParam(mp->id(), mp->name());

	// --evm-storage-layout: storage-ref params / named storage returns are
	// biguint slot handles — register so slot-handle machinery resolves them.
	for (auto const* sp: _ctx.slotRefParams)
		if (sp && !sp->name().empty())
			fn.setSlotStorageRef(sp->id(), awst::makeVarExpression(
				sp->name(), awst::WType::biguintType(), awst::SourceLocation{}));

	// Offset-convention struct-ref params (handle-model dual handle): register the companion
	// uint64 offset var so the body's `s.field` writes hit the element slice via
	// box_replace(key, offset+fieldOff). The offset param itself is in the subroutine signature
	// (FunctionBuilder) and supplied by the caller (SolInternalCall).
	for (auto const* mp: _ctx.mappingKeyParams)
		if (mp && !mp->name().empty()
			&& typeMapper.analysis().structRefOffsetParams.count(mp->id()))
			fn.setStructRefOffset(mp->id(), mp->name() + "__off");

	// Named returns >4 KB: blob-backed aggregates (pointer model) so `p.field[i]`
	// lowers to multi-slot blob word access. Base offset assigned + FMP bumped in
	// FunctionBuilder.
	for (auto const* rp: _ctx.namedReturns)
	{
		if (!rp || rp->name().empty()
			|| rp->referenceLocation() != solidity::frontend::VariableDeclaration::Location::Memory)
			continue;
		auto const* rpType = typeMapper.map(rp->type());
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

	// --evm-memory-layout: MEMORY PARAMS the assembly treats as pointers
	// (`keccak256(s, 32)` on a `string memory s` param) — spill the incoming
	// VALUE into a blob region at function entry and register the param as
	// blob-backed, so asm gets a real offset and value uses read it back.
	std::vector<std::shared_ptr<awst::Statement>> paramSpills;
	if (!_placeholder)
		emitAsmParamSpills(typeMapper, fn, _block, sourceFile, paramSpills);

	auto body = sol_ast::buildBlock(blk, _block);
	if (!paramSpills.empty())
		body->body.insert(body->body.begin(),
			std::make_move_iterator(paramSpills.begin()),
			std::make_move_iterator(paramSpills.end()));
	return body;
}

// ContractBuilder wrappers — route through free-function API.

awst::SourceLocation ContractBuilder::makeLoc(
	solidity::langutil::SourceLocation const& _solLoc
)
{
	return ::puyasol::builder::makeLoc(m_typeMapper, m_sourceFile, _solLoc);
}

std::shared_ptr<awst::Block> ContractBuilder::buildBlock(
	solidity::frontend::Block const& _block)
{
	auto& ctx = m_functionCtx.value();
	return ::puyasol::builder::buildBlock(ctx, _block, ctx.placeholder);
}

void ContractBuilder::setFunctionContext(
	std::vector<std::pair<std::string, awst::WType const*>> const& _params,
	awst::WType const* _returnType,
	std::map<std::string, unsigned> const& _bitWidths,
	std::map<std::string, solidity::frontend::Type const*> const& _paramSolTypes)
{
	auto& ctx = m_functionCtx.value();
	ctx.params = _params;
	ctx.returnType = _returnType;
	ctx.paramBitWidths = _bitWidths;
	ctx.paramSolTypes = _paramSolTypes;
	ctx.returnSolTypes.clear();
	ctx.namedReturns.clear();
	ctx.mappingKeyParams.clear();
	ctx.boxKeyStructParams.clear();
	ctx.blobAggParams.clear();
	ctx.placeholder.reset();
	ctx.inConstructor = false;
	ctx.frameIsProgram = false;
	ctx.encodeReturnsAtBuildTime = false;
	ctx.returnAsmWrap = false;
	ctx.returnWirePlan.clear();
	ctx.seededCalldataPointers.clear();
	ctx.slotRefParams.clear();
}

void ContractBuilder::setPlaceholderBody(std::shared_ptr<awst::Block> _body)
{
	m_functionCtx->placeholder = std::move(_body);
}

void ContractBuilder::prependNonPayableCheck(awst::ContractMethod& _method,
	std::string const& _arc4Selector)
{
	// Only ARC4-dispatched methods are externally callable.
	if (!_method.arc4MethodConfig.has_value())
		return;
	if (!_method.body)
		return;

	auto loc = _method.sourceLocation;

	// msg.value shape — avoids GroupIndex-1 when GroupIndex==0 (underflow-safe).
	auto msgValue = makeMsgValueAmount(loc);

	auto isZero = awst::makeNumericCompare(
		std::move(msgValue), awst::NumericComparison::Eq,
		awst::makeIntegerConstant("0", loc), loc);

	auto assertStmt = awst::makeExpressionStatement(
		awst::makeAssert(std::move(isZero), loc, "not payable"), loc);

	// Gate on the ROUTER having dispatched THIS method. The guard reads a
	// TRANSACTION-level fact (the preceding payment), but it lives in the
	// method BODY — which an internal `callsub` from another method shares. So
	// a PAYABLE function that internally calls a non-payable public one
	// re-evaluated this against the same group and reverted on its own,
	// legitimate payment: friend.tech's payable `buyShares` calls
	// `getPrice(uint256,uint256)`, and every buy with value died on
	// `assert // not payable` inside getPrice. Extremely common shape
	// (buy/sell calling a public price view), invisible until msg.value
	// actually started flowing.
	//
	// ApplicationArgs[0] carries the dispatched method's selector, so it tells
	// entry-from-router apart from entry-from-callsub. Without a selector to
	// compare (empty), keep the unconditional guard — same behaviour as before.
	if (!_arc4Selector.empty())
	{
		auto numArgs = awst::makeTxn(
			std::string("NumAppArgs"), awst::WType::uint64Type(), loc);
		auto hasArgs = awst::makeNumericCompare(
			std::move(numArgs), awst::NumericComparison::Gt,
			awst::makeIntegerConstant("0", loc), loc);
		auto selMatches = awst::makeBytesComparison(
			awst::makeAppArg(0, loc),
			awst::EqualityComparison::Eq,
			awst::makeMethodConstant(_arc4Selector, awst::WType::bytesType(), loc),
			loc);
		auto dispatched = awst::makeBoolBinOp(
			std::move(hasArgs), awst::BinaryBooleanOperator::And,
			std::move(selMatches), loc);
		auto thenBlock = awst::makeBlock(loc);
		thenBlock->body.push_back(std::move(assertStmt));
		_method.body->body.insert(
			_method.body->body.begin(),
			awst::makeIfElse(std::move(dispatched), std::move(thenBlock), nullptr, loc));
		return;
	}
	_method.body->body.insert(_method.body->body.begin(), std::move(assertStmt));
}

std::shared_ptr<awst::Contract> ContractBuilder::build(
	solidity::frontend::ContractDefinition const& _contract,
	StorageRuntimePlan const& _storagePlan,
	bool _emitEvmStorageRuntime
)
{
	m_currentContract = &_contract;
	m_boxArrayVars.clear();
	std::string contractName = _contract.name();
	std::string contractId = m_sourceFile + "." + contractName;

	// Reset the generated-name counters: a contract's temp/subroutine names
	// (`__mod_retval_N`, `f__mod0_N`, …) must depend only on its own content,
	// not on how many contracts compiled before it in the batch (deterministic
	// multi-contract output; prerequisite for parallel per-contract compiles).
	awst::NameGen::resetAll();

	// Reset Yul subroutine sink (drained below).
	m_typeMapper.artifacts().pendingYulSubroutines.clear();

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
		m_overloadedNames, m_functionSymbols,
		m_functionPointers
	);
	m_exprBuilder->currentContract = &_contract;
	m_exprBuilder->viaIRSequencing = m_viaIR;

	// One session-owned layout feeds state access, inline-assembly slot routing,
	// and runtime-dispatch generation. It is always solc's exact logical layout;
	// the selected backend binds declarations to physical AVM cells separately.
	m_exprBuilder->storageLayout = &_storagePlan.solidityLayout;

	// Pre-populate host-bound function map before translation so the call
	// resolver routes them as InstanceMethodTargets.
	for (auto const* function: m_hostBoundFunctions)
	{
		if (!function) continue;
		auto const* scope = function->annotation().contract;
		std::string methodName = "__hostfn_"
			+ (scope ? scope->name() : std::string("free"))
			+ "_" + function->name() + "_" + std::to_string(function->id());
		m_exprBuilder->internalizedFunctionNames[function->id()] = methodName;
	}

	// In-place emplace — TranslationContext caches a pointer to its own scopeState_;
	// copy/move construction would dangle that pointer.
	m_tr.emplace(*m_exprBuilder, m_typeMapper, m_sourceFile);
	m_exprBuilder->currentScope = &*m_tr;
	m_functionCtx.emplace(*m_tr,
		std::vector<std::pair<std::string, awst::WType const*>>{},
		nullptr, std::map<std::string, unsigned>{});
	m_functionCtx->currentContract = &_contract;

	m_exprBuilder->transientStorage =
		m_transientStorage.hasTransientVars() ? &m_transientStorage : nullptr;
	// StorageBackend is per-contract (TransientStorage is per-contract).
	m_storageBackend.emplace(m_storageMapper, m_exprBuilder->transientStorage);
	m_exprBuilder->storageBackend = &*m_storageBackend;

	eb::FunctionPointerBuilder::setCurrentCref(*m_exprBuilder, contractId);

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
	if (!m_typeMapper.profile().evmStorageLayout)
		contract->appState = m_storageMapper.mapStateVariables(_contract, m_sourceFile);

	// EVM-memory scratch slots (default 0-4; raisable via
	// --evm-memory-slots) plus transient + flash-accounting slots.
	contract->reservedScratchSpace = m_typeMapper.profile().scratchLayout.reservedSlots();

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

		if (isUnreachableInternalFunction(
				*func, _contract, m_typeMapper.analysis()))
		{
			Logger::instance().debug(
				"skipping unreachable internal/private function `"
				+ func->name() + "` (absent from solc's call graphs)",
				makeLoc(func->location()));
			continue;
		}
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
			if (isUnreachableInternalFunction(
					*func, _contract, m_typeMapper.analysis()))
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

	// --child-programs-via-box: this contract's bodies emitted box-loading
	// `new C()` creates — append the deployer's provisioning method BEFORE
	// dispatch so the residual ARC4 router (or plain ARC4 router) sees it.
	// Snapshot-and-reset: the set is per-contract, like usesErc1967Admin.
	if (!m_typeMapper.artifacts().boxProvisionedChildren.empty())
	{
		m_typeMapper.artifacts().boxProvisionedChildren.clear();
		contract->methods.push_back(makeProvisionChildProgMethod(
			m_typeMapper, contract->id,
			contract->approvalProgram.sourceLocation));
	}

	if (m_typeMapper.profile().contractAbi == ContractAbi::Evm)
		emitEvmEntryDispatch(_contract, *contract);
	else
	{
		// EVM compat arms FIRST (each self-guards on the [selector, body]
		// carrier shape), then the untouched ARC-4 dispatch — including its
		// exit-early `return ARC4Router()` form for fallback-less contracts,
		// which errs internally on unknown selectors and therefore must come
		// last.
		emitEvmCompatRoutes(_contract, *contract);
		auto const* fallbackFunc = _contract.fallbackFunction();
		auto const* receiveFunc = _contract.receiveFunction();
		if (fallbackFunc && !fallbackFunc->isImplemented())
			fallbackFunc = nullptr;
		if (receiveFunc && !receiveFunc->isImplemented())
			receiveFunc = nullptr;
		if (contract->approvalProgram.body)
			emitSelectorDispatch(
				*contract->approvalProgram.body, fallbackFunc, receiveFunc,
				contract->approvalProgram.sourceLocation);
	}

	// Emit MRO / fallback / explicit-base super subroutines now that all
	// regular method bodies are translated.
	emitSuperSubroutines(*contract, contractName);

	// Emit functions whose lowering requires a concrete contract host. This
	// includes function-pointer dispatch and default-layout storage assembly.
	for (auto const* function: m_hostBoundFunctions)
	{
		if (!function || !function->isImplemented()) continue;
		auto nameIt = m_exprBuilder->internalizedFunctionNames.find(function->id());
		if (nameIt == m_exprBuilder->internalizedFunctionNames.end()) continue;
		clearSuperOverrides();
		// EVERY host-bound function is internalized into EVERY contract, used
		// or not — mark the body so EIP-1967 admin-slot uses record under the
		// function's id and attach via the call graph, not to this contract
		// unconditionally (BuildArtifacts::noteErc1967AdminUse).
		m_typeMapper.artifacts().currentFreestandingFunctionId = function->id();
		auto method = buildFunction(
			*function, contractName, nameIt->second, /*asInternalCopy=*/true);
		m_typeMapper.artifacts().currentFreestandingFunctionId = -1;
		contract->methods.push_back(std::move(method));
		for (auto& sub: m_modifierSubroutines)
			contract->methods.push_back(std::move(sub));
		m_modifierSubroutines.clear();
	}

	// Generate __storage_read/__storage_write dispatch subroutines
	// for assembly sload/sstore support
	// EVM-layout runtime helpers have unit-global SubroutineIDs and bodies that
	// are specialized from unit-global profile flags. Emit them once for the
	// whole unit; generating a copy per concrete contract inflated multi-contract
	// AWST by hundreds of kilobytes and made duplicate-ID resolution ambiguous.
	// Default-layout dispatch remains contract-specific and is always emitted.
	if (_emitEvmStorageRuntime
		|| (!m_typeMapper.profile().evmStorageLayout && _storagePlan.needsDispatch()))
		buildStorageDispatch(_storagePlan, contract.get(), contractName);

	// Generate function pointer dispatch tables
	{
		// Set subroutine IDs for library/free function targets so dispatch
		// uses SubroutineID (resolvable by puya) instead of InstanceMethodTarget.
		eb::FunctionPointerBuilder::setSubroutineIds(
			*m_exprBuilder, m_functionSymbols);

		std::string cref = m_sourceFile + "." + contractName;
		awst::SourceLocation loc;
		loc.file = m_sourceFile;
		auto& dispCtx = *m_exprBuilder;
		auto dispatchMethods = eb::FunctionPointerBuilder::generateDispatchMethods(
			dispCtx, cref, loc, &m_dispatchSubroutines, &contract->methods);
		for (auto& m : dispatchMethods)
			contract->methods.push_back(std::move(m));
		eb::FunctionPointerBuilder::reset(*m_exprBuilder);
	}

	// Drain any Subroutines emitted for recursive Yul functions so the
	// contract-builder caller picks them up alongside fn-ptr dispatchers.
	{
		auto yulSubs = std::move(m_typeMapper.artifacts().pendingYulSubroutines);
		m_typeMapper.artifacts().pendingYulSubroutines.clear();
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

	// Default-layout dispatch bodies are contract-specific because they route
	// logical slots to this contract's named AVM cells. Scope every generated
	// call to the same contract-specific root ID. EVM-layout runtime helpers are
	// compilation-unit singletons and retain their stable global IDs.
	if (!m_typeMapper.profile().evmStorageLayout && _storagePlan.needsDispatch())
	{
		auto const scopeStorageCall = [&](awst::Expression& expression) {
			auto* call = dynamic_cast<awst::SubroutineCallExpression*>(&expression);
			if (!call)
				return;
			auto* id = std::get_if<awst::SubroutineID>(&call->target);
			if (!id)
				return;
			if (id->target == "__puyasol___storage_read")
				id->target = contractId + ".__storage_read";
			else if (id->target == "__puyasol___storage_write")
				id->target = contractId + ".__storage_write";
		};
		awst::visitExpressions(contract->approvalProgram, scopeStorageCall);
		awst::visitExpressions(contract->clearProgram, scopeStorageCall);
		for (auto& method: contract->methods)
			awst::visitExpressions(method, scopeStorageCall);
		for (auto& subroutine: m_dispatchSubroutines)
			if (subroutine && subroutine->body)
				awst::visitExpressions(*subroutine->body, scopeStorageCall);
	}

	// EIP-1967 (proxy.md §1): if any admin-slot use was lowered while
	// translating THIS contract's bodies — or inside a freestanding library/
	// free function THIS contract's call graph reaches (OZ's ERC1967Utils is a
	// library, translated before any contract) — synthesize the admin global
	// and the UpdateApplication method gating native updates on it. Snapshot-
	// and-reset the direct flag so one contract's proxy machinery never leaks
	// into the next unit member. Placed after ALL method translation (ordinary
	// externals build in the loops above, not in buildApprovalProgram).
	// A 1967 slot constant SURVIVING translation means it escaped into
	// runtime data flow (classify consumes direct sload/sstore uses; the
	// let-fold emits no store) — the OZ StorageSlot shape. Warn: storage
	// through a derived slot value splits from the native proxy model.
	{
		std::set<proxies::Erc1967Slot> warned;
		proxies::Erc1967Lowering::warnEscapedSlotConstants(
			contract->approvalProgram, warned);
		proxies::Erc1967Lowering::warnEscapedSlotConstants(
			contract->clearProgram, warned);
		for (auto const& method: contract->methods)
			proxies::Erc1967Lowering::warnEscapedSlotConstants(method, warned);
	}

	bool usesErc1967Admin = m_typeMapper.artifacts().usesErc1967Admin;
	m_typeMapper.artifacts().usesErc1967Admin = false;
	if (!usesErc1967Admin)
		for (int64_t functionId: m_typeMapper.artifacts().erc1967AdminFunctions)
			if (m_typeMapper.analysis().isFunctionReachable(_contract.id(), functionId))
			{
				usesErc1967Admin = true;
				break;
			}
	if (usesErc1967Admin)
	{
		auto loc = contract->approvalProgram.sourceLocation;
		contract->appState.push_back(
			proxies::Erc1967Lowering::adminStateDefinition(loc));
		contract->methods.push_back(
			proxies::Erc1967Lowering::updateGateMethod(contract->id, loc));
	}

	// UUPS (proxy.md §3): a concrete contract inheriting OZ UUPSUpgradeable
	// with an implemented _authorizeUpgrade gets the native update gate —
	// the hook's translated method (modifiers inlined) is the permission
	// check, run inside the UpdateApplication txn.
	if (proxies::UupsLowering::isUupsImplementation(_contract))
	{
		// A modifier'd hook builds as a chain: `_authorizeUpgrade__mod0_<n>`
		// is the entry that runs the modifiers (onlyOwner and friends) before
		// the body — prefer it over the bare-body name.
		awst::ContractMethod const* hook = nullptr;
		for (auto const& method: contract->methods)
		{
			if (method.memberName == "_authorizeUpgrade")
				hook = &method;
			if (method.memberName.rfind("_authorizeUpgrade__mod0", 0) == 0)
			{
				hook = &method;
				break;
			}
		}
		if (hook)
			contract->methods.push_back(proxies::UupsLowering::updateGateMethod(
				contract->id, *hook, contract->approvalProgram.sourceLocation));
	}

	return contract;
}



} // namespace puyasol::builder
