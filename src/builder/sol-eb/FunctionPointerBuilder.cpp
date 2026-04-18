/// @file FunctionPointerBuilder.cpp
/// Implements function pointer support — dispatch tables for internal,
/// inner app calls for external.

#include "builder/sol-eb/FunctionPointerBuilder.h"
#include "builder/sol-eb/AbiEncoderBuilder.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

namespace puyasol::builder::eb
{

using namespace solidity::frontend;

// Static members
std::map<int64_t, FuncPtrEntry> FunctionPointerBuilder::s_targets;
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

	if (_funcType->kind() == FunctionType::Kind::External
		|| _funcType->kind() == FunctionType::Kind::DelegateCall)
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
	if (s_targets.count(id)) return; // already registered

	std::string name = _awstName.empty() ? _funcDef->name() : std::move(_awstName);
	s_targets[id] = FuncPtrEntry{
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
	for (auto& [astId, entry] : s_targets)
	{
		auto it = _idMap.find(astId);
		if (it != _idMap.end())
			entry.subroutineId = it->second;
	}
}

// ── Build a reference to a function (taking its "address") ──

std::shared_ptr<awst::Expression> FunctionPointerBuilder::buildFunctionReference(
	BuilderContext& _ctx,
	FunctionDefinition const* _funcDef,
	awst::SourceLocation const& _loc,
	FunctionType const* _callerFuncType,
	std::shared_ptr<awst::Expression> _receiverAddress)
{
	if (!_funcDef)
	{
		// Zero-initialized function pointer
		auto zero = awst::makeIntegerConstant("0", _loc);
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
	registerTarget(_funcDef, funcType);

	bool isExternal = funcType && (funcType->kind() == FunctionType::Kind::External
		|| funcType->kind() == FunctionType::Kind::DelegateCall);

	if (isExternal)
	{
		// External function pointer = concat(itob(appId), selector).
		// `this.f` → (0 sentinel, internalFuncId).
		// `C(addr).f` → (appId from addr's last 8 bytes, f.selector).
		// The 12-byte representation (8 appId + 4 selector) is stored as
		// raw bytes. Calling it checks appId == 0 (self) for internal
		// dispatch; non-zero uses inner txn with that appId.
		static awst::BytesWType s_bytes12(12);

		if (_receiverAddress)
		{
			// Cross-contract external: derive an 8-byte appId slot from
			// the receiver. Two receiver shapes:
			//   (a) application — `new Other()` — appId is already a uint64,
			//       just itob to 8 bytes.
			//   (b) address — `C(address(N))` — take the last 8 bytes of
			//       the 32-byte address.
			// Treating the last 8 bytes of an address as appId preserves
			// round-trip through `.address` for literal address values used
			// in Solidity tests, while the application path matches the
			// runtime reality of \`new Other()\`.
			auto addr = _receiverAddress;
			std::shared_ptr<awst::Expression> appIdBytes;
			if (addr->wtype == awst::WType::applicationType())
			{
				auto toU64 = awst::makeReinterpretCast(std::move(addr), awst::WType::uint64Type(), _loc);
				auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
				itob->stackArgs.push_back(std::move(toU64));
				appIdBytes = std::move(itob);
			}
			else
			{
				if (addr->wtype != awst::WType::bytesType())
				{
					auto cast = awst::makeReinterpretCast(std::move(addr), awst::WType::bytesType(), _loc);
					addr = std::move(cast);
				}
				auto extract = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
				extract->immediates = {24, 8};
				extract->stackArgs.push_back(std::move(addr));
				appIdBytes = std::move(extract);
			}

			// Cross-contract: store the target's ARC4 method selector in
			// the selector slot. At call-time when appId != 0 we emit an
			// inner app txn with ApplicationArgs[0] = this selector.
			std::string methodSig = AbiEncoderBuilder::buildARC4MethodSelector(_ctx, _funcDef);
			auto selectorConst = std::make_shared<awst::MethodConstant>();
			selectorConst->sourceLocation = _loc;
			selectorConst->wtype = awst::WType::bytesType();
			selectorConst->value = methodSig;

			auto packed = awst::makeIntrinsicCall("concat", &s_bytes12, _loc);
			packed->stackArgs.push_back(std::move(appIdBytes));
			packed->stackArgs.push_back(std::move(selectorConst));
			return packed;
		}

		Logger::instance().warning(
			"external function pointer '" + _funcDef->name()
			+ "': reentrancy is not possible on AVM; self-calls will use "
			"internal dispatch instead of inner transactions", _loc);

		// Self-reference: encode as concat(itob(0), itob(internalId)[:4]).
		// appId=0 is a sentinel that means "current application — use
		// internal dispatch". The selector slot stores the internal fn-ptr
		// ID so the runtime dispatch can route without an inner txn.
		auto const* internalFuncType = _funcDef->functionType(true);
		if (internalFuncType)
			registerTarget(_funcDef, internalFuncType);
		auto idIt = s_targets.find(_funcDef->id());
		unsigned funcId = (idIt != s_targets.end()) ? idIt->second.id : 0;

		// itob(0) — sentinel appId
		auto zeroAppId = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
		auto zeroConst = awst::makeIntegerConstant("0", _loc);
		zeroAppId->stackArgs.push_back(std::move(zeroConst));

		// itob(funcId)[:4] — internal ID in selector slot
		auto idItob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
		auto idConst = awst::makeIntegerConstant(std::to_string(funcId), _loc);
		idItob->stackArgs.push_back(std::move(idConst));
		auto idBytes4 = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
		idBytes4->immediates = {4, 4}; // last 4 bytes of 8-byte itob
		idBytes4->stackArgs.push_back(std::move(idItob));

		// concat(itob(0), idBytes4) → 12 bytes
		auto packed = awst::makeIntrinsicCall("concat", &s_bytes12, _loc);
		packed->stackArgs.push_back(std::move(zeroAppId));
		packed->stackArgs.push_back(std::move(idBytes4));
		return packed;
	}

	// Internal: return the function's unique ID
	auto it = s_targets.find(_funcDef->id());
	unsigned funcId = (it != s_targets.end()) ? it->second.id : 0;

	auto idConst = awst::makeIntegerConstant(std::to_string(funcId), _loc);
	return idConst;
}

// ── Build a call through a function pointer ──

std::shared_ptr<awst::Expression> FunctionPointerBuilder::buildFunctionPointerCall(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> _ptrExpr,
	FunctionType const* _funcType,
	std::vector<std::shared_ptr<awst::Expression>> _args,
	awst::SourceLocation const& _loc)
{
	if (!_funcType)
		return nullptr;

	bool isExternal = (_funcType->kind() == FunctionType::Kind::External
		|| _funcType->kind() == FunctionType::Kind::DelegateCall);

	if (isExternal)
	{
		// External function pointer call: check if self-call (appId == 0
		// sentinel) and route to internal dispatch, otherwise inner txn.

		// Extract appId: btoi(extract(_ptr, 0, 8))
		auto zero = awst::makeIntegerConstant("0", _loc);
		auto eight = awst::makeIntegerConstant("8", _loc);

		auto extractAppId = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
		extractAppId->stackArgs.push_back(_ptrExpr);
		extractAppId->stackArgs.push_back(std::move(zero));
		extractAppId->stackArgs.push_back(std::move(eight));

		auto appId = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
		appId->stackArgs.push_back(std::move(extractAppId));

		// Check if self-call: appId == 0 (sentinel for current app)
		auto isSelf = std::make_shared<awst::NumericComparisonExpression>();
		isSelf->sourceLocation = _loc;
		isSelf->wtype = awst::WType::boolType();
		isSelf->lhs = appId; // shared
		isSelf->op = awst::NumericComparison::Eq;
		auto zeroCheck = awst::makeIntegerConstant("0", _loc);
		isSelf->rhs = std::move(zeroCheck);

		// Self-call path: extract internal ID from selector slot, dispatch
		auto extractId4 = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
		extractId4->immediates = {8, 4};
		extractId4->stackArgs.push_back(_ptrExpr);

		auto internalId = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
		internalId->stackArgs.push_back(std::move(extractId4));

		// Build internal dispatch call with the same args
		std::string dname = dispatchName(_funcType);
		s_neededDispatches[dname] = _funcType;

		awst::WType const* retType = awst::WType::voidType();
		if (!_funcType->returnParameterTypes().empty())
		{
			if (_funcType->returnParameterTypes().size() == 1)
				retType = _ctx.typeMapper.map(_funcType->returnParameterTypes()[0]);
			else
			{
				std::vector<awst::WType const*> retTypes;
				for (auto const* rt : _funcType->returnParameterTypes())
					retTypes.push_back(_ctx.typeMapper.map(rt));
				retType = _ctx.typeMapper.createType<awst::WTuple>(std::move(retTypes), std::nullopt);
			}
		}

		auto selfCall = std::make_shared<awst::SubroutineCallExpression>();
		selfCall->sourceLocation = _loc;
		selfCall->wtype = retType;
		bool inLibraryContext = !_ctx.contractName.empty()
			&& !s_currentCref.empty()
			&& s_currentCref.find("." + _ctx.contractName) == std::string::npos;
		if (inLibraryContext)
			selfCall->target = awst::SubroutineID{s_currentCref + "." + dname};
		else
			selfCall->target = awst::InstanceMethodTarget{dname};

		awst::CallArg selfIdArg;
		selfIdArg.name = "__funcptr_id";
		selfIdArg.value = std::move(internalId);
		selfCall->args.push_back(std::move(selfIdArg));
		for (size_t i = 0; i < _args.size(); ++i)
		{
			awst::CallArg arg;
			arg.name = "__arg" + std::to_string(i);
			arg.value = _args[i]; // shared — used by both branches
			if (i < _funcType->parameterTypes().size())
			{
				auto* expectedType = _ctx.typeMapper.map(_funcType->parameterTypes()[i]);
				if (arg.value->wtype != expectedType)
					arg.value = builder::TypeCoercion::implicitNumericCast(
						std::move(arg.value), expectedType, _loc);
			}
			selfCall->args.push_back(std::move(arg));
		}

		// ── Inner-txn (cross-contract) branch ──
		// Extract selector slot (bytes 8..12) — this was populated at
		// fn-ref construction with either the internal fn-ptr id (self)
		// or the target's ARC4 method selector (cross-contract). The
		// router on the callee expects ARC4 selector at ApplicationArgs[0].
		auto sel4 = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
		sel4->immediates = {8, 4};
		sel4->stackArgs.push_back(_ptrExpr);

		// Build ApplicationArgs tuple: [selector, arg0_encoded, arg1_encoded, ...]
		auto argsTuple = std::make_shared<awst::TupleExpression>();
		argsTuple->sourceLocation = _loc;
		argsTuple->items.push_back(std::move(sel4));
		for (size_t i = 0; i < _args.size(); ++i)
		{
			auto argExpr = _args[i]; // shared with self-call branch
			solidity::frontend::Type const* paramSolType = i < _funcType->parameterTypes().size()
				? _funcType->parameterTypes()[i] : nullptr;
			// Simple ABI encoding: uint64 → itob, biguint → reinterpret to bytes
			// (already 32 bytes), bool → 1 byte, bytes/string → length-prefixed,
			// anything else → pass-through.
			// Target ARC4 width for integer types: uint256 → 32 bytes, uint128 → 16, etc.
			unsigned targetBits = 256;
			if (paramSolType)
				if (auto const* intType = dynamic_cast<IntegerType const*>(paramSolType))
					targetBits = intType->numBits();
			unsigned targetBytes = targetBits / 8;

			std::shared_ptr<awst::Expression> encoded;
			if (argExpr->wtype == awst::WType::uint64Type())
			{
				auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
				itob->stackArgs.push_back(std::move(argExpr));
				std::shared_ptr<awst::Expression> bytesExpr = std::move(itob);
				if (targetBytes > 8 && paramSolType && dynamic_cast<IntegerType const*>(paramSolType))
				{
					// Left-pad itob(8) to targetBytes via `b| bzero(targetBytes)`.
					auto bzero = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
					bzero->stackArgs.push_back(awst::makeIntegerConstant(std::to_string(targetBytes), _loc));
					auto orOp = awst::makeIntrinsicCall("b|", awst::WType::bytesType(), _loc);
					orOp->stackArgs.push_back(std::move(bytesExpr));
					orOp->stackArgs.push_back(std::move(bzero));
					bytesExpr = std::move(orOp);
				}
				encoded = std::move(bytesExpr);
			}
			else if (argExpr->wtype == awst::WType::biguintType())
			{
				auto raw = awst::makeReinterpretCast(std::move(argExpr), awst::WType::bytesType(), _loc);
				if (paramSolType && dynamic_cast<IntegerType const*>(paramSolType))
				{
					// Biguint can be shorter than targetBytes; left-pad via b| bzero.
					auto bzero = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
					bzero->stackArgs.push_back(awst::makeIntegerConstant(std::to_string(targetBytes), _loc));
					auto orOp = awst::makeIntrinsicCall("b|", awst::WType::bytesType(), _loc);
					orOp->stackArgs.push_back(std::move(raw));
					orOp->stackArgs.push_back(std::move(bzero));
					encoded = std::move(orOp);
				}
				else
				{
					encoded = std::move(raw);
				}
			}
			else if (argExpr->wtype == awst::WType::boolType())
			{
				// ARC4 bool: 1 byte, 0x80 = true, 0x00 = false.
				auto setbit = awst::makeIntrinsicCall("setbit", awst::WType::bytesType(), _loc);
				std::vector<uint8_t> zero1{0};
				setbit->stackArgs.push_back(awst::makeBytesConstant(std::move(zero1), _loc));
				setbit->stackArgs.push_back(awst::makeIntegerConstant("0", _loc));
				setbit->stackArgs.push_back(std::move(argExpr));
				encoded = std::move(setbit);
			}
			else if (paramSolType && dynamic_cast<ArrayType const*>(paramSolType)
				&& dynamic_cast<ArrayType const*>(paramSolType)->isByteArrayOrString())
			{
				// ARC4 byte[] encoding: uint16(length) ++ raw_bytes.
				if (argExpr->wtype != awst::WType::bytesType())
				{
					auto cast = awst::makeReinterpretCast(std::move(argExpr), awst::WType::bytesType(), _loc);
					argExpr = std::move(cast);
				}
				auto lenExpr = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
				lenExpr->stackArgs.push_back(argExpr);
				auto itobLen = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
				itobLen->stackArgs.push_back(std::move(lenExpr));
				auto header = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
				header->immediates = {6, 2};
				header->stackArgs.push_back(std::move(itobLen));
				auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
				concat->stackArgs.push_back(std::move(header));
				concat->stackArgs.push_back(std::move(argExpr));
				encoded = std::move(concat);
			}
			else
			{
				// Fallback: reinterpret as bytes.
				if (argExpr->wtype != awst::WType::bytesType())
					encoded = awst::makeReinterpretCast(std::move(argExpr), awst::WType::bytesType(), _loc);
				else
					encoded = std::move(argExpr);
			}
			argsTuple->items.push_back(std::move(encoded));
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
		auto create = std::make_shared<awst::CreateInnerTransaction>();
		create->sourceLocation = _loc;
		create->wtype = &s_applFieldsType;
		create->fields["TypeEnum"] = awst::makeIntegerConstant("6", _loc);
		create->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
		{
			// ApplicationID: reinterpret uint64 appId to application type
			auto appIdCopy = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
			appIdCopy->immediates = {0, 8};
			appIdCopy->stackArgs.push_back(_ptrExpr);
			auto appIdBtoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
			appIdBtoi->stackArgs.push_back(std::move(appIdCopy));
			auto appIdApp = awst::makeReinterpretCast(std::move(appIdBtoi), awst::WType::applicationType(), _loc);
			create->fields["ApplicationID"] = std::move(appIdApp);
		}
		create->fields["OnCompletion"] = awst::makeIntegerConstant("0", _loc);
		create->fields["ApplicationArgs"] = std::move(argsTuple);

		// Submit + read LastLog (strip 4-byte ARC4 return prefix)
		static awst::WInnerTransaction s_applTxnType(6);
		auto submit = std::make_shared<awst::SubmitInnerTransaction>();
		submit->sourceLocation = _loc;
		submit->wtype = &s_applTxnType;
		submit->itxns.push_back(std::move(create));

		// Build the inner-txn result expression (coerced to retType)
		auto buildInnerTxnResult = [&]() -> std::shared_ptr<awst::Expression> {
			auto readLog = awst::makeIntrinsicCall("itxn", awst::WType::bytesType(), _loc);
			readLog->immediates = {std::string("LastLog")};
			auto strip = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
			strip->immediates = {4, 0};
			strip->stackArgs.push_back(std::move(readLog));
			if (retType == awst::WType::bytesType() || retType == awst::WType::voidType())
				return strip;
			if (retType == awst::WType::biguintType())
				return awst::makeReinterpretCast(std::move(strip), awst::WType::biguintType(), _loc);
			if (retType == awst::WType::uint64Type())
			{
				auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
				btoi->stackArgs.push_back(std::move(strip));
				return btoi;
			}
			if (retType == awst::WType::boolType())
			{
				auto getbit = awst::makeIntrinsicCall("getbit", awst::WType::uint64Type(), _loc);
				getbit->stackArgs.push_back(std::move(strip));
				getbit->stackArgs.push_back(awst::makeIntegerConstant("0", _loc));
				auto cmp = awst::makeNumericCompare(std::move(getbit), awst::NumericComparison::Ne, awst::makeIntegerConstant("0", _loc), _loc);
				return cmp;
			}
			if (retType == awst::WType::stringType())
				return awst::makeReinterpretCast(std::move(strip), awst::WType::stringType(), _loc);
			if (retType == awst::WType::accountType())
				return awst::makeReinterpretCast(std::move(strip), awst::WType::accountType(), _loc);
			return awst::makeReinterpretCast(std::move(strip), retType, _loc);
		};

		// Void return: emit both branches as statements (no temp).
		if (retType == awst::WType::voidType())
		{
			auto ifBlock = std::make_shared<awst::Block>();
			ifBlock->sourceLocation = _loc;
			ifBlock->body.push_back(awst::makeExpressionStatement(selfCall, _loc));

			auto elseBlock = std::make_shared<awst::Block>();
			elseBlock->sourceLocation = _loc;
			elseBlock->body.push_back(awst::makeExpressionStatement(submit, _loc));

			auto ifStmt = std::make_shared<awst::IfElse>();
			ifStmt->sourceLocation = _loc;
			ifStmt->condition = isSelf;
			ifStmt->ifBranch = std::move(ifBlock);
			ifStmt->elseBranch = std::move(elseBlock);
			_ctx.prePendingStatements.push_back(std::move(ifStmt));

			auto vc = std::make_shared<awst::VoidConstant>();
			vc->sourceLocation = _loc;
			vc->wtype = awst::WType::voidType();
			return vc;
		}

		// Non-void: spill both branches' result into a shared temp; the
		// containing expression reads the temp.
		static int s_tmpCounter = 0;
		std::string tmpName = "__fnptr_res_" + std::to_string(++s_tmpCounter);

		auto ifBlock = std::make_shared<awst::Block>();
		ifBlock->sourceLocation = _loc;
		{
			auto tmpTarget = awst::makeVarExpression(tmpName, retType, _loc);
			ifBlock->body.push_back(awst::makeAssignmentStatement(tmpTarget, selfCall, _loc));
		}

		auto elseBlock = std::make_shared<awst::Block>();
		elseBlock->sourceLocation = _loc;
		{
			elseBlock->body.push_back(awst::makeExpressionStatement(submit, _loc));
			auto tmpTarget = awst::makeVarExpression(tmpName, retType, _loc);
			elseBlock->body.push_back(awst::makeAssignmentStatement(tmpTarget, buildInnerTxnResult(), _loc));
		}

		auto ifStmt = std::make_shared<awst::IfElse>();
		ifStmt->sourceLocation = _loc;
		ifStmt->condition = isSelf;
		ifStmt->ifBranch = std::move(ifBlock);
		ifStmt->elseBranch = std::move(elseBlock);
		_ctx.prePendingStatements.push_back(std::move(ifStmt));

		return awst::makeVarExpression(tmpName, retType, _loc);

	}

	// Internal: call __funcptr_dispatch_<signature>(id, args...)
	std::string dname = dispatchName(_funcType);
	// Track that this dispatch signature is needed (even if no targets registered)
	s_neededDispatches[dname] = _funcType;

	// Determine return type
	awst::WType const* retType = awst::WType::voidType();
	if (!_funcType->returnParameterTypes().empty())
	{
		if (_funcType->returnParameterTypes().size() == 1)
			retType = _ctx.typeMapper.map(_funcType->returnParameterTypes()[0]);
		else
		{
			std::vector<awst::WType const*> retTypes;
			for (auto const* rt : _funcType->returnParameterTypes())
				retTypes.push_back(_ctx.typeMapper.map(rt));
			retType = _ctx.typeMapper.createType<awst::WTuple>(std::move(retTypes), std::nullopt);
		}
	}

	auto call = std::make_shared<awst::SubroutineCallExpression>();
	call->sourceLocation = _loc;
	call->wtype = retType;
	// Use SubroutineID from library context (where InstanceMethodTarget
	// fails with "invocation outside of a contract method"). From contract
	// context, InstanceMethodTarget resolves correctly.
	bool inLibraryContext = !_ctx.contractName.empty()
		&& !s_currentCref.empty()
		&& s_currentCref.find("." + _ctx.contractName) == std::string::npos;
	if (inLibraryContext)
		call->target = awst::SubroutineID{s_currentCref + "." + dname};
	else
		call->target = awst::InstanceMethodTarget{dname};

	// First arg: the pointer ID
	awst::CallArg idArg;
	idArg.name = "__funcptr_id";
	idArg.value = std::move(_ptrExpr);
	call->args.push_back(std::move(idArg));

	// Remaining args: coerce to match dispatch parameter types
	for (size_t i = 0; i < _args.size(); ++i)
	{
		awst::CallArg arg;
		arg.name = "__arg" + std::to_string(i);
		arg.value = std::move(_args[i]);
		// Coerce arg type to match dispatch parameter type
		if (i < _funcType->parameterTypes().size())
		{
			auto const* paramSolType = _funcType->parameterTypes()[i];
			awst::WType const* expectedType = _ctx.typeMapper.map(paramSolType);
			if (arg.value->wtype != expectedType)
				arg.value = builder::TypeCoercion::implicitNumericCast(
					std::move(arg.value), expectedType, _loc);
		}
		call->args.push_back(std::move(arg));
	}

	return call;
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
	std::string const& _cref,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Subroutine>>* _outRootSubs)
{
	std::vector<awst::ContractMethod> methods;

	if (s_targets.empty() && s_neededDispatches.empty())
		return methods;

	// Helper: figure out which contract a function is defined in.
	auto funcScopeContract = [](FunctionDefinition const* fd) -> ContractDefinition const* {
		if (!fd) return nullptr;
		auto const* scope = fd->scope();
		return dynamic_cast<ContractDefinition const*>(scope);
	};
	// Find our current contract from _cref: last "."-separated segment.
	std::string contractName;
	auto dotPos = _cref.find_last_of('.');
	if (dotPos != std::string::npos)
		contractName = _cref.substr(dotPos + 1);

	// Group targets by dispatch name (= signature)
	std::map<std::string, std::vector<FuncPtrEntry const*>> groups;
	for (auto const& [astId, entry] : s_targets)
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
			&& entry.name.find("__super_") != 0;
		if (foreignNonResolvable)
		{
			// Double-check: if the function's visibility is not external/public
			// (e.g. internal/private from a base contract reachable via
			// inheritance), keep it — an InstanceMethodTarget on the
			// derived contract would still resolve via MRO flattening.
			auto vis = entry.funcDef->visibility();
			if (vis != Visibility::External && vis != Visibility::Public)
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
		// Signed integers ≤64 bits get promoted to biguint for ABI sign extension.
		if (funcType->returnParameterTypes().empty())
			dispatch.returnType = awst::WType::voidType();
		else if (funcType->returnParameterTypes().size() == 1)
		{
			auto const* retSolType = funcType->returnParameterTypes()[0];
			if (auto const* intType = dynamic_cast<IntegerType const*>(retSolType))
			{
				if (intType->numBits() <= 64 && intType->isSigned())
					dispatch.returnType = awst::WType::biguintType(); // sign-extended
				else if (intType->numBits() <= 64)
					dispatch.returnType = awst::WType::uint64Type();
				else
					dispatch.returnType = awst::WType::biguintType();
			}
			else if (dynamic_cast<BoolType const*>(retSolType))
				dispatch.returnType = awst::WType::boolType();
			else if (auto const* arrType = dynamic_cast<ArrayType const*>(retSolType))
			{
				if (arrType->isString())
					dispatch.returnType = awst::WType::stringType();
				else if (arrType->isByteArray())
					dispatch.returnType = awst::WType::bytesType();
				else
					dispatch.returnType = awst::WType::biguintType();
			}
			else
				dispatch.returnType = awst::WType::biguintType();
		}
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
			auto const* paramSolType = funcType->parameterTypes()[i];
			if (auto const* intType = dynamic_cast<IntegerType const*>(paramSolType))
				arg.wtype = intType->numBits() <= 64
					? awst::WType::uint64Type() : awst::WType::biguintType();
			else if (dynamic_cast<BoolType const*>(paramSolType))
				arg.wtype = awst::WType::boolType();
			else if (auto const* arrType = dynamic_cast<ArrayType const*>(paramSolType))
			{
				if (arrType->isString())
					arg.wtype = awst::WType::stringType();
				else if (arrType->isByteArray())
					arg.wtype = awst::WType::bytesType();
				else
					arg.wtype = awst::WType::biguintType();
			}
			else if (paramSolType && paramSolType->category() == Type::Category::StringLiteral)
				arg.wtype = awst::WType::stringType();
			else if (auto const* fnType = dynamic_cast<FunctionType const*>(paramSolType))
				arg.wtype = mapFunctionType(fnType);
			else if (auto const* fbType = dynamic_cast<solidity::frontend::FixedBytesType const*>(paramSolType))
				arg.wtype = new awst::BytesWType(static_cast<int>(fbType->numBytes()));
			else
				arg.wtype = awst::WType::biguintType();
			arg.sourceLocation = _loc;
			dispatch.args.push_back(arg);
		}

		// Body: switch(__funcptr_id) { case ID1: return func1(args); ... }
		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = _loc;

		// Build if/else chain (innermost = default: assert false)
		auto defaultBlock = std::make_shared<awst::Block>();
		defaultBlock->sourceLocation = _loc;
		{
			// assert(false) — invalid function pointer ID
			auto stmt = awst::makeExpressionStatement(awst::makeAssert(
				awst::makeBoolConstant(false, _loc), _loc, "invalid function pointer"), _loc);
			defaultBlock->body.push_back(std::move(stmt));
		}

		std::shared_ptr<awst::Block> elseBlock = defaultBlock;

		for (auto const* entry : entries)
		{
			// Condition: __funcptr_id == entry->id
			auto idVar = awst::makeVarExpression("__funcptr_id", awst::WType::uint64Type(), _loc);

			auto idConst = awst::makeIntegerConstant(std::to_string(entry->id), _loc);

			auto cmp = awst::makeNumericCompare(std::move(idVar), awst::NumericComparison::Eq, std::move(idConst), _loc);

			// If branch: call the actual function and return result
			auto ifBlock = std::make_shared<awst::Block>();
			ifBlock->sourceLocation = _loc;
			{
				auto call = std::make_shared<awst::SubroutineCallExpression>();
				call->sourceLocation = _loc;
				call->wtype = dispatch.returnType;
				if (!entry->subroutineId.empty())
					call->target = awst::SubroutineID{entry->subroutineId};
				else
					call->target = awst::InstanceMethodTarget{entry->name};

				// Check if target is public (has ARC4 wrapping)
				bool isPublic = entry->funcDef && (
					entry->funcDef->visibility() == Visibility::Public
					|| entry->funcDef->visibility() == Visibility::External);

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

					awst::WType const* arc4Type = nullptr;
					if (isPublic)
					{
						auto const* paramSolType = funcType->parameterTypes()[i];
						if (var->wtype == awst::WType::biguintType())
						{
							// Biguint: preserve bit width from Solidity int type
							unsigned bits = 256;
							if (auto const* intType = dynamic_cast<IntegerType const*>(paramSolType))
								bits = intType->numBits();
							arc4Type = new awst::ARC4UIntN(static_cast<int>(bits));
						}
						else if (var->wtype && var->wtype->kind() == awst::WTypeKind::Bytes
							&& dynamic_cast<FunctionType const*>(paramSolType))
						{
							// External fn-ptr bytes[12] → arc4.static_array<arc4.uint8, 12>.
							// ContractBuilder only ARC4-remaps bytes[N] params when the
							// Solidity type is FunctionType (see ContractBuilder.cpp
							// isAggregate check); matching that rule here so the
							// dispatch call-site wraps iff the target's signature
							// expects an ARC4 arg.
							auto const* bytesType = static_cast<awst::BytesWType const*>(var->wtype);
							if (bytesType->length().has_value())
							{
								auto const* arc4Byte = new awst::ARC4UIntN(8);
								arc4Type = new awst::ARC4StaticArray(arc4Byte, bytesType->length().value());
							}
						}
					}

					if (isPublic && arc4Type && arc4Type != var->wtype)
					{
						// Public target: wrap native → ARC4 type
						auto encode = std::make_shared<awst::ARC4Encode>();
						encode->sourceLocation = _loc;
						encode->wtype = arc4Type;
						encode->value = std::move(var);

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
						auto decode = std::make_shared<awst::ARC4Decode>();
						decode->sourceLocation = _loc;
						decode->wtype = dispatch.returnType;
						decode->value = std::move(retValue);
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

			auto ifElse = std::make_shared<awst::IfElse>();
			ifElse->sourceLocation = _loc;
			ifElse->condition = std::move(cmp);
			ifElse->ifBranch = std::move(ifBlock);
			ifElse->elseBranch = std::move(elseBlock);

			auto newElse = std::make_shared<awst::Block>();
			newElse->sourceLocation = _loc;
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
			auto sub = std::make_shared<awst::Subroutine>();
			sub->sourceLocation = dispatch.sourceLocation;
			sub->id = _cref + "." + dispatch.memberName;
			sub->name = dispatch.memberName;
			sub->returnType = dispatch.returnType;
			sub->args = dispatch.args;
			sub->body = dispatch.body; // shared ptr — same body
			sub->pure = dispatch.pure;
			_outRootSubs->push_back(std::move(sub));
		}

		methods.push_back(std::move(dispatch));
	}

	return methods;
}

} // namespace puyasol::builder::eb
