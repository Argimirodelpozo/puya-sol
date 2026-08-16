/// @file FunctionPointerBuilder.cpp
/// Implements function pointer support — dispatch tables for internal,
/// inner app calls for external.

#include "builder/itxn/FunctionPointerBuilder.h"
#include "awst/NameGen.h"
#include "builder/SelectorSemantics.h"
#include "builder/abi/AbiEncoderBuilder.h"
#include "builder/itxn/CallResolver.h"
#include "builder/itxn/FunctionPointerDispatchTypes.h"
#include "builder/sol-types/FunctionPointerKind.h"
#include "builder/sol-types/ConversionPlan.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/SolIntType.h"
#include "Logger.h"

#include <cctype>
#include "builder/itxn/InnerCallHandlers.h"

namespace puyasol::builder::eb
{

using namespace solidity::frontend;

namespace
{

/// True when translating a library subroutine (InstanceMethodTarget would fail).
bool inLibraryContext(ContractContext const& _ctx, std::string const& _currentCref)
{
	return !_ctx.contractName.empty()
		&& !_currentCref.empty()
		&& _currentCref.find("." + _ctx.contractName) == std::string::npos;
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
	bool const rootContext = inLibraryContext(_ctx, registry.currentCref);
	if (rootContext)
		registry.neededRootDispatches.insert(dname);

	awst::SubroutineTarget target = rootContext
		? awst::SubroutineTarget{awst::SubroutineID{registry.currentCref + "." + dname}}
		: awst::SubroutineTarget{awst::InstanceMethodTarget{dname}};
	auto call = awst::makeSubroutineCall(
		std::move(target), computeReturnType(_ctx, _funcType), _loc);

	awst::pushCallArg(call->args, "__funcptr_id", std::move(_ptrIdExpr));

	// EVM write protection: a view/pure-typed pointer runs its target in a
	// static context — dispatching to a NON-view target (only reachable by
	// laundering the id through asm) must revert like a failed staticcall.
	// The dispatch's non-view arms assert on this flag.
	bool staticCtx = _funcType
		&& (_funcType->stateMutability() == StateMutability::View
			|| _funcType->stateMutability() == StateMutability::Pure);
	awst::pushCallArg(call->args, "__static",
		awst::makeIntegerConstant(staticCtx ? uint64_t{1} : uint64_t{0}, _loc));

	for (size_t i = 0; i < _args.size(); ++i)
	{
		awst::CallArg arg;
		arg.name = "__arg" + std::to_string(i);
		arg.value = _args[i];
		if (i < _funcType->parameterTypes().size())
		{
			auto const* parameterType = _funcType->parameterTypes()[i];
			auto* expectedType = _ctx.typeMapper.map(parameterType);
			arg.value = builder::ConversionPlan{
				nullptr,
				parameterType,
				expectedType,
				builder::ConversionPlan::Context::Argument}.emit(
					std::move(arg.value), _loc);
		}
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

void FunctionPointerBuilder::registerTarget(
	ContractContext& _ctx,
	FunctionDefinition const* _funcDef,
	FunctionType const* _funcType,
	std::string _awstName)
{
	auto& registry = _ctx.functionPointers;
	if (!_funcDef) return;
	if (_awstName.empty()
		&& _funcType
		&& _funcType->kind() == FunctionType::Kind::Internal)
		_funcDef = &CallResolver::resolveVirtualTarget(_ctx, *_funcDef);
	int64_t id = _funcDef->id();
	std::pair<int64_t, std::string> key{id, _awstName};
	if (registry.targets.count(key)) return; // already registered for this caller context

	std::string name = std::move(_awstName);
	if (name.empty())
		if (auto const* symbol = _ctx.functionSymbols.resolve(id))
			name = *symbol;
	if (name.empty())
		name = _funcDef->name();
	registry.targets[key] = FuncPtrEntry{
		id,
		name,
		registry.nextId++,
		_funcType,
		_funcDef,
		"" // subroutineId — populated later via setSubroutineId
	};
}

void FunctionPointerBuilder::setSubroutineIds(
	ContractContext& _ctx,
	FunctionSymbolTable const& _symbols)
{
	for (auto& [key, entry] : _ctx.functionPointers.targets)
	{
		if (auto const hostBound = _ctx.internalizedFunctionNames.find(key.first);
			hostBound != _ctx.internalizedFunctionNames.end())
		{
			entry.name = hostBound->second;
			entry.subroutineId.clear();
			continue;
		}
		if (auto const* symbol = _symbols.resolve(key.first))
		{
			if (_symbols.isRootSubroutine(key.first))
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
	registerTarget(_ctx, _funcDef, funcType, _awstName);

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
			// Cross-contract: application → itob(u64); address → last 8 bytes
			// (round-trips with .address, which pads appId to 32 bytes).
			auto addr = _receiverAddress;
			if (addr->wtype == awst::WType::applicationType())
			{
				auto toU64 = awst::makeAsUInt64(std::move(addr), _loc);
				appIdBytes = awst::makeItob(std::move(toU64), _loc);
			}
			else
			{
				if (addr->wtype != awst::WType::bytesType())
					addr = awst::makeAsBytes(std::move(addr), _loc);
				appIdBytes = awst::makeExtract(std::move(addr), 24, 8, _loc);
			}

			// Routing selector: used as ApplicationArgs[0].
			auto selectorConst = awst::makeMethodConstant(
				InnerCallHandlers::buildMethodSelector(_ctx, _funcDef),
				awst::WType::bytesType(), _loc);
			routeSelector = std::move(selectorConst);
		}
		else
		{
			Logger::instance().warning(
				"external function pointer '" + _funcDef->name()
				+ "': reentrancy is not possible on AVM; self-calls will use "
				"internal dispatch instead of inner transactions", _loc);

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

	// Internal: return the function's unique ID
	auto const* targetFunc = _funcDef;
	if (_awstName.empty())
		targetFunc = &CallResolver::resolveVirtualTarget(_ctx, *_funcDef);
	auto const& targets = _ctx.functionPointers.targets;
	auto it = targets.find({targetFunc->id(), _awstName});
	unsigned funcId = (it != targets.end()) ? it->second.id : 0;

	auto idConst = awst::makeIntegerConstant(funcId, _loc);
	return idConst;
}

// ── Build a call through a function pointer ──

std::shared_ptr<awst::Expression> FunctionPointerBuilder::buildFunctionPointerCall(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _ptrExpr,
	FunctionType const* _funcType,
	std::vector<std::shared_ptr<awst::Expression>> _args,
	awst::SourceLocation const& _loc)
{
	if (!_funcType)
		return nullptr;

	bool isExternal = isExternalFunctionPointer(_funcType);

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

		auto const routeSelectorOffset = static_cast<int>(
			externalFunctionPointerRouteSelectorOffset(
				_ctx.typeMapper.profile()));

		// Map the routing selector → internal id via __sel_to_id_<sig>.
		std::string selToIdName = "__sel_to_id_" + dispatchName(_funcType);
		auto& registry = _ctx.functionPointers;
		std::string const dname = dispatchName(_funcType);
		registry.neededDispatches[dname] = _funcType;
		bool const rootContext = inLibraryContext(_ctx, registry.currentCref);
		if (rootContext)
			registry.neededRootDispatches.insert(dname);

		awst::SubroutineTarget selToIdTarget = rootContext
			? awst::SubroutineTarget{awst::SubroutineID{registry.currentCref + "." + selToIdName}}
			: awst::SubroutineTarget{awst::InstanceMethodTarget{selToIdName}};
		auto selToIdCall = awst::makeSubroutineCall(
			std::move(selToIdTarget), awst::WType::uint64Type(), _loc);
		awst::pushCallArg(selToIdCall->args, "__sel",
			extractSlice(routeSelectorOffset, 4));

		auto selfCall = buildDispatchCall(_ctx, _funcType, std::move(selToIdCall), _args, _loc);
		awst::WType const* retType = selfCall->wtype;

		// Cross-contract: the explicit ARC-4 field → ApplicationArgs[0].
		auto sel4 = extractSlice(routeSelectorOffset, 4);

		auto argsTuple = awst::makeTupleExpression(nullptr, _loc);
		argsTuple->items.push_back(std::move(sel4));
		for (size_t i = 0; i < _args.size(); ++i)
		{
			solidity::frontend::Type const* paramSolType =
				i < _funcType->parameterTypes().size() ? _funcType->parameterTypes()[i] : nullptr;
			// THE shared arg encoder (declared-width biguint, sign-correct,
			// arrays/structs ARC4-encoded). The private encodeArgForInnerTxn
			// copy had drifted: negative sub-256 signed args zero-extended
			// (callee len-assert revert) and aggregates skipped ARC4 entirely
			// — the fourth copy the AbiCodec consolidation missed.
			argsTuple->items.push_back(
				InnerCallHandlers::encodeArgToBytes(
					_ctx, _args[i], nullptr, paramSolType, _loc));
		}
		{
			std::vector<awst::WType const*> argTypes;
			for (auto const& item : argsTuple->items)
				argTypes.push_back(item->wtype);
			argsTuple->wtype = _ctx.typeMapper.createType<awst::WTuple>(std::move(argTypes), std::nullopt);
		}

		static awst::WInnerTransactionFields s_applFieldsType(6); // TxnTypeAppl
		auto create = awst::makeCreateInnerTransaction(&s_applFieldsType, _loc);
		create->fields["TypeEnum"] = awst::makeIntegerConstant("6", _loc);
		create->fields["Fee"] = awst::makeZero(_loc);
		// ApplicationID: reinterpret uint64 appId to application type
		create->fields["ApplicationID"] = awst::makeAsApplication(extractU64(0), _loc);
		create->fields["OnCompletion"] = awst::makeZero(_loc);
		create->fields["ApplicationArgs"] = std::move(argsTuple);

		static awst::WInnerTransaction s_applTxnType(6);
		auto submit = awst::makeSubmitInnerTransaction(&s_applTxnType, _loc);
		submit->itxns.push_back(std::move(create));

		// Decode LastLog: strip 4-byte ARC4 return prefix, coerce to retType.
		auto buildInnerTxnResult = [&]() -> std::shared_ptr<awst::Expression> {
			auto readLog = awst::makeItxn("LastLog", awst::WType::bytesType(), _loc);
			auto strip = awst::makeExtract(std::move(readLog), 4, 0, _loc); // len=0 = extract to end
			if (retType == awst::WType::bytesType() || retType == awst::WType::voidType())
				return strip;
			if (retType == awst::WType::uint64Type())
				return awst::makeBtoi(std::move(strip), _loc);
			if (retType == awst::WType::boolType())
			{
				// ARC4 bool: byte 0's top bit set → true.
				auto getbit = awst::makeGetbit(
					std::move(strip), awst::makeZero(_loc), _loc);
				return awst::makeNumericCompare(
					std::move(getbit), awst::NumericComparison::Ne,
					awst::makeIntegerConstant("0", _loc), _loc);
			}
			return awst::makeReinterpretCast(std::move(strip), retType, _loc);
		};

		auto ifStmt = awst::makeIfElse(isSelf, awst::makeBlock(_loc), awst::makeBlock(_loc), _loc);

		if (retType == awst::WType::voidType())
		{
			ifStmt->ifBranch->body.push_back(awst::makeExpressionStatement(selfCall, _loc));
			ifStmt->elseBranch->body.push_back(awst::makeExpressionStatement(submit, _loc));
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
		ifStmt->elseBranch->body.push_back(awst::makeExpressionStatement(submit, _loc));
		ifStmt->elseBranch->body.push_back(writeTmp(buildInnerTxnResult()));
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

std::vector<awst::ContractMethod> FunctionPointerBuilder::generateDispatchMethods(
	ContractContext& _ctx,
	std::string const& _cref,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Subroutine>>* _outRootSubs)
{
	std::vector<awst::ContractMethod> methods;
	auto const& registry = _ctx.functionPointers;

	if (registry.neededDispatches.empty())
		return methods;

	auto funcScopeContract = [](FunctionDefinition const* fd) -> ContractDefinition const* {
		return fd ? fd->annotation().contract : nullptr;
	};
	// Extract contract name from _cref (last "."-separated segment).
	std::string contractName;
	auto dotPos = _cref.find_last_of('.');
	if (dotPos != std::string::npos)
		contractName = _cref.substr(dotPos + 1);

	std::map<std::string, std::vector<FuncPtrEntry const*>> groups;
	for (auto const& [key, entry] : registry.targets)
	{
		std::string dname = dispatchName(entry.funcType);
		// Taking a function's address does not require a dispatcher. A dynamic
		// call or external self-call records the signature in neededDispatches.
		if (!registry.neededDispatches.count(dname))
			continue;
		// Skip foreign targets (different non-library, non-base contract).
		// Keep if: awstName starts with __super_, subroutineId is set,
		// or the function is in the current contract or a library.
		auto const* fdContract = funcScopeContract(entry.funcDef);
		bool foreignNonResolvable = fdContract
			&& !contractName.empty()
			&& fdContract->name() != contractName
			&& !fdContract->isLibrary()
			&& entry.subroutineId.empty()
			&& entry.name.find("__super_") == std::string::npos;
		if (foreignNonResolvable)
		{
			// Internal/private base-contract fn: keep — InstanceMethodTarget resolves via MRO.
			if (!entry.funcDef->isPartOfExternalInterface())
				foreignNonResolvable = false;
		}
		if (foreignNonResolvable)
			continue;
		groups[dname].push_back(&entry);
	}
	// Ensure needed signatures have entries, even if empty.
	for (auto const& [dname, funcType] : registry.neededDispatches)
	{
		if (groups.find(dname) == groups.end())
			groups[dname] = {};
	}

	for (auto const& [dname, entries] : groups)
	{
		FunctionType const* funcType = nullptr;
		if (!entries.empty())
			funcType = entries[0]->funcType;
		else if (registry.neededDispatches.count(dname))
			funcType = registry.neededDispatches.at(dname);
		if (!funcType) continue;

		awst::ContractMethod dispatch;
		dispatch.sourceLocation = _loc;
		dispatch.cref = _cref;
		dispatch.memberName = dname;
		dispatch.arc4MethodConfig = std::nullopt;
		dispatch.pure = false;

		// Return type: the SAME native mapping the call site uses
		// (computeReturnType — single native type or WTuple for multi).
		// The old mapDispatchType drifted from the call site (public signed
		// ≤64 promoted to biguint vs uint64 at the call; multi-return was a
		// silent void). Public targets return WIRE-encoded values — the
		// per-entry body below adapts them back to the native return.
		dispatch.returnType = computeReturnType(_ctx, funcType);

		// Args: __funcptr_id first, then __static, then function params.
		{
			awst::SubroutineArgument idArg;
			idArg.name = "__funcptr_id";
			idArg.wtype = awst::WType::uint64Type();
			idArg.sourceLocation = _loc;
			dispatch.args.push_back(idArg);
		}
		{
			awst::SubroutineArgument stArg;
			stArg.name = "__static";
			stArg.wtype = awst::WType::uint64Type();
			stArg.sourceLocation = _loc;
			dispatch.args.push_back(stArg);
		}
		for (size_t i = 0; i < funcType->parameterTypes().size(); ++i)
		{
			awst::SubroutineArgument arg;
			arg.name = "__arg" + std::to_string(i);
			// The SAME native mapping the call site coerces to — the old
			// mapDispatchType sent address/enum/struct/non-byte-array params
			// to biguint while the call site passed account/uint64/array
			// wtypes.
			arg.wtype = _ctx.typeMapper.map(funcType->parameterTypes()[i]);
			arg.sourceLocation = _loc;
			dispatch.args.push_back(arg);
		}

		auto body = awst::makeBlock(_loc);

		// Build if/else chain; innermost = assert(false) for invalid id.
		auto defaultBlock = awst::makeBlock(_loc);
		{
			auto stmt = awst::makeExpressionStatement(awst::makeAssert(
				awst::makeFalse(_loc), _loc, "invalid function pointer"), _loc);
			defaultBlock->body.push_back(std::move(stmt));
		}

		std::shared_ptr<awst::Block> elseBlock = defaultBlock;

		for (auto const* entry : entries)
		{
			// Public multi-return target: the wire-tuple decode is
			// unimplemented. Merely REGISTERING such a pointer (.selector /
			// .address accessors) must not fail the compile — skip the entry
			// so only an actual dynamic dispatch to it hits the
			// invalid-function-pointer assert (loud at runtime).
			if (entry->funcDef && entry->funcDef->isPartOfExternalInterface()
				&& dynamic_cast<awst::WTuple const*>(dispatch.returnType))
			{
				Logger::instance().warning(
					"function pointer to PUBLIC multi-return '" + entry->name
					+ "' cannot be dispatched (wire tuple decode "
					  "unimplemented); calls through this pointer will fail "
					  "at runtime.", _loc);
				continue;
			}
			auto idVar = awst::makeVarExpression("__funcptr_id", awst::WType::uint64Type(), _loc);
			auto idConst = awst::makeIntegerConstant(entry->id, _loc);
			auto cmp = awst::makeNumericCompare(std::move(idVar), awst::NumericComparison::Eq, std::move(idConst), _loc);

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
					std::move(target), dispatch.returnType, _loc);

				bool isPublic = entry->funcDef
					&& entry->funcDef->isPartOfExternalInterface();

				for (size_t i = 0; i < funcType->parameterTypes().size(); ++i)
				{
					awst::CallArg arg;
					auto var = awst::makeVarExpression("__arg" + std::to_string(i), dispatch.args[i + 2].wtype, _loc);

					// Use the target's param name; fall back to _paramN for unnamed
					// params (matches AWSTBuilder's synthesised naming).
					std::string paramName = "__arg" + std::to_string(i);
					if (entry->funcDef && i < entry->funcDef->parameters().size())
					{
						paramName = entry->funcDef->parameters()[i]->name();
						if (paramName.empty())
							paramName = "_param" + std::to_string(i);
					}

					awst::WType const* arc4Type = isPublic
						? dispatchPublicArgArc4Type(
							_ctx.typeMapper, var->wtype, funcType->parameterTypes()[i])
						: nullptr;

					if (arc4Type && arc4Type != var->wtype)
					{
						auto encode = awst::makeARC4Encode(std::move(var), arc4Type, _loc);

						arg.name = "__arc4_" + paramName;
						arg.value = std::move(encode);
					}
					else
					{
						arg.name = paramName;
						arg.value = std::move(var);
					}
					call->args.push_back(std::move(arg));
				}

				// Public targets return WIRE-encoded values (build-time
				// return encoding): adapt back to the native dispatch return.
				bool retIsSignedNarrow = false;
				if (funcType->returnParameterTypes().size() == 1)
					if (auto it = builder::SolIntType::fromSol(
							funcType->returnParameterTypes()[0]);
						it && it->isSigned && it->bits <= 64)
						retIsSignedNarrow = true;
				if (isPublic && dispatch.returnType == awst::WType::biguintType())
					call->wtype = _ctx.typeMapper.createType<awst::ARC4UIntN>(256); // arc4.uint256
				else if (isPublic && retIsSignedNarrow
					&& dispatch.returnType == awst::WType::uint64Type())
					// Signed narrow publishes as uint256 on the wire.
					call->wtype = _ctx.typeMapper.createType<awst::ARC4UIntN>(256);
				// (public multi-return handled by the entry skip above)

				if (dispatch.returnType != awst::WType::voidType())
				{
					std::shared_ptr<awst::Expression> retValue = std::move(call);
					if (isPublic && retValue->wtype != dispatch.returnType)
					{
						// ARC4 wire → biguint, then narrow to the native
						// carrier when needed (canonical 256-bit TC's low 8
						// bytes ARE the 64-bit-TC carrier for signed narrow).
						std::shared_ptr<awst::Expression> decoded =
							awst::makeARC4Decode(std::move(retValue),
								awst::WType::biguintType(), _loc);
						if (dispatch.returnType != awst::WType::biguintType())
							decoded = builder::TypeCoercion::implicitNumericCast(
								std::move(decoded), dispatch.returnType, _loc);
						retValue = std::move(decoded);
					}
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

		// __sel_to_id_<sig>(__sel: bytes) -> uint64
		// Maps a 4-byte ARC4 selector to an internal dispatch id (self-call path).
		// Always generated even for empty groups so call-site references resolve.
		{
			awst::ContractMethod selToId;
			selToId.sourceLocation = _loc;
			selToId.cref = _cref;
			selToId.memberName = "__sel_to_id_" + dname;
			selToId.arc4MethodConfig = std::nullopt;
			selToId.pure = false;
			selToId.returnType = awst::WType::uint64Type();
			{
				awst::SubroutineArgument selArg;
				selArg.name = "__sel";
				selArg.wtype = awst::WType::bytesType();
				selArg.sourceLocation = _loc;
				selToId.args.push_back(selArg);
			}

			auto selBody = awst::makeBlock(_loc);

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
				if (!entry->funcDef) continue;
				// MethodConstant = sha512_256(sig)[:4] — same as puya's router and fn-ptr slot.
				auto methodConst = awst::makeMethodConstant(
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
			selToId.body = selBody;

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
	}

	return methods;
}

} // namespace puyasol::builder::eb
