/// @file FunctionPointerBuilder.cpp
/// Implements function pointer support — dispatch tables for internal,
/// inner app calls for external.

#include "builder/itxn/FunctionPointerBuilder.h"
#include "builder/abi/AbiEncoderBuilder.h"
#include "builder/itxn/FunctionPointerDispatchTypes.h"
#include "builder/sol-types/FunctionPointerKind.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

namespace puyasol::builder::eb
{

using namespace solidity::frontend;

namespace
{


/// True iff translation is happening from a library subroutine context,
/// where InstanceMethodTarget fails ("invocation outside of a contract method").
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
	std::string dname = dispatchName(_funcType);
	s_neededDispatches[dname] = _funcType;

	awst::SubroutineTarget target = inLibraryContext(_ctx, s_currentCref)
		? awst::SubroutineTarget{awst::SubroutineID{s_currentCref + "." + dname}}
		: awst::SubroutineTarget{awst::InstanceMethodTarget{dname}};
	auto call = awst::makeSubroutineCall(
		std::move(target), computeReturnType(_ctx, _funcType), _loc);

	awst::pushCallArg(call->args, "__funcptr_id", std::move(_ptrIdExpr));

	for (size_t i = 0; i < _args.size(); ++i)
	{
		awst::CallArg arg;
		arg.name = "__arg" + std::to_string(i);
		arg.value = _args[i];
		if (i < _funcType->parameterTypes().size())
		{
			auto* expectedType = _ctx.typeMapper.map(_funcType->parameterTypes()[i]);
			if (arg.value->wtype != expectedType)
				arg.value = builder::TypeCoercion::implicitNumericCast(
					std::move(arg.value), expectedType, _loc);
		}
		call->args.push_back(std::move(arg));
	}
	return call;
}

// Static members
std::map<std::pair<int64_t, std::string>, FuncPtrEntry> FunctionPointerBuilder::s_targets;
unsigned FunctionPointerBuilder::s_nextId = 1; // 0 = zero-initialized/invalid
std::map<std::string, solidity::frontend::FunctionType const*> FunctionPointerBuilder::s_neededDispatches;
std::string FunctionPointerBuilder::s_currentCref;

void FunctionPointerBuilder::reset()
{
	s_targets.clear();
	s_nextId = 1;
	s_neededDispatches.clear();
	s_currentCref.clear();
}

// ── Type mapping ──

awst::WType const* FunctionPointerBuilder::mapFunctionType(
	FunctionType const* _funcType)
{
	if (!_funcType)
		return awst::WType::uint64Type();

	if (isExternalFunctionPointer(_funcType))
	{
		// 12 bytes: itob(appId) 8 + selector 4
		static awst::BytesWType s_extFnPtrType(12);
		return &s_extFnPtrType;
	}

	// Internal function pointers: uint64 ID
	return awst::WType::uint64Type();
}

// ── Register a function as a pointer target ──

void FunctionPointerBuilder::registerTarget(
	FunctionDefinition const* _funcDef,
	FunctionType const* _funcType,
	std::string _awstName)
{
	if (!_funcDef) return;
	int64_t id = _funcDef->id();
	std::pair<int64_t, std::string> key{id, _awstName};
	if (s_targets.count(key)) return; // already registered for this caller context

	std::string name = _awstName.empty() ? _funcDef->name() : _awstName;
	s_targets[key] = FuncPtrEntry{
		id,
		name,
		s_nextId++,
		_funcType,
		_funcDef,
		"" // subroutineId — populated later via setSubroutineId
	};
}

void FunctionPointerBuilder::setSubroutineIds(
	std::unordered_map<int64_t, std::string> const& _idMap)
{
	for (auto& [key, entry] : s_targets)
	{
		auto it = _idMap.find(key.first);
		if (it != _idMap.end())
			entry.subroutineId = it->second;
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

	// Use caller-provided FunctionType if available (determines
	// Internal vs External when both exist, e.g., `this.g` is External
	// even though g also has an Internal overload).
	auto const* funcType = _callerFuncType;
	if (!funcType)
	{
		funcType = _funcDef->functionType(true); // internal
		if (!funcType)
			funcType = _funcDef->functionType(false); // external
	}

	// Register as target
	registerTarget(_funcDef, funcType, _awstName);

	bool isExternal = isExternalFunctionPointer(funcType);

	if (isExternal)
	{
		// External function pointer = concat(appIdBytes[8], selectorBytes[4]) = 12 bytes.
		// `this.f` → (itob(CurrentApplicationID), f.ARC4-selector) — the dispatch
		//   site recognises the self appId and takes an internal-dispatch shortcut.
		// `C(addr).f` → (8-byte appId derived from addr, f.ARC4-selector) — inner
		//   app txn at call time.
		// Calling code compares the captured appId against CurrentApplicationID
		// (self) for the internal-dispatch shortcut; a different appId uses an
		// inner txn. (Earlier this used an itob(0) sentinel + internal-id slot;
		// that's no longer the encoding.)
		static awst::BytesWType s_bytes12(12);

		// Helper: itob(constInt) → 8 bytes.
		auto makeItobConst = [&](std::string _val) -> std::shared_ptr<awst::Expression> {
			return awst::makeItob(awst::makeIntegerConstant(std::move(_val), _loc), _loc);
		};

		std::shared_ptr<awst::Expression> appIdBytes;
		std::shared_ptr<awst::Expression> selectorBytes;

		if (_receiverAddress)
		{
			// Cross-contract: appId from receiver (application → itob(u64);
			// address → last 8 bytes of 32-byte address). The 32→8 truncation
			// round-trips with our .address accessor, which pads appId to 32
			// bytes for Solidity-test address literals.
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

			// Store the target's ARC4 method selector in the selector slot.
			// At call-time with appId != 0 we emit an inner app txn with
			// ApplicationArgs[0] = this selector.
			auto selectorConst = awst::makeMethodConstant(
				AbiEncoderBuilder::buildARC4MethodSelector(_ctx, _funcDef),
				awst::WType::bytesType(), _loc);
			selectorBytes = std::move(selectorConst);
		}
		else
		{
			Logger::instance().warning(
				"external function pointer '" + _funcDef->name()
				+ "': reentrancy is not possible on AVM; self-calls will use "
				"internal dispatch instead of inner transactions", _loc);

			// Self-reference: encode appId as the CURRENT application id (not
			// a 0 sentinel) so the pointer survives crossing contract
			// boundaries — when another contract receives the pointer it can
			// route to a normal inner txn back to us. Within our own contract
			// the dispatch site compares the captured appId against
			// `CurrentApplicationID` to take an internal-dispatch shortcut.
			if (auto const* internalFuncType = _funcDef->functionType(true))
				registerTarget(_funcDef, internalFuncType, _awstName);

			auto curApp = awst::makeGlobal(
				std::string("CurrentApplicationID"), awst::WType::uint64Type(), _loc);
			appIdBytes = awst::makeItob(std::move(curApp), _loc);
			auto selectorConst = awst::makeMethodConstant(
				AbiEncoderBuilder::buildARC4MethodSelector(_ctx, _funcDef),
				awst::WType::bytesType(), _loc);
			selectorBytes = std::move(selectorConst);
		}

		auto packed = awst::makeIntrinsicCall("concat", &s_bytes12, _loc);
		packed->stackArgs.push_back(std::move(appIdBytes));
		packed->stackArgs.push_back(std::move(selectorBytes));
		return packed;
	}

	// Internal: return the function's unique ID
	auto it = s_targets.find({_funcDef->id(), _awstName});
	unsigned funcId = (it != s_targets.end()) ? it->second.id : 0;

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
		// External function pointer call: check if self-call (appId == 0
		// sentinel) and route to internal dispatch, otherwise inner txn.

		// Local helpers: extract N bytes at offset; btoi(8-byte-slice).
		auto extractSlice = [&](int _offset, int _length) {
			return awst::makeExtract(_ptrExpr, _offset, _length, _loc);
		};
		auto extractU64 = [&](int _offset) {
			return awst::makeBtoi(extractSlice(_offset, 8), _loc);
		};

		// Check if self-call: appId == CurrentApplicationID. (Captures of
		// `this.x` encode the appId as the current app's id; a pointer
		// passed in from outside has the originating contract's id.)
		auto isSelf = awst::makeNumericCompare(
			extractU64(0), awst::NumericComparison::Eq,
			awst::makeGlobal(
				std::string("CurrentApplicationID"),
				awst::WType::uint64Type(), _loc),
			_loc);

		// Self-call path: selector slot now holds ARC4 selector (was internal
		// id; changed for `.selector` accessor consistency with cross-call).
		// Map selector → internal id via the per-signature `__sel_to_id_<sig>`
		// helper, then dispatch through the existing id-based dispatcher.
		// The helper is generated in `generateDispatchMethods`.
		std::string selToIdName = "__sel_to_id_" + dispatchName(_funcType);
		s_neededDispatches[dispatchName(_funcType)] = _funcType;

		awst::SubroutineTarget selToIdTarget = inLibraryContext(_ctx, s_currentCref)
			? awst::SubroutineTarget{awst::SubroutineID{s_currentCref + "." + selToIdName}}
			: awst::SubroutineTarget{awst::InstanceMethodTarget{selToIdName}};
		auto selToIdCall = awst::makeSubroutineCall(
			std::move(selToIdTarget), awst::WType::uint64Type(), _loc);
		awst::pushCallArg(selToIdCall->args, "__sel", extractSlice(8, 4));

		// Build internal dispatch call with the same args (shared with
		// the inner-txn branch below — hence we pass _args by value)
		auto selfCall = buildDispatchCall(_ctx, _funcType, std::move(selToIdCall), _args, _loc);
		awst::WType const* retType = selfCall->wtype;

		// ── Inner-txn (cross-contract) branch ──
		// Selector slot (bytes 8..12) = ARC4 method selector for the callee's
		// router; used as ApplicationArgs[0].
		auto sel4 = extractSlice(8, 4);

		// Build ApplicationArgs tuple: [selector, arg0_encoded, arg1_encoded, ...]
		auto argsTuple = awst::makeTupleExpression(nullptr, _loc);
		argsTuple->items.push_back(std::move(sel4));
		for (size_t i = 0; i < _args.size(); ++i)
		{
			solidity::frontend::Type const* paramSolType =
				i < _funcType->parameterTypes().size() ? _funcType->parameterTypes()[i] : nullptr;
			argsTuple->items.push_back(encodeArgForInnerTxn(_args[i], paramSolType, _loc));
		}
		// Build WTuple type
		{
			std::vector<awst::WType const*> argTypes;
			for (auto const& item : argsTuple->items)
				argTypes.push_back(item->wtype);
			argsTuple->wtype = _ctx.typeMapper.createType<awst::WTuple>(std::move(argTypes), std::nullopt);
		}

		// Build CreateInnerTransaction for application call
		static awst::WInnerTransactionFields s_applFieldsType(6); // TxnTypeAppl
		auto create = awst::makeCreateInnerTransaction(&s_applFieldsType, _loc);
		create->fields["TypeEnum"] = awst::makeIntegerConstant("6", _loc);
		create->fields["Fee"] = awst::makeZero(_loc);
		// ApplicationID: reinterpret uint64 appId to application type
		create->fields["ApplicationID"] = awst::makeAsApplication(extractU64(0), _loc);
		create->fields["OnCompletion"] = awst::makeZero(_loc);
		create->fields["ApplicationArgs"] = std::move(argsTuple);

		// Submit + read LastLog (strip 4-byte ARC4 return prefix)
		static awst::WInnerTransaction s_applTxnType(6);
		auto submit = awst::makeSubmitInnerTransaction(&s_applTxnType, _loc);
		submit->itxns.push_back(std::move(create));

		// Read itxn LastLog and coerce the ARC4-prefixed bytes to retType.
		auto buildInnerTxnResult = [&]() -> std::shared_ptr<awst::Expression> {
			auto readLog = awst::makeItxn("LastLog", awst::WType::bytesType(), _loc);
			// extract(readLog, 4, 0) — strip the 4-byte ARC4 return prefix.
			// len=0 → extract to END: strip the 4-byte ARC4 return prefix.
			auto strip = awst::makeExtract(std::move(readLog), 4, 0, _loc);
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

		// Emit `if (isSelf) selfCall else submit (and read result)`. Puts the
		// if-else into prePendingStatements; the expression this function
		// returns is the result expression the caller reads.
		auto ifStmt = awst::makeIfElse(isSelf, awst::makeBlock(_loc), awst::makeBlock(_loc), _loc);

		if (retType == awst::WType::voidType())
		{
			ifStmt->ifBranch->body.push_back(awst::makeExpressionStatement(selfCall, _loc));
			ifStmt->elseBranch->body.push_back(awst::makeExpressionStatement(submit, _loc));
			_ctx.prePendingStatements.push_back(std::move(ifStmt));
			auto vc = awst::makeVoidConstant(_loc);
			return vc;
		}

		// Non-void: spill both branches' result into a shared temp that the
		// containing expression reads.
		static int s_tmpCounter = 0;
		std::string tmpName = "__fnptr_res_" + std::to_string(++s_tmpCounter);
		auto writeTmp = [&](std::shared_ptr<awst::Expression> _val) {
			auto target = awst::makeVarExpression(tmpName, retType, _loc);
			return awst::makeAssignmentStatement(std::move(target), std::move(_val), _loc);
		};
		ifStmt->ifBranch->body.push_back(writeTmp(selfCall));
		ifStmt->elseBranch->body.push_back(awst::makeExpressionStatement(submit, _loc));
		ifStmt->elseBranch->body.push_back(writeTmp(buildInnerTxnResult()));
		_ctx.prePendingStatements.push_back(std::move(ifStmt));

		return awst::makeVarExpression(tmpName, retType, _loc);
	}

	// Internal: call __funcptr_dispatch_<signature>(id, args...)
	return buildDispatchCall(_ctx, _funcType, std::move(_ptrExpr), _args, _loc);
}

// ── Dispatch name from function type signature ──

std::string FunctionPointerBuilder::dispatchName(
	FunctionType const* _funcType)
{
	// Build a name based on param and return types
	std::string name = "__funcptr_dispatch";
	if (_funcType)
	{
		for (auto const* pt : _funcType->parameterTypes())
		{
			if (auto const* intType = dynamic_cast<IntegerType const*>(pt))
				name += "_u" + std::to_string(intType->numBits());
			else if (dynamic_cast<BoolType const*>(pt))
				name += "_bool";
			else
				name += "_x";
		}
		name += "_ret";
		for (auto const* rt : _funcType->returnParameterTypes())
		{
			if (auto const* intType = dynamic_cast<IntegerType const*>(rt))
				name += "_u" + std::to_string(intType->numBits());
			else if (dynamic_cast<BoolType const*>(rt))
				name += "_bool";
			else
				name += "_x";
		}
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

	if (s_targets.empty() && s_neededDispatches.empty())
		return methods;

	// Helper: figure out which contract a function is defined in.
	// solc's Scoper populates `annotation().contract` directly.
	auto funcScopeContract = [](FunctionDefinition const* fd) -> ContractDefinition const* {
		return fd ? fd->annotation().contract : nullptr;
	};
	// Find our current contract from _cref: last "."-separated segment.
	std::string contractName;
	auto dotPos = _cref.find_last_of('.');
	if (dotPos != std::string::npos)
		contractName = _cref.substr(dotPos + 1);

	// Group targets by dispatch name (= signature)
	std::map<std::string, std::vector<FuncPtrEntry const*>> groups;
	for (auto const& [key, entry] : s_targets)
	{
		// Skip targets that are genuinely foreign (different non-library,
		// non-base contract). Library functions are shared subroutines;
		// base-contract functions reachable via linearized inheritance (or
		// super-rewrite to \`__super_N\`) are resolvable on the caller.
		// Heuristic: keep entry if the registered awstName starts with
		// \`__super_\`, or the funcDef has a non-empty subroutineId, or
		// the contract is the current one, or it's a library.
		auto const* fdContract = funcScopeContract(entry.funcDef);
		bool foreignNonResolvable = fdContract
			&& !contractName.empty()
			&& fdContract->name() != contractName
			&& !fdContract->isLibrary()
			&& entry.subroutineId.empty()
			&& entry.name.find("__super_") == std::string::npos;
		if (foreignNonResolvable)
		{
			// Double-check: if the function's visibility is not external/public
			// (e.g. internal/private from a base contract reachable via
			// inheritance), keep it — an InstanceMethodTarget on the
			// derived contract would still resolve via MRO flattening.
			if (!entry.funcDef->isPartOfExternalInterface())
				foreignNonResolvable = false;
		}
		if (foreignNonResolvable)
			continue;
		std::string dname = dispatchName(entry.funcType);
		groups[dname].push_back(&entry);
	}
	// Ensure all needed dispatch signatures have entries (even if empty)
	for (auto const& [dname, funcType] : s_neededDispatches)
	{
		if (groups.find(dname) == groups.end())
			groups[dname] = {};
	}

	for (auto const& [dname, entries] : groups)
	{
		// Get the function type from entries or from s_neededDispatches
		FunctionType const* funcType = nullptr;
		if (!entries.empty())
			funcType = entries[0]->funcType;
		else if (s_neededDispatches.count(dname))
			funcType = s_neededDispatches.at(dname);
		if (!funcType) continue;

		awst::ContractMethod dispatch;
		dispatch.sourceLocation = _loc;
		dispatch.cref = _cref;
		dispatch.memberName = dname;
		dispatch.arc4MethodConfig = std::nullopt;
		dispatch.pure = false;

		// Return type — must match what buildFunction produces for the target.
		// Signed ≤64-bit returns are only promoted to biguint when at least one
		// entry is public/external (ARC4 boundary). With all-private entries,
		// the callees return native uint64 and the dispatcher must too —
		// otherwise the declared type won't match the returned value.
		bool anyPublic = false;
		for (auto const* entry : entries)
		{
			if (entry->funcDef && entry->funcDef->isPartOfExternalInterface())
			{
				anyPublic = true;
				break;
			}
		}
		if (funcType->returnParameterTypes().empty())
			dispatch.returnType = awst::WType::voidType();
		else if (funcType->returnParameterTypes().size() == 1)
			dispatch.returnType = mapDispatchType(
				funcType->returnParameterTypes()[0], /*_promoteSignedI64Biguint=*/anyPublic);
		else
			dispatch.returnType = awst::WType::voidType(); // TODO: tuple returns

		// Args: __funcptr_id, then the function params
		{
			awst::SubroutineArgument idArg;
			idArg.name = "__funcptr_id";
			idArg.wtype = awst::WType::uint64Type();
			idArg.sourceLocation = _loc;
			dispatch.args.push_back(idArg);
		}
		for (size_t i = 0; i < funcType->parameterTypes().size(); ++i)
		{
			awst::SubroutineArgument arg;
			arg.name = "__arg" + std::to_string(i);
			arg.wtype = mapDispatchType(
				funcType->parameterTypes()[i], /*_promoteSignedI64Biguint=*/false);
			arg.sourceLocation = _loc;
			dispatch.args.push_back(arg);
		}

		// Body: switch(__funcptr_id) { case ID1: return func1(args); ... }
		auto body = awst::makeBlock(_loc);

		// Build if/else chain (innermost = default: assert false)
		auto defaultBlock = awst::makeBlock(_loc);
		{
			// assert(false) — invalid function pointer ID
			auto stmt = awst::makeExpressionStatement(awst::makeAssert(
				awst::makeFalse(_loc), _loc, "invalid function pointer"), _loc);
			defaultBlock->body.push_back(std::move(stmt));
		}

		std::shared_ptr<awst::Block> elseBlock = defaultBlock;

		for (auto const* entry : entries)
		{
			// Condition: __funcptr_id == entry->id
			auto idVar = awst::makeVarExpression("__funcptr_id", awst::WType::uint64Type(), _loc);

			auto idConst = awst::makeIntegerConstant(entry->id, _loc);

			auto cmp = awst::makeNumericCompare(std::move(idVar), awst::NumericComparison::Eq, std::move(idConst), _loc);

			// If branch: call the actual function and return result
			auto ifBlock = awst::makeBlock(_loc);
			{
				awst::SubroutineTarget target = !entry->subroutineId.empty()
					? awst::SubroutineTarget{awst::SubroutineID{entry->subroutineId}}
					: awst::SubroutineTarget{awst::InstanceMethodTarget{entry->name}};
				auto call = awst::makeSubroutineCall(
					std::move(target), dispatch.returnType, _loc);

				// Check if target is public (has ARC4 wrapping)
				bool isPublic = entry->funcDef
					&& entry->funcDef->isPartOfExternalInterface();

				for (size_t i = 0; i < funcType->parameterTypes().size(); ++i)
				{
					awst::CallArg arg;
					auto var = awst::makeVarExpression("__arg" + std::to_string(i), dispatch.args[i + 1].wtype, _loc);

					// Get the actual parameter name from the target function.
					// If the target parameter is unnamed (e.g. `g(string) external`),
					// fall back to `_paramN` — matching how AWSTBuilder synthesises
					// names for unnamed parameters.
					std::string paramName = "__arg" + std::to_string(i);
					if (entry->funcDef && i < entry->funcDef->parameters().size())
					{
						paramName = entry->funcDef->parameters()[i]->name();
						if (paramName.empty())
							paramName = "_param" + std::to_string(i);
					}

					awst::WType const* arc4Type = isPublic
						? dispatchPublicArgArc4Type(var->wtype, funcType->parameterTypes()[i])
						: nullptr;

					if (arc4Type && arc4Type != var->wtype)
					{
						// Public target: wrap native → ARC4 type
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

				// For public targets with ARC4 return, decode the result
				if (isPublic && dispatch.returnType == awst::WType::biguintType())
				{
					call->wtype = new awst::ARC4UIntN(256); // arc4.uint256
				}

				if (dispatch.returnType != awst::WType::voidType())
				{
					std::shared_ptr<awst::Expression> retValue = std::move(call);
					// If target is public and returns ARC4, decode back to biguint
					if (isPublic && retValue->wtype != dispatch.returnType)
					{
						auto decode = awst::makeARC4Decode(std::move(retValue), dispatch.returnType, _loc);
						retValue = std::move(decode);
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

		// Also emit as a root-level Subroutine so library subroutines can
		// resolve the dispatch via SubroutineID (puya can't resolve
		// InstanceMethodTarget from outside the contract scope).
		if (_outRootSubs)
		{
			auto sub = awst::makeSubroutine(
				_cref + "." + dispatch.memberName, dispatch.memberName,
				dispatch.args, dispatch.returnType, dispatch.body /*shared*/,
				dispatch.pure, dispatch.sourceLocation);
			_outRootSubs->push_back(std::move(sub));
		}

		methods.push_back(std::move(dispatch));

		// ── Companion: `__sel_to_id_<sig>(__sel: bytes) -> uint64` ──
		// External self-call sites pass the 4-byte ARC4 selector from the
		// fn-ptr's selector slot through this helper to recover the
		// internal dispatch id. (Self-call encoding now stores the ARC4
		// selector for `.selector` accessor consistency; see the encoding
		// comment in `buildFunctionReference`.) Body is a chain of
		// `if __sel == method("sig") then return id` over the entries
		// registered for this signature group; default branch errs.
		// Always generated (even when entries is empty) so the call-site
		// reference resolves; an empty body just errs at runtime, which
		// matches "no self-call possible for this signature".
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
				// MethodConstant resolves to sha512_256(sig)[:4] at puya
				// lowering time — same value puya's router matches in the
				// approval program, and the same value we store in the
				// fn-ptr's selector slot (cross-call path). Byte equality.
				auto methodConst = awst::makeMethodConstant(
					AbiEncoderBuilder::buildARC4MethodSelector(_ctx, entry->funcDef),
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

			if (_outRootSubs)
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
