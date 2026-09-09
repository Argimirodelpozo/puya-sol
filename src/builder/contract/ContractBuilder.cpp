#include "builder/SourceLocConvert.h"
#include "builder/contract/RouterConditions.h"
#include "builder/ProgramAnalysis.h"
#include <variant>
#include "builder/contract/ContractBuilder.h"
#include "builder/contract/SelectorRouter.h"
#include "builder/contract/EvmMemoryCodec.h"
#include "awst/NameGen.h"
#include "awst/Visit.h"
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
	auto method = awst::ContractMethod(_cref, "__provisionChildProg",
		awst::WType::voidType(),
		{{"name", awst::WType::bytesType(), _loc},
			{"total", awst::WType::uint64Type(), _loc},
			{"offset", awst::WType::uint64Type(), _loc},
			{"chunk", awst::WType::bytesType(), _loc}},
		_loc);

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
	auto body = method.body;
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
		if (!_fn.scope.bindings.assemblyAggregates.contains(id))
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
		if (!_fn.scope.bindings.blobAggregates.get(id).empty())
			continue;   // already pointer-modeled (>4KB path)
		auto const* wt = _typeMapper.map(t);
		std::string offN = "__blobagg_off_" + std::to_string(id);
		awst::SourceLocation loc0 = makeLoc(_typeMapper, _sourceFile, vd->location());
		if (emitBlobBackValue(_typeMapper, t, wt,
				awst::makeVarExpression(vd->name(), wt, loc0),
				offN, static_cast<int>(id), loc0, _out))
			_fn.scope.bindings.blobAggregates.set(id, offN);
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
		_fn.scope.bindings.assemblyAggregates.insert(id);
}

std::shared_ptr<awst::Block> buildBlock(
	sol_ast::FunctionContext& _ctx,
	solidity::frontend::Block const& _block,
	sol_ast::PlaceholderFactory _placeholder)
{
	auto& fn = _ctx;
	auto& exprBuilder = _ctx.tr.contractCtx;
	auto& typeMapper = _ctx.tr.typeMapper;
	auto const& sourceFile = _ctx.tr.sourceFile;
	auto fnGuard = exprBuilder.pushScopeRaii(&fn.scope);
	sol_ast::BlockContext blk{fn, nullptr, _placeholder};
	auto blkGuard = exprBuilder.pushScopeRaii(&blk.scope);

	// Mapping storage-ref params: `m[k]` resolves the dynamic box-key prefix at runtime.
	for (auto const* mp: _ctx.mappingKeyParams)
		if (mp && !mp->name().empty())
			fn.scope.bindings.mappingKeyParams.set(mp->id(), mp->name());

	// --evm-storage-layout: storage-ref params / named storage returns are
	// biguint slot handles — register so slot-handle machinery resolves them.
	for (auto const* sp: _ctx.slotRefParams)
		if (sp && !sp->name().empty())
			fn.scope.bindings.slotStorageRefs.set(sp->id(), awst::makeVarExpression(
				sp->name(), awst::WType::biguintType(), awst::SourceLocation{}));

	// Offset-convention struct-ref params (handle-model dual handle): register the companion
	// uint64 offset var so the body's `s.field` writes hit the element slice via
	// box_replace(key, offset+fieldOff). The offset param itself is in the subroutine signature
	// (FunctionBuilder) and supplied by the caller (SolInternalCall).
	for (auto const* mp: _ctx.mappingKeyParams)
		if (mp && !mp->name().empty()
			&& typeMapper.analysis().structRefOffsetParams.count(mp->id()))
			fn.scope.bindings.structRefOffsets.set(mp->id(), mp->name() + "__off");

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
			fn.scope.bindings.blobAggregates.set(rp->id(), "__blobagg_off_" + std::to_string(rp->id()));
	}

	// Blob-agg params >4 KB: param's local IS the uint64 base offset (caller passed
	// it — see SolInternalCall/SolIdentifier); no FMP bump needed.
	for (auto const* p: _ctx.blobAggParams)
		if (p && !p->name().empty())
			fn.scope.bindings.blobAggregates.set(p->id(), p->name());

	// Promote memory aggregates used as values in inline assembly to blob-backed
	// (Yul memory pointer). Must mark before body translation so SolVariableDeclaration
	// blob-backs them at their declaration. Not run during modifier re-entrancy
	// (_placeholder set) — pre-built placeholder contexts are unsafe to re-walk.
	if (!_placeholder)
		markAssemblyAggregates(fn, _block);

	// EVM blob memory: MEMORY PARAMS the assembly treats as pointers
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
	solidity::frontend::Block const& _block,
	sol_ast::PlaceholderFactory _placeholder)
{
	return ::puyasol::builder::buildBlock(*m_functionCtx, _block, std::move(_placeholder));
}

void ContractBuilder::setFunctionContext(
	std::vector<std::pair<std::string, awst::WType const*>> const& _params,
	awst::WType const* _returnType,
	std::map<std::string, unsigned> const& _bitWidths,
	std::map<std::string, solidity::frontend::Type const*> const& _paramSolTypes)
{
	auto& ctx = m_functionCtx.emplace(*m_tr, _params, _returnType, _bitWidths);
	ctx.paramSolTypes = _paramSolTypes;
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

std::string ContractBuilder::beginContract(
	solidity::frontend::ContractDefinition const& _contract,
	StorageRuntimePlan const& _storagePlan)
{
	m_currentContract = &_contract;
	m_storageMapper.beginContract(_storagePlan.solidityLayout, m_sourceFile);
	m_boxArrayVars.clear();
	std::string contractName = _contract.name();
	// solc's CompilerStack::filesystemFriendlyName rule: a contract whose name
	// collides with another compiled contract is emitted under its fully
	// qualified name (path and name joined by `_`); unique names stay plain.
	if (auto it = m_artifactNames.find(_contract.fullyQualifiedName());
		it != m_artifactNames.end())
		contractName = it->second;
	m_contractId = _contract.fullyQualifiedName();

	// Reset the generated-name counters: a contract's temp/subroutine names
	// (`__mod_retval_N`, `f__mod0_N`, …) must depend only on its own content,
	// not on how many contracts compiled before it in the batch (deterministic
	// multi-contract output; prerequisite for parallel per-contract compiles).
	awst::NameGen::resetAll();

	// Reset Yul subroutine sink (drained by emitFunctionPointerDispatch).
	m_typeMapper.artifacts().pendingYulSubroutines.clear();

	// Collect transient state variables
	m_transientStorage.collectVars(_contract, m_typeMapper);
	// Note: setTransientStorage called after m_exprBuilder is created (createFunctionContexts)
	return contractName;
}

std::set<int64_t> ContractBuilder::collectOverloadedNames(
	solidity::frontend::ContractDefinition const& _contract)
{
	// Overloaded names: true overloads (same name, different params) only;
	// virtual overrides occupy the same slot and don't count.
	// Must be computed before translator creation so ctor uses correct names.
	m_overloadedNames.clear();
	// Function ids that a more-derived contract overrides — computed here for
	// overload naming, reused by buildInheritedFunctions to skip re-emitting
	// overridden inherited functions.
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
	return overriddenIds;
}

void ContractBuilder::createExpressionBuilder(
	solidity::frontend::ContractDefinition const& _contract,
	StorageRuntimePlan const& _storagePlan,
	std::string const& _contractName)
{
	m_exprBuilder = std::make_unique<eb::ContractContext>(
		m_typeMapper, m_storageMapper, m_sourceFile, _contractName,
		m_overloadedNames, m_functionSymbols,
		m_functionPointers
	);
	m_exprBuilder->currentContract = &_contract;
	m_exprBuilder->viaIRSequencing = m_viaIR;

	// One session-owned layout feeds state access, inline-assembly slot routing,
	// and runtime-dispatch generation. It is always solc's exact logical layout;
	// the selected backend binds declarations to physical AVM cells separately.
	m_exprBuilder->storageLayout = &_storagePlan.solidityLayout;
}

std::vector<solidity::frontend::FunctionDefinition const*>
ContractBuilder::collectReachableHostBoundFunctions(
	solidity::frontend::ContractDefinition const& _contract) const
{
	// A host-bound free/library function only belongs in contracts whose solc
	// call graph can reach it.  The unit-global list is deliberately
	// conservative (it also closes over root callers), but copying that whole
	// list into every concrete contract both bloats multi-contract output and
	// can leave unrelated contracts calling runtime helpers they do not own.
	// If solc did not provide a graph, retain the conservative fallback.
	std::vector<solidity::frontend::FunctionDefinition const*>
		reachableHostBoundFunctions;
	for (auto const* function: m_hostBoundFunctions)
		if (function
			&& (!m_typeMapper.analysis().hasContractReachability(_contract.id())
				|| m_typeMapper.analysis().isFunctionReachable(
					_contract.id(), function->id())))
			reachableHostBoundFunctions.push_back(function);
	return reachableHostBoundFunctions;
}

void ContractBuilder::registerHostBoundFunctionNames(
	std::vector<solidity::frontend::FunctionDefinition const*> const& _functions)
{
	// Pre-populate host-bound function map before translation so the call
	// resolver routes them as InstanceMethodTargets.
	for (auto const* function: _functions)
	{
		if (!function) continue;
		auto const* scope = function->annotation().contract;
		std::string methodName = "__hostfn_"
			+ (scope ? scope->name() : std::string("free"))
			+ "_" + function->name() + "_" + std::to_string(function->id());
		m_exprBuilder->internalizedFunctionNames[function->id()] = methodName;
	}
}

void ContractBuilder::createFunctionContexts()
{
	m_tr.emplace(*m_exprBuilder, m_typeMapper, m_sourceFile);
	m_exprBuilder->currentScope = &m_tr->scope;
	m_functionCtx.emplace(*m_tr,
		std::vector<std::pair<std::string, awst::WType const*>>{},
		nullptr, std::map<std::string, unsigned>{});

	m_exprBuilder->transientStorage =
		m_transientStorage.hasTransientVars() ? &m_transientStorage : nullptr;
	// StorageBackend is per-contract (TransientStorage is per-contract).
	m_storageBackend.emplace(m_storageMapper, m_exprBuilder->transientStorage);
	m_exprBuilder->storageBackend = &*m_storageBackend;

	eb::FunctionPointerBuilder::setCurrentCref(*m_exprBuilder, m_contractId);
}

std::shared_ptr<awst::Contract> ContractBuilder::makeContractNode(
	solidity::frontend::ContractDefinition const& _contract,
	std::string const& _contractName)
{
	auto contract = std::make_shared<awst::Contract>();
	contract->sourceLocation = makeLoc(_contract.location());
	contract->id = m_contractId;
	contract->name = _contractName;

	if (_contract.documentation())
	{
		std::string const& doc = *_contract.documentation()->text();
		contract->description = doc;
	}

	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		if (base != &_contract)
			contract->methodResolutionOrder.push_back(
				base->fullyQualifiedName()
			);
	}

	// --evm-storage-layout: state lives in opaque numbered slots — no per-var
	// ARC-56 declarations (the reason the mode is opt-in; see the design doc).
	if (!m_typeMapper.profile().evmStorageLayout)
		contract->appState = m_storageMapper.mapStateVariables(_contract, m_sourceFile);

	// EVM-memory scratch slots (default 0-4; raisable via
	// --evm-memory-slots) plus transient + flash-accounting slots.
	contract->reservedScratchSpace = m_typeMapper.profile().scratchLayout.reservedSlots();
	if (m_transientStorage.addressShadowSize())
		contract->reservedScratchSpace.push_back(m_transientStorage.addressShadowSlot());
	return contract;
}

void ContractBuilder::buildPrograms(
	solidity::frontend::ContractDefinition const& _contract,
	std::string const& _contractName,
	awst::Contract& _contractNode)
{
	// Approval and clear programs
	m_postInitMethod.reset();
	_contractNode.approvalProgram = buildApprovalProgram(_contract, _contractName);
	_contractNode.clearProgram = buildClearProgram(_contract, _contractName);

	if (m_postInitMethod)
	{
		awst::AppStorageDefinition ctorPendingState;
		ctorPendingState.memberName = "__ctor_pending";
		ctorPendingState.sourceLocation = _contractNode.approvalProgram.sourceLocation;
		ctorPendingState.storageKind = awst::AppStorageKind::AppGlobal;
		ctorPendingState.storageWType = awst::WType::uint64Type();
		ctorPendingState.key = awst::makeUtf8BytesConstant(
			"__ctor_pending", ctorPendingState.sourceLocation);
		_contractNode.appState.push_back(std::move(ctorPendingState));

		_contractNode.methods.push_back(std::move(*m_postInitMethod));
		m_postInitMethod.reset();
	}
	for (auto& constructorSubroutine: m_modifierSubroutines)
		_contractNode.methods.push_back(std::move(constructorSubroutine));
	m_modifierSubroutines.clear();
}

std::string ContractBuilder::translationKey(
	solidity::frontend::FunctionDefinition const& _func) const
{
	std::string key = _func.name();
	if (m_overloadedNames.count(key))
		key += "#" + std::to_string(_func.id());
	return key;
}

void ContractBuilder::appendMethodWithModifierSubs(
	awst::Contract& _contractNode, awst::ContractMethod _method)
{
	_contractNode.methods.push_back(std::move(_method));
	for (auto& sub: m_modifierSubroutines)
		_contractNode.methods.push_back(std::move(sub));
	m_modifierSubroutines.clear();
}

/// fallback/receive have empty Solidity names; give explicit memberName.
static std::string specialMemberName(
	solidity::frontend::FunctionDefinition const& _func)
{
	if (_func.isFallback())
		return "__fallback";
	if (_func.isReceive())
		return "__receive";
	return {};
}

void ContractBuilder::buildDefinedFunctions(
	solidity::frontend::ContractDefinition const& _contract,
	std::string const& _contractName,
	awst::Contract& _contractNode,
	std::set<std::string>& _translatedFunctions)
{
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
		_translatedFunctions.insert(translationKey(*func));
		appendMethodWithModifierSubs(_contractNode,
			buildFunction(*func, _contractName, specialMemberName(*func)));
	}
}

void ContractBuilder::buildInheritedFunctions(
	solidity::frontend::ContractDefinition const& _contract,
	std::string const& _contractName,
	awst::Contract& _contractNode,
	std::set<int64_t> const& _overriddenIds,
	std::set<std::string>& _translatedFunctions)
{
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		if (base == &_contract)
			continue; // Already handled by buildDefinedFunctions

		for (auto const* func: base->definedFunctions())
		{
			if (func->isConstructor())
				continue;

			// A base function overridden by a more-derived version must NOT be
			// re-emitted: the derived override already occupies the same ABI
			// route. The name#id dedup key alone let it through (different id),
			// producing a duplicate ABI method (stale base body) that routed
			// on the same selector — safe only by MRO emission order.
			if (_overriddenIds.count(func->id()))
				continue;

			std::string key = translationKey(*func);
			if (_translatedFunctions.count(key))
				continue;

			if (!func->isImplemented())
				continue;
			if (isUnreachableInternalFunction(
					*func, _contract, m_typeMapper.analysis()))
				continue;

			_translatedFunctions.insert(key);
			appendMethodWithModifierSubs(_contractNode,
				buildFunction(*func, _contractName, specialMemberName(*func)));
		}
	}
}

void ContractBuilder::buildRouters(
	solidity::frontend::ContractDefinition const& _contract,
	awst::Contract& _contractNode)
{
	// --child-programs-via-box: this contract's bodies emitted box-loading
	// `new C()` creates — append the deployer's provisioning method BEFORE
	// dispatch so the residual ARC4 router (or plain ARC4 router) sees it.
	// Snapshot-and-reset: the set is per-contract, like usesErc1967Admin.
	if (!m_typeMapper.artifacts().boxProvisionedChildren.empty())
	{
		m_typeMapper.artifacts().boxProvisionedChildren.clear();
		_contractNode.methods.push_back(makeProvisionChildProgMethod(
			m_typeMapper, _contractNode.id,
			_contractNode.approvalProgram.sourceLocation));
	}

	if (m_typeMapper.profile().contractAbi == ContractAbi::Evm)
		emitEvmEntryDispatch(_contract, _contractNode);
	else
	{
		// EVM compat arms FIRST (each self-guards on the [selector, body]
		// carrier shape), then the untouched ARC-4 dispatch — including its
		// exit-early `return ARC4Router()` form for fallback-less contracts,
		// which errs internally on unknown selectors and therefore must come
		// last.
		emitEvmCompatRoutes(_contract, _contractNode);
		auto const* fallbackFunc = _contract.fallbackFunction();
		auto const* receiveFunc = _contract.receiveFunction();
		if (fallbackFunc && !fallbackFunc->isImplemented())
			fallbackFunc = nullptr;
		if (receiveFunc && !receiveFunc->isImplemented())
			receiveFunc = nullptr;
		if (_contractNode.approvalProgram.body)
			emitSelectorDispatch(
				*_contractNode.approvalProgram.body, fallbackFunc, receiveFunc,
				_contractNode.approvalProgram.sourceLocation);
	}

	// Router-memoized struct decoders (EvmAbiDecode): the arms referenced
	// them by name while dispatch was built; append the bodies now.
	{
		auto& arts = m_typeMapper.artifacts();
		for (auto& method: arts.pendingEvmDecodeMethods)
		{
			method.cref = _contractNode.id;
			_contractNode.methods.push_back(std::move(method));
		}
		arts.pendingEvmDecodeMethods.clear();
		arts.evmDecodeStructMethods.clear();
	}
}

void ContractBuilder::buildHostBoundFunctions(
	std::string const& _contractName,
	awst::Contract& _contractNode,
	std::vector<solidity::frontend::FunctionDefinition const*> const& _functions)
{
	// Emit functions whose lowering requires a concrete contract host. This
	// includes function-pointer dispatch and default-layout storage assembly.
	for (auto const* function: _functions)
	{
		if (!function || !function->isImplemented()) continue;
		auto nameIt = m_exprBuilder->internalizedFunctionNames.find(function->id());
		if (nameIt == m_exprBuilder->internalizedFunctionNames.end()) continue;
		// Attribute EIP-1967 admin-slot use to the freestanding function and
		// attach it through this contract's call graph.
		m_typeMapper.artifacts().currentFreestandingFunctionId = function->id();
		auto method = buildFunction(
			*function, _contractName, nameIt->second, /*asInternalCopy=*/true);
		m_typeMapper.artifacts().currentFreestandingFunctionId = -1;
		appendMethodWithModifierSubs(_contractNode, std::move(method));
	}
}

void ContractBuilder::emitFunctionPointerDispatch(awst::Contract& _contractNode)
{
	// Generate function pointer dispatch tables
	{
		// Set subroutine IDs for library/free function targets so dispatch
		// uses SubroutineID (resolvable by puya) instead of InstanceMethodTarget.
		eb::FunctionPointerBuilder::setSubroutineIds(
			*m_exprBuilder, m_functionSymbols);

		auto const& cref = m_contractId;
		awst::SourceLocation loc(m_sourceFile);
		auto& dispCtx = *m_exprBuilder;
		auto dispatchMethods = eb::FunctionPointerBuilder::generateDispatchMethods(
			dispCtx, cref, loc, &m_dispatchSubroutines, &_contractNode.methods);
		for (auto& m : dispatchMethods)
			_contractNode.methods.push_back(std::move(m));
		eb::FunctionPointerBuilder::reset(*m_exprBuilder);
	}

	// Drain any Subroutines emitted for reachable Yul functions so the
	// contract-builder caller picks them up alongside fn-ptr dispatchers.
	{
		auto yulSubs = std::move(m_typeMapper.artifacts().pendingYulSubroutines);
		m_typeMapper.artifacts().pendingYulSubroutines.clear();
		for (auto& sub: yulSubs)
			m_dispatchSubroutines.push_back(std::move(sub));
	}
}

void ContractBuilder::scopeStorageDispatchCalls(
	StorageRuntimePlan const& _storagePlan,
	awst::Contract& _contractNode)
{
	// Default-layout dispatch bodies are contract-specific because they route
	// logical slots to this contract's named AVM cells. Scope every generated
	// call to the same contract-specific root ID. EVM-layout runtime helpers are
	// compilation-unit singletons and retain their stable global IDs.
	if (m_typeMapper.profile().evmStorageLayout || !_storagePlan.needsDispatch())
		return;
	auto const& contractId = m_contractId;
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
	awst::visitExpressions(_contractNode.approvalProgram, scopeStorageCall);
	awst::visitExpressions(_contractNode.clearProgram, scopeStorageCall);
	for (auto& method: _contractNode.methods)
		awst::visitExpressions(method, scopeStorageCall);
	for (auto& subroutine: m_dispatchSubroutines)
		if (subroutine && subroutine->body)
			awst::visitExpressions(*subroutine->body, scopeStorageCall);
}

void ContractBuilder::warnEscapedErc1967Slots(awst::Contract const& _contractNode)
{
	if (!m_typeMapper.profile().proxyAdaptation)
		return;
	// A 1967 slot constant SURVIVING translation means it escaped into
	// runtime data flow (classify consumes direct sload/sstore uses; the
	// let-fold emits no store) — the OZ StorageSlot shape. Warn: storage
	// through a derived slot value splits from the native proxy model.
	std::set<proxies::Erc1967Slot> warned;
	proxies::Erc1967Lowering::warnEscapedSlotConstants(
		_contractNode.approvalProgram, warned);
	proxies::Erc1967Lowering::warnEscapedSlotConstants(
		_contractNode.clearProgram, warned);
	for (auto const& method: _contractNode.methods)
		proxies::Erc1967Lowering::warnEscapedSlotConstants(method, warned);
}

void ContractBuilder::emitErc1967AdminGate(
	solidity::frontend::ContractDefinition const& _contract,
	awst::Contract& _contractNode)
{
	if (!m_typeMapper.profile().proxyAdaptation)
		return;
	// EIP-1967 (proxy.md §1): if any admin-slot use was lowered while
	// translating THIS contract's bodies — or inside a freestanding library/
	// free function THIS contract's call graph reaches (OZ's ERC1967Utils is a
	// library, translated before any contract) — synthesize the admin global
	// and the UpdateApplication method gating native updates on it. Snapshot-
	// and-reset the direct flag so one contract's proxy machinery never leaks
	// into the next unit member. Placed after ALL method translation (ordinary
	// externals build in the function loops, not in buildApprovalProgram).
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
		auto loc = _contractNode.approvalProgram.sourceLocation;
		_contractNode.appState.push_back(
			proxies::Erc1967Lowering::adminStateDefinition(loc));
		_contractNode.methods.push_back(
			proxies::Erc1967Lowering::updateGateMethod(_contractNode.id, loc));
	}
}

void ContractBuilder::emitUupsUpdateGate(
	solidity::frontend::ContractDefinition const& _contract,
	awst::Contract& _contractNode)
{
	// UUPS (proxy.md §3): a concrete contract inheriting OZ UUPSUpgradeable
	// with an implemented _authorizeUpgrade gets the native update gate —
	// the hook's translated method (modifiers inlined) is the permission
	// check, run inside the UpdateApplication txn.
	if (!m_typeMapper.profile().proxyAdaptation
		|| !proxies::UupsLowering::isUupsImplementation(_contract))
		return;
	// The translated hook remains the chain entry: its wrapper invokes the
	// outermost modifier subroutine and therefore preserves the complete
	// permission check. Internal methods use their registered opaque symbol,
	// so resolve the concrete override instead of looking for the Solidity
	// source name (or coupling the gate to a generated `__mod0` name).
	solidity::frontend::FunctionDefinition const* authorizeFunction = nullptr;
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		if (!base) continue;
		for (auto const* function: base->definedFunctions())
			if (function && function->name() == "_authorizeUpgrade"
				&& function->isImplemented())
			{
				authorizeFunction = function;
				break;
			}
		if (authorizeFunction) break;
	}

	awst::ContractMethod const* hook = nullptr;
	if (authorizeFunction)
	{
		std::string hookName = authorizeFunction->name();
		if (auto const* symbol =
			m_functionSymbols.resolve(authorizeFunction->id()))
			hookName = *symbol;
		for (auto const& method: _contractNode.methods)
			if (method.memberName == hookName)
			{
				hook = &method;
				break;
			}
	}
	if (hook)
		_contractNode.methods.push_back(proxies::UupsLowering::updateGateMethod(
			_contractNode.id, *hook, _contractNode.approvalProgram.sourceLocation));
}

std::shared_ptr<awst::Contract> ContractBuilder::build(
	solidity::frontend::ContractDefinition const& _contract,
	StorageRuntimePlan const& _storagePlan,
	bool _emitEvmStorageRuntime
)
{
	std::string const contractName = beginContract(_contract, _storagePlan);
	std::set<int64_t> const overriddenIds = collectOverloadedNames(_contract);
	createExpressionBuilder(_contract, _storagePlan, contractName);
	auto const reachableHostBoundFunctions =
		collectReachableHostBoundFunctions(_contract);
	registerHostBoundFunctionNames(reachableHostBoundFunctions);
	createFunctionContexts();

	auto contract = makeContractNode(_contract, contractName);
	buildPrograms(_contract, contractName, *contract);

	std::set<std::string> translatedFunctions;
	buildDefinedFunctions(_contract, contractName, *contract, translatedFunctions);
	// Getters before inherited functions so `uint256 public override test` beats
	// an inherited `function test()`.
	buildPublicStateVariableGetters(_contract, *contract, contractName, translatedFunctions);
	// Inherited functions (after getters — same precedence rule).
	buildInheritedFunctions(
		_contract, contractName, *contract, overriddenIds, translatedFunctions);

	buildRouters(_contract, *contract);
	buildHostBoundFunctions(contractName, *contract, reachableHostBoundFunctions);
	// Close the concrete-implementation worklist before generating dispatchers.
	emitSuperSubroutines(*contract, contractName);

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

	emitFunctionPointerDispatch(*contract);
	scopeStorageDispatchCalls(_storagePlan, *contract);
	warnEscapedErc1967Slots(*contract);
	emitErc1967AdminGate(_contract, *contract);
	emitUupsUpdateGate(_contract, *contract);

	return contract;
}

} // namespace puyasol::builder
