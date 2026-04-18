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

namespace
{

/// Map a function type's return parameters to the dispatch return WType.
/// void for no returns, single WType for one return, WTuple for multiple.
awst::WType const* computeReturnType(BuilderContext& _ctx, FunctionType const* _funcType)
{
	if (!_funcType || _funcType->returnParameterTypes().empty())
		return awst::WType::voidType();
	auto const& rts = _funcType->returnParameterTypes();
	if (rts.size() == 1)
		return _ctx.typeMapper.map(rts[0]);
	std::vector<awst::WType const*> retTypes;
	for (auto const* rt : rts)
		retTypes.push_back(_ctx.typeMapper.map(rt));
	return _ctx.typeMapper.createType<awst::WTuple>(std::move(retTypes), std::nullopt);
}

/// True iff translation is happening from a library subroutine context,
/// where InstanceMethodTarget fails ("invocation outside of a contract method").
bool inLibraryContext(BuilderContext const& _ctx, std::string const& _currentCref)
{
	return !_ctx.contractName.empty()
		&& !_currentCref.empty()
		&& _currentCref.find("." + _ctx.contractName) == std::string::npos;
}

/// Left-pad `_bytes` to exactly _targetBytes bytes via `b| bzero(_targetBytes)`.
/// Both operands are shorter-first aligned to the right by `b|`.
std::shared_ptr<awst::Expression> leftPadBytes(
	std::shared_ptr<awst::Expression> _bytes,
	unsigned _targetBytes,
	awst::SourceLocation const& _loc)
{
	auto bzero = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
	bzero->stackArgs.push_back(awst::makeIntegerConstant(std::to_string(_targetBytes), _loc));
	auto orOp = awst::makeIntrinsicCall("b|", awst::WType::bytesType(), _loc);
	orOp->stackArgs.push_back(std::move(_bytes));
	orOp->stackArgs.push_back(std::move(bzero));
	return orOp;
}

/// For a public/external target function, compute the ARC4 WType that the
/// target's AWST parameter will have. Mirrors ContractBuilder's param remap:
///   - biguint (uint128..uint256, etc.): arc4.uintN preserving bit width.
///   - bytes[12] from a FunctionType param: arc4.static_array<arc4.uint8, 12>.
///   - other: nullptr (no wrapping needed).
awst::WType const* dispatchPublicArgArc4Type(
	awst::WType const* _nativeType, solidity::frontend::Type const* _paramSolType)
{
	using namespace solidity::frontend;
	if (_nativeType == awst::WType::biguintType())
	{
		unsigned bits = 256;
		if (auto const* intType = dynamic_cast<IntegerType const*>(_paramSolType))
			bits = intType->numBits();
		return new awst::ARC4UIntN(static_cast<int>(bits));
	}
	if (_nativeType && _nativeType->kind() == awst::WTypeKind::Bytes
		&& dynamic_cast<FunctionType const*>(_paramSolType))
	{
		// External fn-ptr bytes[12] → arc4.static_array<arc4.uint8, 12>.
		// ContractBuilder only ARC4-remaps bytes[N] params when the Solidity
		// type is FunctionType (see ContractBuilder.cpp isAggregate check);
		// matching that rule here so the dispatch call-site wraps iff the
		// target's signature expects an ARC4 arg.
		auto const* bytesType = static_cast<awst::BytesWType const*>(_nativeType);
		if (bytesType->length().has_value())
		{
			auto const* arc4Byte = new awst::ARC4UIntN(8);
			return new awst::ARC4StaticArray(arc4Byte, bytesType->length().value());
		}
	}
	return nullptr;
}

/// Map a Solidity type to the dispatch-method WType.
/// _promoteSignedI64Biguint=true treats int8..int64 (signed) as biguint so
/// that sign-extension works at the ABI boundary — used for dispatch return
/// types; for arg types it stays uint64.
awst::WType const* mapDispatchType(
	solidity::frontend::Type const* _solType, bool _promoteSignedI64Biguint)
{
	using namespace solidity::frontend;
	if (auto const* intType = dynamic_cast<IntegerType const*>(_solType))
	{
		if (intType->numBits() <= 64 && _promoteSignedI64Biguint && intType->isSigned())
			return awst::WType::biguintType();
		if (intType->numBits() <= 64)
			return awst::WType::uint64Type();
		return awst::WType::biguintType();
	}
	if (dynamic_cast<BoolType const*>(_solType))
		return awst::WType::boolType();
	if (auto const* arrType = dynamic_cast<ArrayType const*>(_solType))
	{
		if (arrType->isString())
			return awst::WType::stringType();
		if (arrType->isByteArray())
			return awst::WType::bytesType();
		return awst::WType::biguintType();
	}
	if (_solType && _solType->category() == Type::Category::StringLiteral)
		return awst::WType::stringType();
	if (auto const* fnType = dynamic_cast<FunctionType const*>(_solType))
		return FunctionPointerBuilder::mapFunctionType(fnType);
	if (auto const* fbType = dynamic_cast<FixedBytesType const*>(_solType))
		return new awst::BytesWType(static_cast<int>(fbType->numBytes()));
	return awst::WType::biguintType();
}

/// Encode one argument as ARC4-raw bytes for an inner-application-call
/// `ApplicationArgs[i]` field. Follows the ARC4 ABI encoding rules:
///   - uintN (N ≤ 64, native uint64): itob, then left-pad to N/8 bytes.
///   - biguint: reinterpret; if IntegerType, left-pad to N/8 bytes.
///   - bool: 1 byte, 0x80 for true else 0x00.
///   - bytes / string (dynamic): uint16(length) ++ raw bytes.
///   - other: reinterpret as bytes.
std::shared_ptr<awst::Expression> encodeArgForInnerTxn(
	std::shared_ptr<awst::Expression> _argExpr,
	solidity::frontend::Type const* _paramSolType,
	awst::SourceLocation const& _loc)
{
	unsigned targetBits = 256;
	if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(_paramSolType))
		targetBits = intType->numBits();
	unsigned targetBytes = targetBits / 8;

	if (_argExpr->wtype == awst::WType::uint64Type())
	{
		auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
		itob->stackArgs.push_back(std::move(_argExpr));
		std::shared_ptr<awst::Expression> bytesExpr = std::move(itob);
		if (targetBytes > 8 && dynamic_cast<solidity::frontend::IntegerType const*>(_paramSolType))
			bytesExpr = leftPadBytes(std::move(bytesExpr), targetBytes, _loc);
		return bytesExpr;
	}
	if (_argExpr->wtype == awst::WType::biguintType())
	{
		auto raw = awst::makeReinterpretCast(std::move(_argExpr), awst::WType::bytesType(), _loc);
		if (dynamic_cast<solidity::frontend::IntegerType const*>(_paramSolType))
			return leftPadBytes(std::move(raw), targetBytes, _loc);
		return raw;
	}
	if (_argExpr->wtype == awst::WType::boolType())
	{
		// ARC4 bool: 1 byte, 0x80 = true, 0x00 = false.
		auto setbit = awst::makeIntrinsicCall("setbit", awst::WType::bytesType(), _loc);
		std::vector<uint8_t> zero1{0};
		setbit->stackArgs.push_back(awst::makeBytesConstant(std::move(zero1), _loc));
		setbit->stackArgs.push_back(awst::makeIntegerConstant("0", _loc));
		setbit->stackArgs.push_back(std::move(_argExpr));
		return setbit;
	}
	if (auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(_paramSolType);
		arrType && arrType->isByteArrayOrString())
	{
		// ARC4 byte[] encoding: uint16(length) ++ raw_bytes.
		if (_argExpr->wtype != awst::WType::bytesType())
			_argExpr = awst::makeReinterpretCast(std::move(_argExpr), awst::WType::bytesType(), _loc);
		auto lenExpr = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
		lenExpr->stackArgs.push_back(_argExpr);
		auto itobLen = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
		itobLen->stackArgs.push_back(std::move(lenExpr));
		auto header = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
		header->immediates = {6, 2};
		header->stackArgs.push_back(std::move(itobLen));
		auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
		concat->stackArgs.push_back(std::move(header));
		concat->stackArgs.push_back(std::move(_argExpr));
		return concat;
	}
	// Fallback: reinterpret as bytes.
	if (_argExpr->wtype != awst::WType::bytesType())
		return awst::makeReinterpretCast(std::move(_argExpr), awst::WType::bytesType(), _loc);
	return _argExpr;
}

} // namespace

std::shared_ptr<awst::SubroutineCallExpression> FunctionPointerBuilder::buildDispatchCall(
	BuilderContext& _ctx,
	FunctionType const* _funcType,
	std::shared_ptr<awst::Expression> _ptrIdExpr,
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc)
{
	std::string dname = dispatchName(_funcType);
	s_neededDispatches[dname] = _funcType;

	auto call = std::make_shared<awst::SubroutineCallExpression>();
	call->sourceLocation = _loc;
	call->wtype = computeReturnType(_ctx, _funcType);
	if (inLibraryContext(_ctx, s_currentCref))
		call->target = awst::SubroutineID{s_currentCref + "." + dname};
	else
		call->target = awst::InstanceMethodTarget{dname};

	awst::CallArg idArg;
	idArg.name = "__funcptr_id";
	idArg.value = std::move(_ptrIdExpr);
	call->args.push_back(std::move(idArg));

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
		// External function pointer = concat(appIdBytes[8], selectorBytes[4]) = 12 bytes.
		// `this.f` → (itob(0) sentinel, internalFuncId[:4]) — self-call, uses
		//   internal dispatch at call time.
		// `C(addr).f` → (8-byte appId derived from addr, f.ARC4-selector) — inner
		//   app txn at call time.
		// Calling code checks appId == 0 (self) for internal dispatch; non-zero
		// uses inner txn with that appId.
		static awst::BytesWType s_bytes12(12);

		// Helper: itob(constInt) → 8 bytes.
		auto makeItobConst = [&](std::string _val) -> std::shared_ptr<awst::Expression> {
			auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
			itob->stackArgs.push_back(awst::makeIntegerConstant(std::move(_val), _loc));
			return itob;
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
				auto toU64 = awst::makeReinterpretCast(std::move(addr), awst::WType::uint64Type(), _loc);
				auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
				itob->stackArgs.push_back(std::move(toU64));
				appIdBytes = std::move(itob);
			}
			else
			{
				if (addr->wtype != awst::WType::bytesType())
					addr = awst::makeReinterpretCast(std::move(addr), awst::WType::bytesType(), _loc);
				auto extract = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
				extract->immediates = {24, 8};
				extract->stackArgs.push_back(std::move(addr));
				appIdBytes = std::move(extract);
			}

			// Store the target's ARC4 method selector in the selector slot.
			// At call-time with appId != 0 we emit an inner app txn with
			// ApplicationArgs[0] = this selector.
			auto selectorConst = std::make_shared<awst::MethodConstant>();
			selectorConst->sourceLocation = _loc;
			selectorConst->wtype = awst::WType::bytesType();
			selectorConst->value = AbiEncoderBuilder::buildARC4MethodSelector(_ctx, _funcDef);
			selectorBytes = std::move(selectorConst);
		}
		else
		{
			Logger::instance().warning(
				"external function pointer '" + _funcDef->name()
				+ "': reentrancy is not possible on AVM; self-calls will use "
				"internal dispatch instead of inner transactions", _loc);

			// Self-reference: appId=0 sentinel means "current application —
			// use internal dispatch". The selector slot holds the internal
			// fn-ptr ID so runtime dispatch can route without an inner txn.
			if (auto const* internalFuncType = _funcDef->functionType(true))
				registerTarget(_funcDef, internalFuncType);
			auto idIt = s_targets.find(_funcDef->id());
			unsigned funcId = (idIt != s_targets.end()) ? idIt->second.id : 0;

			appIdBytes = makeItobConst("0");
			// itob(funcId)[4:4] — take the low 4 bytes of the 8-byte itob.
			auto idBytes4 = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
			idBytes4->immediates = {4, 4};
			idBytes4->stackArgs.push_back(makeItobConst(std::to_string(funcId)));
			selectorBytes = std::move(idBytes4);
		}

		auto packed = awst::makeIntrinsicCall("concat", &s_bytes12, _loc);
		packed->stackArgs.push_back(std::move(appIdBytes));
		packed->stackArgs.push_back(std::move(selectorBytes));
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

		// Helper: extract N bytes starting at offset from the 12-byte ptr.
		auto extractSlice = [&](int _offset, int _length) {
			auto e = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
			e->immediates = {_offset, _length};
			e->stackArgs.push_back(_ptrExpr);
			return e;
		};
		// Helper: btoi(extractSlice(offset, 8)) — extract a uint64 from ptr.
		auto extractU64 = [&](int _offset) {
			auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
			btoi->stackArgs.push_back(extractSlice(_offset, 8));
			return btoi;
		};

		// Check if self-call: appId == 0 (sentinel for current app).
		auto isSelf = std::make_shared<awst::NumericComparisonExpression>();
		isSelf->sourceLocation = _loc;
		isSelf->wtype = awst::WType::boolType();
		isSelf->lhs = extractU64(0);
		isSelf->op = awst::NumericComparison::Eq;
		isSelf->rhs = awst::makeIntegerConstant("0", _loc);

		// Self-call path: extract internal ID from selector slot (bytes 8..12).
		auto internalId = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
		internalId->stackArgs.push_back(extractSlice(8, 4));

		// Build internal dispatch call with the same args (shared with
		// the inner-txn branch below — hence we pass _args by value)
		auto selfCall = buildDispatchCall(_ctx, _funcType, std::move(internalId), _args, _loc);
		awst::WType const* retType = selfCall->wtype;

		// ── Inner-txn (cross-contract) branch ──
		// Selector slot (bytes 8..12) = ARC4 method selector for the callee's
		// router; used as ApplicationArgs[0].
		auto sel4 = extractSlice(8, 4);

		// Build ApplicationArgs tuple: [selector, arg0_encoded, arg1_encoded, ...]
		auto argsTuple = std::make_shared<awst::TupleExpression>();
		argsTuple->sourceLocation = _loc;
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
		auto create = std::make_shared<awst::CreateInnerTransaction>();
		create->sourceLocation = _loc;
		create->wtype = &s_applFieldsType;
		create->fields["TypeEnum"] = awst::makeIntegerConstant("6", _loc);
		create->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
		// ApplicationID: reinterpret uint64 appId to application type
		create->fields["ApplicationID"] = awst::makeReinterpretCast(
			extractU64(0), awst::WType::applicationType(), _loc);
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
			dispatch.returnType = mapDispatchType(
				funcType->returnParameterTypes()[0], /*_promoteSignedI64Biguint=*/true);
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

					awst::WType const* arc4Type = isPublic
						? dispatchPublicArgArc4Type(var->wtype, funcType->parameterTypes()[i])
						: nullptr;

					if (arc4Type && arc4Type != var->wtype)
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
