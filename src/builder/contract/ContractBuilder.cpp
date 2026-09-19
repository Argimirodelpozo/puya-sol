#include "builder/solc/SourceLocConvert.h"
#include "builder/contract/RouterConditions.h"
#include "builder/contract/StorageDispatchSupport.h"
#include "builder/context/ProgramAnalysis.h"
#include <variant>
#include "builder/contract/ContractBuilder.h"
#include "builder/solc/FunctionIdentity.h"
#include "builder/contract/SelectorRouter.h"
#include "builder/codec/EvmMemoryCodec.h"
#include "awst/NameGen.h"
#include "awst/Visit.h"
#include "builder/ast/SolStatement.h"
#include "builder/yul/AssemblyBuilder.h"
#include "builder/codec/Arc4Defaults.h"
#include "builder/solc/StorageRefPointer.h"
#include "builder/storage/StateVarWalker.h"
#include "builder/lowering/calls/FunctionPointerBuilder.h"
#include "builder/types/TypeCoercion.h"
#include "builder/eb/AssemblyBoundary.h"
#include "builder/target/EvmLayoutMode.h"
#include "builder/storage/StorageLayout.h"
#include "builder/context/BuildArtifacts.h"
#include "Logger.h"
#include "builder/lowering/proxies/Erc1967Lowering.h"
#include "builder/lowering/proxies/UupsLowering.h"
#include "builder/target/EvmFeaturePolicy.h"

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
	uint64_t _opupBudget,
	std::map<std::string, uint64_t> const& _ensureBudget,
	std::vector<solidity::frontend::FunctionDefinition const*> const& _hostBoundFunctions
)
	: m_typeMapper(_typeMapper),
	  m_storageMapper(_storageMapper),
	  m_functionPointers(_functionPointers),
	  m_sourceFile(_sourceFile),
	  m_opupBudget(_opupBudget),
	  m_ensureBudget(_ensureBudget),
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
	std::set<int64_t> const& identityDeclarations;
	explicit AssemblyAggregateScanner(std::set<int64_t>& _ids, std::set<int64_t> const& identity)
		: ids(_ids), identityDeclarations(identity)
	{}
	bool visit(solidity::frontend::Identifier const& identifier) override
	{
		if (auto const* declaration = identifier.annotation().referencedDeclaration;
			declaration && identityDeclarations.contains(declaration->id()))
			ids.insert(declaration->id());
		return true;
	}
	bool visit(solidity::frontend::VariableDeclaration const& declaration) override
	{
		if (identityDeclarations.contains(declaration.id())) ids.insert(declaration.id());
		return true;
	}

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
	return !_analysis.isCallableReachable(_contract.id(), _fn.id());
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
	auto found = _typeMapper.analysis().functionDeclarations.find(_fn.callableId);
	if (found == _typeMapper.analysis().functionDeclarations.end()) return;
	// Named return parameters are locals too. Their native defaults are
	// prepended before these spills; Yul and reference aliases need their
	// offsets registered before the body is lowered.
	auto parameters = found->second->parameters();
	parameters.insert(parameters.end(), found->second->returnParameters().begin(),
		found->second->returnParameters().end());
	for (auto const& parameter: parameters)
	{
		auto const* vd = parameter.get();
		auto id = vd->id();
		if (!_fn.scope.bindings.assemblyAggregates.contains(id)
			|| vd->referenceLocation() != solidity::frontend::VariableDeclaration::Location::Memory
			|| vd->name().empty() || !_fn.scope.bindings.blobAggregates.get(id).empty())
			continue;
		auto const* type = vd->type();
		auto const* native = _typeMapper.map(type);
		std::string offset = "__blobagg_off_" + std::to_string(id);
		auto loc = makeLoc(_typeMapper, _sourceFile, vd->location());
		if (!emitBlobBackValue(_typeMapper, type, native,
			awst::makeVarExpression(vd->name(), native, loc), offset, static_cast<int>(id), loc, _out))
			throw SizeError("Cannot preserve memory parameter identity");
		_fn.scope.bindings.blobAggregates.set(id, offset);
		if (vd->isReturnParameter()) continue;
		std::string original = offset + "_entry";
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(original, awst::WType::uint64Type(), loc),
			awst::makeVarExpression(offset, awst::WType::uint64Type(), loc), loc));
		_fn.originalMemoryParams.emplace(id, std::move(original));
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
	AssemblyAggregateScanner scanner{asmAggIds, _fn.tr.typeMapper.analysis().memoryIdentityDeclarations};
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
	prepareAssemblyBoundary(fn, _block, paramSpills);
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
	awst::WType const* _returnType)
{
	auto& ctx = m_functionCtx.emplace(*m_tr, _params, _returnType);
	m_exprBuilder->currentScope = &ctx.scope;
}

void ContractBuilder::prependNonPayableCheck(awst::ContractMethod& _method,
	std::string const& _arc4Selector)
{
	// EVM dispatch owns this check for every non-payable solc interface
	// function (including getters and fallback). Its targets are ordinary
	// internal subroutines: checking again here wastes bytes and can reject a
	// legitimate internal call from a payable entry. Native ARC4 dispatch
	// still relies on the selector-gated body check below.
	if (m_typeMapper.profile().contractAbi == ContractAbi::Evm)
		return;
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
		m_overloadedNames,
		m_functionPointers
	);
	m_exprBuilder->currentContract = &_contract;

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
				|| m_typeMapper.analysis().isCallableReachable(
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
	m_functionCtx.emplace(*m_tr,
		std::vector<std::pair<std::string, awst::WType const*>>{},
		nullptr);
	m_exprBuilder->currentScope = &m_functionCtx->scope;

	m_exprBuilder->transientStorage =
		m_transientStorage.hasTransientVars() ? &m_transientStorage : nullptr;
	// StorageBackend is per-contract (TransientStorage is per-contract).
	m_storageBackend.emplace(m_storageMapper, m_exprBuilder->transientStorage);
	m_exprBuilder->storageBackend = &*m_storageBackend;

	m_exprBuilder->functionPointers.currentCref = m_contractId;
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

	// Numbered slots stay opaque; immutables are named cells in BOTH layouts
	// and must contribute their real keys to ARC-56 and deployment schema.
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
	if (!m_typeMapper.artifacts().contract().boxProvisionedChildren.empty())
	{
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
		solidity::ScopedSaveAndRestore freestandingIdGuard(
			m_typeMapper.artifacts().currentFreestandingFunctionId, function->id());
		auto method = buildFunction(
			*function, _contractName, nameIt->second, /*asInternalCopy=*/true);
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
			*m_exprBuilder);

		auto const& cref = m_contractId;
		awst::SourceLocation loc(m_sourceFile);
		auto& dispCtx = *m_exprBuilder;
		auto dispatchMethods = eb::FunctionPointerBuilder::generateDispatchMethods(
			dispCtx, cref, loc, &m_dispatchSubroutines, &_contractNode.methods);
		for (auto& m : dispatchMethods)
			_contractNode.methods.push_back(std::move(m));
		m_exprBuilder->functionPointers.reset();
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
	if (!_storagePlan.needsDispatch())
		return;
	bool const evm = m_typeMapper.profile().evmStorageLayout;
	if (evm && _storagePlan.requiresSparseSlots) return;
	// Named-layout dispatch is host-bound. Slot-layout variants have distinct
	// identities, chosen from this host's solc layout and reachable-call facts.
	std::string const prefix = !evm ? m_contractId + "."
		: _storagePlan.solidityLayout.totalSlots() <= kEvmSlotsPerPage
			? storage_dispatch::singlePageSlotPrefix : storage_dispatch::denseSlotPrefix;
	auto const scopeStorageCall = [&](awst::Expression& expression) {
		auto* call = dynamic_cast<awst::SubroutineCallExpression*>(&expression);
		if (!call)
			return;
		auto* id = std::get_if<awst::SubroutineID>(&call->target);
		if (!id)
			return;
		if (id->target == "__puyasol___storage_read")
			id->target = prefix + "__storage_read";
		else if (id->target == "__puyasol___storage_write")
			id->target = prefix + "__storage_write";
	};
	awst::visitExpressions(_contractNode.approvalProgram, scopeStorageCall);
	awst::visitExpressions(_contractNode.clearProgram, scopeStorageCall);
	for (auto& method: _contractNode.methods)
		awst::visitExpressions(method, scopeStorageCall);
	for (auto& method: m_typeMapper.artifacts().contract().pendingHelpers)
		awst::visitExpressions(method, scopeStorageCall);
	for (auto& subroutine: m_dispatchSubroutines)
	{
		if (!subroutine || !subroutine->body) continue;
		// Shared runtime roots (including the dynamic-array codecs) must stay
		// generic, even if the first emitted concrete host happens to be dense.
		if (evm && (subroutine->id == std::string(storage_dispatch::genericSlotPrefix) + subroutine->name
			|| subroutine->id.starts_with(storage_dispatch::denseSlotPrefix)
			|| subroutine->id.starts_with(storage_dispatch::singlePageSlotPrefix))) continue;
		awst::visitExpressions(*subroutine->body, scopeStorageCall);
	}
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

void ContractBuilder::emitProxyUpdateGate(
	solidity::frontend::ContractDefinition const& _contract,
	awst::Contract& _contractNode)
{
	if (!m_typeMapper.profile().proxyAdaptation) return;
	auto const& analysis = m_typeMapper.analysis();
	auto const& loc = _contractNode.approvalProgram.sourceLocation;
	// A single per-contract policy: admin-cell use OR the resolved UUPS hook.
	// Library facts are scoped by solc reachability, never compilation order.
	bool admin = m_typeMapper.artifacts().contract().usesErc1967Admin;
	for (auto functionId: m_typeMapper.artifacts().erc1967AdminFunctions)
		admin |= analysis.isCallableReachable(_contract.id(), functionId);
	auto const found = analysis.proxy.authorizationHooks.find(_contract.id());
	auto const* authorize = found == analysis.proxy.authorizationHooks.end() ? nullptr : found->second;
	if (admin && authorize)
	{
		Logger::instance().error(
			"proxy adaptation: ambiguous native update policy: both ERC-1967 admin storage "
			"and UUPS authorization are reachable. Select one explicit native authorization policy.", loc);
		return;
	}
	if (admin)
	{
		_contractNode.appState.push_back(proxies::Erc1967Lowering::adminStateDefinition(loc));
		_contractNode.methods.push_back(proxies::Erc1967Lowering::updateGateMethod(
			_contractNode.id, buildMessageSender(m_typeMapper, loc), loc));
	}
	else if (authorize)
	{
		// The exact solc override's wrapper retains the complete modifier chain.
		auto symbol = functionSymbol(*authorize);
		if (symbol)
			for (auto const& method: _contractNode.methods)
				if (method.memberName == *symbol)
				{
					auto gate = proxies::UupsLowering::updateGateMethod(_contractNode.id, method, loc);
					_contractNode.methods.push_back(std::move(gate));
					return;
				}
		Logger::instance().error("proxy adaptation: resolved UUPS authorization hook was not emitted.",
			makeLoc(authorize->location()));
	}
}

std::shared_ptr<awst::Contract> ContractBuilder::build(
	solidity::frontend::ContractDefinition const& _contract,
	StorageRuntimePlan const& _storagePlan,
	bool _emitEvmStorageRuntime
)
{
	BuildArtifacts::ContractScope emissions(m_typeMapper.artifacts(), _contract.id());
	awst::NameGen::Scope namingScope;
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

	buildHostBoundFunctions(contractName, *contract, reachableHostBoundFunctions);
	// Close the concrete-implementation worklist before generating dispatchers.
	emitSuperSubroutines(*contract, contractName);

	// Generate __storage_read/__storage_write dispatch subroutines
	// for assembly sload/sstore support
	// EVM-layout runtime variants have shape-keyed root IDs. Emit each once;
	// concrete hosts select their variant below without changing shared roots.
	// Default-layout dispatch remains contract-specific and is always emitted.
	if (_emitEvmStorageRuntime
		|| (!m_typeMapper.profile().evmStorageLayout && _storagePlan.needsDispatch()))
		buildStorageDispatch(_storagePlan, contract.get(), contractName);

	emitFunctionPointerDispatch(*contract);
	// Complete lifecycle methods before the EVM router decides whether the
	// contract needs residual ARC4 dispatch. Constructor work must not decide
	// whether the native update gate is reachable.
	emitProxyUpdateGate(_contract, *contract);
	buildRouters(_contract, *contract);
	emitSelfCallDispatch(_contract, *contract);
	scopeStorageDispatchCalls(_storagePlan, *contract);
	warnEscapedErc1967Slots(*contract);
	// Attach after hosted/base implementations too: those bodies can request
	// helpers after router construction. All references retain one host-local ID.
	for (auto& method: m_typeMapper.artifacts().contract().pendingHelpers)
	{
		method.cref = contract->id;
		contract->methods.push_back(std::move(method));
	}
	m_typeMapper.artifacts().pendingScratchReservations.push_back({contract, _contract.id(),
		std::move(m_typeMapper.artifacts().contract().scratchSlots)});

	return contract;
}

} // namespace puyasol::builder
