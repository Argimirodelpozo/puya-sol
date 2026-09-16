/// @file FunctionPointerBuilder.cpp
/// Implements function pointer support — dispatch tables for internal,
/// inner app calls for external.

#include "builder/lowering/calls/FunctionPointerBuilder.h"
#include "builder/solc/FunctionIdentity.h"
#include "builder/target/ApplicationTarget.h"
#include "builder/lowering/itxn/ApplicationCall.h"
#include "builder/lowering/itxn/NativePayment.h"
#include "awst/NameGen.h"
#include "builder/target/EvmFeaturePolicy.h"
#include "builder/codec/SelectorSemantics.h"
#include "builder/solc/SolcFacts.h"
#include "builder/lowering/abi/AbiEncoderBuilder.h"
#include "builder/codec/EvmAbiDecode.h"
#include "builder/codec/EvmAbiEncode.h"
#include "builder/lowering/calls/CallResolver.h"
#include "builder/lowering/calls/FunctionPointerDispatchTypes.h"
#include "builder/types/FunctionPointerKind.h"
#include "builder/types/ConversionPlan.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/SolIntType.h"
#include "builder/ast/calls/RevertBlob.h"
#include "Logger.h"

#include <algorithm>
#include <cctype>
#include "builder/lowering/itxn/InnerCallHandlers.h"

namespace puyasol::builder::eb
{

using namespace solidity::frontend;

namespace
{

/// Freestanding bodies cannot use InstanceMethodTarget. The resolved host
/// declaration is authoritative; display-name substring matching is not.
bool inRootContext(ContractContext const& _ctx)
{
	return !_ctx.currentContract && !_ctx.functionPointers.currentCref.empty();
}

} // namespace

std::shared_ptr<awst::SubroutineCallExpression> FunctionPointerBuilder::buildDispatchCall(
	ContractContext& _ctx,
	FunctionType const* _funcType,
	std::shared_ptr<awst::Expression> _ptrIdExpr,
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc)
{
	auto& registry = _ctx.functionPointers;
	std::string dname = dispatchName(_funcType);
	registry.neededDispatches[dname] = _funcType;
	bool const rootContext = inRootContext(_ctx);
	if (rootContext)
		registry.neededRootDispatches.insert(dname);

	awst::SubroutineTarget target = rootContext
		? awst::SubroutineTarget{awst::SubroutineID{registry.currentCref + "." + dname}}
		: awst::SubroutineTarget{awst::InstanceMethodTarget{dname}};
	auto call = awst::makeSubroutineCall(
		std::move(target), computeReturnType(_ctx, _funcType), _loc);

	awst::pushCallArg(call->args, "__funcptr_id", std::move(_ptrIdExpr));

	// Only external view/pure calls create an EVM static context. Internal
	// pointers jump without changing the caller's execution context.
	bool staticCtx = isExternalFunctionPointer(_funcType)
		&& (_funcType->stateMutability() == StateMutability::View
			|| _funcType->stateMutability() == StateMutability::Pure);
	awst::pushCallArg(call->args, "__static",
		staticCtx ? awst::makeIntegerConstant("1", _loc)
			: ApplicationCall::staticContext(_ctx.typeMapper, _loc));

	for (size_t i = 0; i < _args.size(); ++i)
	{
		awst::CallArg arg;
		arg.name = "__arg" + std::to_string(i);
		arg.value = _args[i];
		call->args.push_back(std::move(arg));
	}
	return call;
}

void FunctionPointerBuilder::setCurrentCref(
	ContractContext& _ctx, std::string _cref)
{
	_ctx.functionPointers.currentCref = std::move(_cref);
}

void FunctionPointerBuilder::reset(ContractContext& _ctx)
{
	_ctx.functionPointers.reset();
}

// ── Type mapping ──

awst::WType const* FunctionPointerBuilder::mapFunctionType(
	ContractContext& _ctx,
	FunctionType const* _funcType)
{
	if (!_funcType)
		return awst::WType::uint64Type();

	if (isExternalFunctionPointer(_funcType))
		return _ctx.typeMapper.map(_funcType);

	// Internal function pointers: uint64 ID
	return awst::WType::uint64Type();
}

// ── Register a function as a pointer target ──

unsigned FunctionPointerBuilder::registerTarget(
	ContractContext& _ctx,
	FunctionDefinition const* _funcDef,
	FunctionType const* _funcType,
	std::string _awstName)
{
	if (!_funcDef) return 0;
	auto& registry = _ctx.functionPointers;
	auto const id = _funcDef->id();
	if (_ctx.baseImplementationIds.count(id))
		_awstName = CallResolver::baseImplementationName(_ctx, *_funcDef);
	if (auto found = registry.targets.find(id); found != registry.targets.end())
	{
		if (!_awstName.empty())
			found->second.name = std::move(_awstName);
		return found->second.id;
	}
	if (_awstName.empty())
		if (auto symbol = functionSymbol(*_funcDef))
			_awstName = *symbol;
	if (_awstName.empty())
		_awstName = CallResolver::resolveMethodName(_ctx, *_funcDef);
	auto pointerId = registry.nextId++;
	registry.targets.emplace(id, FuncPtrEntry{
		id, std::move(_awstName), pointerId, _funcType, _funcDef, ""
	});
	return pointerId;
}

void FunctionPointerBuilder::setSubroutineIds(ContractContext& _ctx)
{
	for (auto& [id, entry] : _ctx.functionPointers.targets)
	{
		if (_ctx.baseImplementationIds.count(id))
		{
			entry.name = CallResolver::baseImplementationName(_ctx, *entry.funcDef);
			continue;
		}
		if (auto const hostBound = _ctx.internalizedFunctionNames.find(id);
			hostBound != _ctx.internalizedFunctionNames.end())
		{
			entry.name = hostBound->second;
			entry.subroutineId.clear();
			continue;
		}
		if (auto symbol = functionSymbol(*entry.funcDef))
		{
			if (isRootSubroutine(*entry.funcDef))
				entry.subroutineId = *symbol;
			else
				entry.name = *symbol;
		}
	}
}

// ── Build a reference to a function (taking its "address") ──

std::shared_ptr<awst::Expression> FunctionPointerBuilder::buildFunctionReference(
	ContractContext& _ctx,
	FunctionDefinition const* _funcDef,
	awst::SourceLocation const& _loc,
	FunctionType const* _callerFuncType,
	std::shared_ptr<awst::Expression> _receiverAddress,
	std::string const& _awstName)
{
	if (!_funcDef)
	{
		// Zero-initialized function pointer
		auto zero = awst::makeZero(_loc);
		return zero;
	}

	// Use caller FunctionType if given (e.g. `this.g` is External even if g has an Internal overload).
	auto const* funcType = _callerFuncType;
	if (!funcType)
	{
		funcType = _funcDef->functionType(true); // internal
		if (!funcType)
			funcType = _funcDef->functionType(false); // external
	}

	// Register as target
	auto const funcId = registerTarget(_ctx, _funcDef, funcType, _awstName);

	bool isExternal = isExternalFunctionPointer(funcType);

	if (isExternal)
	{
		// Compatibility layout: appId[8] ++ ARC4-selector[4]. Under
		// --evm-selectors the pointer carries appId[8] ++ Solidity-selector[4]
		// ++ ARC4-selector[4], keeping language and transport identities distinct.
		// `this.f` → CurrentApplicationID + selectors: dispatch site
		//   compares appId == CurrentApplicationID and takes internal-dispatch shortcut.
		// `C(addr).f` uses the ARC-4 field when issuing an inner app txn.
		auto const* pointerType = _ctx.typeMapper.map(funcType);

		std::shared_ptr<awst::Expression> appIdBytes;
		std::shared_ptr<awst::Expression> routeSelector;

		if (_receiverAddress)
		{
			appIdBytes = awst::makeItob(ApplicationTarget::pointerId(
				_ctx.typeMapper.profile(), _receiverAddress, _loc), _loc);

			// Routing selector: used as ApplicationArgs[0].
			auto selectorConst = awst::makeMethodConstant(
				InnerCallHandlers::buildMethodSelector(_ctx, _funcDef),
				awst::WType::bytesType(), _loc);
			routeSelector = std::move(selectorConst);
		}
		else
		{
			EvmFeaturePolicy::report(
				EvmFeature::SelfCall, _ctx.typeMapper.profile(), _loc);

			// Self-ref: store CurrentApplicationID (not 0) so the pointer survives
			// crossing contract boundaries. Dispatch site shortcuts to internal dispatch
			// when appId == CurrentApplicationID.
			if (auto const* internalFuncType = _funcDef->functionType(true))
				registerTarget(_ctx, _funcDef, internalFuncType, _awstName);

			auto curApp = awst::makeGlobal(
				std::string("CurrentApplicationID"), awst::WType::uint64Type(), _loc);
			appIdBytes = awst::makeItob(std::move(curApp), _loc);
			auto selectorConst = awst::makeMethodConstant(
				InnerCallHandlers::buildMethodSelector(_ctx, _funcDef),
				awst::WType::bytesType(), _loc);
			routeSelector = std::move(selectorConst);
		}

		std::shared_ptr<awst::Expression> left = std::move(appIdBytes);
		if (_ctx.typeMapper.profile().evmSelectors)
		{
			auto const* externalType = funcType;
			if (!externalType || !isExternalFunctionPointer(externalType))
				externalType = _funcDef->functionType(false);
			if (!externalType)
				return nullptr;
			auto semanticSelector = builder::SelectorSemantics::functionSelector(
				_ctx, *externalType,
				InnerCallHandlers::buildMethodSelector(_ctx, _funcDef), _loc);
			left = awst::makeConcat(
				std::move(left), std::move(semanticSelector), _loc);
		}

		auto packed = awst::makeIntrinsicCall("concat", pointerType, _loc);
		packed->stackArgs.push_back(std::move(left));
		packed->stackArgs.push_back(std::move(routeSelector));
		return packed;
	}

	// Internal references to the same concrete implementation share an ID.
	return awst::makeIntegerConstant(funcId, _loc);
}

// ── Build a call through a function pointer ──

std::shared_ptr<awst::Expression> FunctionPointerBuilder::buildFunctionPointerCall(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _ptrExpr,
	FunctionType const* _funcType,
	std::vector<std::shared_ptr<awst::Expression>> _args,
	awst::SourceLocation const& _loc,
	std::shared_ptr<awst::Expression> _callValue)
{
	if (!_funcType)
		return nullptr;

	bool isExternal = isExternalFunctionPointer(_funcType);
	bool const evmContractAbi =
		_ctx.typeMapper.profile().contractAbi == ContractAbi::Evm;

	if (isExternal)
	{
		// The pointer expression is sliced repeatedly below (appId ×2, selector
		// ×2) — pin it so a side-effecting pointer source evaluates once.
		_ptrExpr = awst::makeEvalOnce(std::move(_ptrExpr), _loc);
		// Self-call (appId == CurrentApplicationID) → internal dispatch; else inner txn.
		auto extractSlice = [&](int _offset, int _length) {
			return awst::makeExtract(_ptrExpr, _offset, _length, _loc);
		};
		auto extractU64 = [&](int _offset) {
			return awst::makeBtoi(extractSlice(_offset, 8), _loc);
		};

		auto isSelf = awst::makeNumericCompare(
			extractU64(0), awst::NumericComparison::Eq,
			awst::makeGlobal(
				std::string("CurrentApplicationID"),
				awst::WType::uint64Type(), _loc),
			_loc);

		auto const routeSelectorOffset = evmContractAbi
			? static_cast<int>(externalFunctionPointerSoliditySelectorOffset)
			: static_cast<int>(externalFunctionPointerRouteSelectorOffset(
				_ctx.typeMapper.profile()));

		// Map the routing selector → internal id via __sel_to_id_<sig>.
		std::string selToIdName = "__sel_to_id_" + dispatchName(_funcType);
		auto& registry = _ctx.functionPointers;
		std::string const dname = dispatchName(_funcType);
		registry.neededSelectorDispatches.insert(dname);
		registry.neededDispatches[dname] = _funcType;
		bool const rootContext = inRootContext(_ctx);
		if (rootContext)
			registry.neededRootDispatches.insert(dname);

		awst::SubroutineTarget selToIdTarget = rootContext
			? awst::SubroutineTarget{awst::SubroutineID{registry.currentCref + "." + selToIdName}}
			: awst::SubroutineTarget{awst::InstanceMethodTarget{selToIdName}};
		auto selToIdCall = awst::makeSubroutineCall(
			std::move(selToIdTarget), awst::WType::uint64Type(), _loc);
		awst::pushCallArg(selToIdCall->args, "__sel",
			extractSlice(routeSelectorOffset, 4));

		std::shared_ptr<awst::Expression> selfCall = buildDispatchCall(_ctx, _funcType, std::move(selToIdCall), _args, _loc);
		awst::WType const* retType = selfCall->wtype;

		// Cross-contract selector chosen by the contract wire profile.
		auto sel4 = extractSlice(routeSelectorOffset, 4);

		auto argsTuple = awst::makeTupleExpression(nullptr, _loc);
		argsTuple->items.push_back(std::move(sel4));
		if (evmContractAbi)
		{
			std::vector<std::shared_ptr<awst::Expression>> converted;
			for (size_t i = 0; i < _args.size(); ++i)
			{
				auto const* parameter = i < _funcType->parameterTypes().size()
					? _funcType->parameterTypes()[i] : nullptr;
				auto value = _args[i];
				if (parameter)
					value = builder::ConversionPlan{
						nullptr, parameter, _ctx.typeMapper.map(parameter),
						builder::ConversionPlan::Context::AbiArgument}.emit(
							std::move(value), _loc);
				converted.push_back(std::move(value));
			}
			argsTuple->items.push_back(abi::encodeEvmAbi(
				_ctx.typeMapper, _funcType->parameterTypes(),
				std::move(converted), _loc, _ctx.preEffects()));
		}
		else
		{
			for (size_t i = 0; i < _args.size(); ++i)
			{
				solidity::frontend::Type const* paramSolType =
					i < _funcType->parameterTypes().size()
						? _funcType->parameterTypes()[i] : nullptr;
				argsTuple->items.push_back(
					InnerCallHandlers::encodeArgToBytes(
						_ctx, _args[i], nullptr, paramSolType, _loc));
			}
		}
		{
			std::vector<awst::WType const*> argTypes;
			for (auto const& item : argsTuple->items)
				argTypes.push_back(item->wtype);
			argsTuple->wtype = _ctx.typeMapper.createType<awst::WTuple>(std::move(argTypes), std::nullopt);
		}

		auto ifStmt = awst::makeIfElse(isSelf, awst::makeBlock(_loc), awst::makeBlock(_loc), _loc);

		// Self calls retain the acknowledged outer-msg.value adaptation;
		// option effects have already executed even though no payment is emitted.
		selfCall = ApplicationCall::withStaticContext(_ctx.typeMapper, std::move(selfCall),
			_funcType->stateMutability() <= StateMutability::View, _loc, ifStmt->ifBranch->body);
		auto& foreign = ifStmt->elseBranch->body;
		auto app = awst::makeAsApplication(extractU64(0), _loc);
		auto payment = _callValue ? buildNativePayment(_ctx.typeMapper.profile(),
			foreign, app, std::move(_callValue), _loc) : nullptr;
		auto payload = ApplicationCall::submit(_ctx.typeMapper, std::move(app),
			std::move(argsTuple), std::move(payment), _loc, foreign);

		if (retType == awst::WType::voidType())
		{
			ifStmt->ifBranch->body.push_back(awst::makeExpressionStatement(selfCall, _loc));
			ApplicationCall::setReturnData(_ctx.typeMapper, awst::makeBytesConstant({}, _loc),
				_loc, ifStmt->ifBranch->body);
			_ctx.preEffects().push_back(std::move(ifStmt));
			auto vc = awst::makeVoidConstant(_loc);
			return vc;
		}

		// Non-void: spill both branches' result into a shared temp.
		std::string tmpName = "__fnptr_res_" + std::to_string((awst::NameGen::next("FunctionPointerBuilder.s_tmpCounter") + 1));
		auto writeTmp = [&](std::shared_ptr<awst::Expression> _val) {
			auto target = awst::makeVarExpression(tmpName, retType, _loc);
			return awst::makeAssignmentStatement(std::move(target), std::move(_val), _loc);
		};
		ifStmt->ifBranch->body.push_back(writeTmp(selfCall));
		ApplicationCall::setTypedReturnData(_ctx.typeMapper,
			awst::makeVarExpression(tmpName, retType, _loc), _funcType->returnParameterTypes(),
			evmContractAbi, _loc, ifStmt->ifBranch->body);
		ifStmt->elseBranch->body.push_back(writeTmp(
			decodeExternalCallResult(_ctx.typeMapper, std::move(payload),
				_funcType->returnParameterTypes(), retType, _loc, foreign)));
		_ctx.preEffects().push_back(std::move(ifStmt));

		return awst::makeVarExpression(tmpName, retType, _loc);
	}

	// Internal: dispatch by id.
	return buildDispatchCall(_ctx, _funcType, std::move(_ptrExpr), _args, _loc);
}

// ── Dispatch name from function type signature ──

std::string FunctionPointerBuilder::dispatchName(
	FunctionType const* _funcType)
{
	// One dispatch group per DISTINCT signature: solc's Type::identifier()
	// is canonical and injective (t_uint8 vs t_int8 vs t_address vs
	// t_string_memory_ptr ...). The old namer collapsed signedness
	// (int8/uint8 both "_u8") and every non-int type to "_x", merging
	// distinct pointer signatures into one group typed by whichever
	// signature registered first.
	auto typeTag = [](Type const* t) -> std::string {
		if (!t) return "x";
		std::string id = t->identifier();
		for (auto& c: id)
			if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
				c = '_';
		return id;
	};
	std::string name = "__funcptr_dispatch";
	if (_funcType)
	{
		for (auto const* pt : _funcType->parameterTypes())
			name += "_" + typeTag(pt);
		name += "_ret";
		for (auto const* rt : _funcType->returnParameterTypes())
			name += "_" + typeTag(rt);
	}
	return name;
}

// ── Generate dispatch subroutines ──

namespace dispatch_detail
{

/// Group registered targets by dispatch signature. Foreign non-resolvable
/// targets (different non-library contract, no subroutine id) are dropped; signatures
/// demanded by call sites but with no surviving targets keep an EMPTY group
/// (the dispatch subroutine must still exist so references resolve).
std::map<std::string, std::vector<FuncPtrEntry const*>> collectDispatchGroups(
	FunctionPointerRegistry const& _registry, ContractDefinition const* _contract)
{
	std::map<std::string, std::vector<FuncPtrEntry const*>> groups;
	for (auto const& [key, entry] : _registry.targets)
	{
		std::string dname = FunctionPointerBuilder::dispatchName(entry.funcType);
		// Taking a function's address does not require a dispatcher. A dynamic
		// call or external self-call records the signature in neededDispatches.
		if (!_registry.neededDispatches.count(dname))
			continue;
		// Use solc's declaration identity and C3 hierarchy, not parsed cref or
		// short-name equality. Inherited public targets belong to this host too.
		auto const* fdContract = entry.funcDef ? entry.funcDef->annotation().contract : nullptr;
		bool inHierarchy = false;
		if (_contract)
		{
			auto const& bases = _contract->annotation().linearizedBaseContracts;
			inHierarchy = std::find(bases.begin(), bases.end(), fdContract) != bases.end();
		}
		if (fdContract && _contract && !inHierarchy
			&& !fdContract->isLibrary()
			&& entry.subroutineId.empty())
			continue;
		groups[dname].push_back(&entry);
	}
	// Ensure needed signatures have entries, even if empty.
	for (auto const& [dname, funcType] : _registry.neededDispatches)
	{
		if (groups.find(dname) == groups.end())
			groups[dname] = {};
	}
	return groups;
}

/// Method skeleton: native return type + (__funcptr_id, __static, params).
awst::ContractMethod buildDispatchSignature(
	ContractContext& _ctx,
	std::string const& _cref,
	std::string const& _dname,
	FunctionType const* _funcType,
	awst::SourceLocation const& _loc)
{
	// Return type: the SAME native mapping the call site uses
	// (computeReturnType — single native type or WTuple for multi).
	// The old mapDispatchType drifted from the call site (public signed
	// ≤64 promoted to biguint vs uint64 at the call; multi-return was a
	// silent void). Public targets return WIRE-encoded values — the
	// per-entry body adapts them back to the native return.
	// Args: __funcptr_id first, then __static, then function params.
	auto dispatch = awst::ContractMethod(_cref, _dname,
		computeReturnType(_ctx, _funcType),
		{{"__funcptr_id", awst::WType::uint64Type(), _loc},
			{"__static", awst::WType::uint64Type(), _loc}}, _loc);
	for (size_t i = 0; i < _funcType->parameterTypes().size(); ++i)
	{
		// The SAME native mapping the call site coerces to — the old
		// mapDispatchType sent address/enum/struct/non-byte-array params
		// to biguint while the call site passed account/uint64/array
		// wtypes.
		dispatch.args.emplace_back(
			"__arg" + std::to_string(i),
			_ctx.typeMapper.map(_funcType->parameterTypes()[i]), _loc);
	}
	return dispatch;
}

/// Innermost else of the id chain: EVM reverts with Panic(0x51) for an
/// invalid/uninitialized internal function pointer — log that payload before
/// the assert so the revert-data oracle sees identical bytes (same
/// log-then-err convention as require/assert, RevertBlob.h).
std::shared_ptr<awst::Block> buildInvalidPointerBlock(
	awst::SourceLocation const& _loc)
{
	auto defaultBlock = awst::makeBlock(_loc);
	auto logCall = awst::makeIntrinsicCall(
		"log", awst::WType::voidType(), _loc);
	logCall->stackArgs.push_back(awst::makeBytesConstant(
		sol_ast::panicRevertBlobBytes(0x51), _loc));
	defaultBlock->body.push_back(
		awst::makeExpressionStatement(std::move(logCall), _loc));
	auto stmt = awst::makeExpressionStatement(awst::makeAssert(
		awst::makeFalse(_loc), _loc, "invalid function pointer"), _loc);
	defaultBlock->body.push_back(std::move(stmt));
	return defaultBlock;
}

/// One `__funcptr_id == N` arm: static-context write protection, target call
/// with per-param coercion, and the wire→native return adaptation for PUBLIC
/// targets (whose returns are build-time wire-encoded).
std::shared_ptr<awst::Block> buildDispatchEntryArm(
	ContractContext& _ctx,
	FuncPtrEntry const* entry,
	FunctionType const* funcType,
	awst::ContractMethod const& dispatch,
	awst::ContractMethod const* targetMethod,
	awst::SourceLocation const& _loc)
{
	auto ifBlock = awst::makeBlock(_loc);
	// Write protection: this target mutates state — a static-context
	// dispatch (view/pure pointer, id laundered in via asm) reverts,
	// mirroring EVM's failed staticcall (tstore_hidden_staticcall).
	if (entry->funcDef
		&& (entry->funcDef->stateMutability() == StateMutability::NonPayable
			|| entry->funcDef->stateMutability() == StateMutability::Payable))
	{
		auto stVar = awst::makeVarExpression(
			"__static", awst::WType::uint64Type(), _loc);
		auto isZero = awst::makeNumericCompare(std::move(stVar),
			awst::NumericComparison::Eq,
			awst::makeIntegerConstant(uint64_t{0}, _loc), _loc);
		ifBlock->body.push_back(awst::makeExpressionStatement(
			awst::makeAssert(std::move(isZero), _loc,
				"write protection"), _loc));
	}
	{
		awst::SubroutineTarget target = !entry->subroutineId.empty()
			? awst::SubroutineTarget{awst::SubroutineID{entry->subroutineId}}
			: awst::SubroutineTarget{awst::InstanceMethodTarget{entry->name}};
		auto call = awst::makeSubroutineCall(
			std::move(target), targetMethod ? targetMethod->returnType : dispatch.returnType, _loc);

		// ABI wrappers move routing config off this internally callable wire body.
		bool const isPublic = targetMethod && entry->funcDef
			&& entry->funcDef->isPartOfExternalInterface()
			&& entry->name == CallResolver::resolveMethodName(_ctx, *entry->funcDef);
		auto const* plan = entry->funcDef
			? &_ctx.typeMapper.callBoundaryPlan(*entry->funcDef, _ctx.currentContract) : nullptr;
		for (size_t i = 0; i < funcType->parameterTypes().size(); ++i)
		{
			awst::CallArg arg;
			arg.value = awst::makeVarExpression("__arg" + std::to_string(i), dispatch.args[i + 2].wtype, _loc);
			if (plan)
			{
				auto const& parameter = plan->parameters.at(i);
				arg.name = isPublic ? parameter.wireName() : parameter.name;
				if (isPublic) arg.value = parameter.encodeArgument(std::move(arg.value), _loc);
				else arg.value = TypeCoercion::coerceForAssignment(std::move(arg.value), parameter.type, _loc);
			}
			if (targetMethod)
				arg.name = targetMethod->args.at(i).name;
			call->args.push_back(std::move(arg));
		}

		if (dispatch.returnType != awst::WType::voidType())
		{
			auto retValue = decodeCallResult(std::move(call), dispatch.returnType, _loc);
			auto ret = awst::makeReturnStatement(std::move(retValue), _loc);
			ifBlock->body.push_back(std::move(ret));
		}
		else
		{
			auto stmt = awst::makeExpressionStatement(std::move(call), _loc);
			ifBlock->body.push_back(std::move(stmt));
			auto ret = awst::makeReturnStatement(nullptr, _loc);
			ifBlock->body.push_back(std::move(ret));
		}
	}
	return ifBlock;
}

/// __sel_to_id_<sig>(__sel: bytes) -> uint64
/// Maps a 4-byte routing selector to an internal dispatch id (self-call path).
awst::ContractMethod buildSelToIdMethod(
	ContractContext& _ctx,
	std::string const& _cref,
	std::string const& dname,
	std::vector<FuncPtrEntry const*> const& entries,
	awst::SourceLocation const& _loc)
{
	auto selToId = awst::ContractMethod(_cref, "__sel_to_id_" + dname,
		awst::WType::uint64Type(),
		{{"__sel", awst::WType::bytesType(), _loc}}, _loc);

	auto selBody = selToId.body;

	auto selDefault = awst::makeBlock(_loc);
	{
		auto stmt = awst::makeExpressionStatement(awst::makeAssert(
			awst::makeFalse(_loc), _loc,
			"unknown function selector in self-call dispatch"), _loc);
		selDefault->body.push_back(std::move(stmt));
	}
	std::shared_ptr<awst::Block> selElse = selDefault;

	for (auto const* entry : entries)
	{
		if (!entry->funcDef || !entry->funcDef->isPartOfExternalInterface()) continue;
		// A base implementation shares its selector with the override, but it
		// is only reachable by internal identity, never through a self-call ABI.
		if (_ctx.currentContract && entry->funcDef->annotation().contract
			&& !entry->funcDef->annotation().contract->isLibrary()
			&& std::none_of(_ctx.currentContract->interfaceFunctionList(true).begin(),
				_ctx.currentContract->interfaceFunctionList(true).end(), [&](auto const& item) {
					return item.second->hasDeclaration()
						&& &item.second->declaration() == entry->funcDef;
				}))
			continue;
		std::shared_ptr<awst::Expression> methodConst;
		if (_ctx.typeMapper.profile().contractAbi == ContractAbi::Evm)
		{
			auto const* externalType = entry->funcDef->functionType(false);
			if (!externalType)
				continue;
			methodConst = awst::makeBytesConstant(
				builder::SolcFacts::externalSelector(*externalType), _loc,
				awst::BytesEncoding::Base16, awst::WType::bytesType());
		}
		else
			methodConst = awst::makeMethodConstant(
				InnerCallHandlers::buildMethodSelector(_ctx, entry->funcDef),
				awst::WType::bytesType(), _loc);

		auto selVar = awst::makeVarExpression("__sel", awst::WType::bytesType(), _loc);
		auto cmp = awst::makeBytesComparison(std::move(selVar),
			awst::EqualityComparison::Eq, std::move(methodConst), _loc);

		auto thenBlock = awst::makeBlock(_loc);
		thenBlock->body.push_back(awst::makeReturnStatement(
			awst::makeIntegerConstant(entry->id, _loc), _loc));

		auto ifElse = awst::makeIfElse(
			std::move(cmp), std::move(thenBlock), std::move(selElse), _loc);

		auto newElse = awst::makeBlock(_loc);
		newElse->body.push_back(std::move(ifElse));
		selElse = std::move(newElse);
	}
	for (auto& stmt : selElse->body)
		selBody->body.push_back(std::move(stmt));
	return selToId;
}

} // namespace dispatch_detail

std::vector<awst::ContractMethod> FunctionPointerBuilder::generateDispatchMethods(
	ContractContext& _ctx,
	std::string const& _cref,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Subroutine>>* _outRootSubs,
	std::vector<awst::ContractMethod> const* _existingMethods)
{
	using namespace dispatch_detail;
	std::vector<awst::ContractMethod> methods;
	auto const& registry = _ctx.functionPointers;

	if (registry.neededDispatches.empty())
		return methods;

	// A pointer taken through a base/interface still routes to this host's
	// external override when its receiver is self. Internal virtual lookup
	// cannot select it: legal external overrides may change data location.
	if (_ctx.currentContract)
		for (auto const& [_, entry]: registry.targets)
		{
			if (!entry.funcDef || !entry.funcDef->isPartOfExternalInterface()
				|| !registry.neededSelectorDispatches.count(dispatchName(entry.funcType))) continue;
			for (auto const& [selector, type]: _ctx.currentContract->interfaceFunctionList(true))
				if (type->externalSignature() == entry.funcDef->externalSignature() && type->hasDeclaration())
					if (auto const* target = dynamic_cast<FunctionDefinition const*>(&type->declaration()))
						registerTarget(_ctx, target, type);
		}
	auto groups = collectDispatchGroups(registry, _ctx.currentContract);

	for (auto const& [dname, entries] : groups)
	{
		FunctionType const* funcType = nullptr;
		if (!entries.empty())
			funcType = entries[0]->funcType;
		else if (registry.neededDispatches.count(dname))
			funcType = registry.neededDispatches.at(dname);
		if (!funcType) continue;

		auto dispatch = buildDispatchSignature(_ctx, _cref, dname, funcType, _loc);

		auto body = awst::makeBlock(_loc);

		// Build if/else chain; innermost = Panic(0x51) + assert for invalid id.
		std::shared_ptr<awst::Block> elseBlock = buildInvalidPointerBlock(_loc);

		for (auto const* entry : entries)
		{
			// Ground-truth return type for wire adaptation: the target's
			// translated ContractMethod (public returns are wire-encoded).
			awst::ContractMethod const* targetMethod = nullptr;
			if (_existingMethods)
				for (auto const& m : *_existingMethods)
					if (m.memberName == entry->name)
					{
						targetMethod = &m;
						break;
					}
			// Public multi-return without a resolvable wire tuple (target
			// method missing, or its return isn't the expected tuple): skip
			// the entry — only an actual dynamic dispatch to it hits the
			// invalid-function-pointer assert (loud at runtime).
			if (entry->funcDef && entry->funcDef->isPartOfExternalInterface()
				&& dynamic_cast<awst::WTuple const*>(dispatch.returnType)
				&& !(targetMethod && dynamic_cast<awst::WTuple const*>(
					targetMethod->returnType)))
			{
				Logger::instance().warning(
					"function pointer to PUBLIC multi-return '" + entry->name
					+ "' cannot be dispatched (no resolvable wire tuple); "
					  "calls through this pointer will fail at runtime.", _loc);
				continue;
			}
			auto idVar = awst::makeVarExpression("__funcptr_id", awst::WType::uint64Type(), _loc);
			auto idConst = awst::makeIntegerConstant(entry->id, _loc);
			auto cmp = awst::makeNumericCompare(std::move(idVar), awst::NumericComparison::Eq, std::move(idConst), _loc);

			auto ifBlock = buildDispatchEntryArm(
				_ctx, entry, funcType, dispatch, targetMethod, _loc);

			auto ifElse = awst::makeIfElse(
				std::move(cmp), std::move(ifBlock), std::move(elseBlock), _loc);

			auto newElse = awst::makeBlock(_loc);
			newElse->body.push_back(std::move(ifElse));
			elseBlock = std::move(newElse);
		}

		for (auto& stmt : elseBlock->body)
			body->body.push_back(std::move(stmt));

		dispatch.body = body;

		// Also emit as root-level Subroutine: library subroutines can't use
		// InstanceMethodTarget outside the contract scope.
		if (_outRootSubs && registry.neededRootDispatches.count(dname))
		{
			auto sub = awst::makeSubroutine(
				_cref + "." + dispatch.memberName, dispatch.memberName,
				dispatch.args, dispatch.returnType, dispatch.body /*shared*/,
				dispatch.pure, dispatch.sourceLocation);
			_outRootSubs->push_back(std::move(sub));
		}

		methods.push_back(std::move(dispatch));

		// Only external pointer calls need selector lookup. Empty groups still
		// get a helper so their call-site references resolve.
		if (!registry.neededSelectorDispatches.count(dname)) continue;
		auto selToId = buildSelToIdMethod(_ctx, _cref, dname, entries, _loc);
		if (_outRootSubs && registry.neededRootDispatches.count(dname))
		{
			auto sub = awst::makeSubroutine(
				_cref + "." + selToId.memberName, selToId.memberName,
				selToId.args, selToId.returnType, selToId.body,
				/*pure=*/false, selToId.sourceLocation);
			_outRootSubs->push_back(std::move(sub));
		}
		methods.push_back(std::move(selToId));
	}

	return methods;
}

} // namespace puyasol::builder::eb
