/// @file InnerCallHandlers.cpp
/// Handles address.call/staticcall/delegatecall/transfer inner transaction patterns
/// and precompile routing.

#include "builder/itxn/InnerCallHandlers.h"
#include "awst/NameGen.h"
#include "builder/EvmFeaturePolicy.h"
#include "builder/abi/AbiEncoderBuilder.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/itxn/InnerCallInternal.h"
#include "builder/itxn/CallResolver.h"
#include "builder/sol-eb/SolBoolBuilder.h"
#include "builder/sol-types/ConversionPlan.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

namespace puyasol::builder::eb
{

// ── Helpers ──

std::optional<uint64_t> detectPrecompileAddress(
	solidity::frontend::Expression const& _baseExpr)
{
	using namespace solidity::frontend;
	std::optional<uint64_t> precompileAddr;
	if (auto const* baseCall = dynamic_cast<FunctionCall const*>(&_baseExpr))
	{
		if (baseCall->annotation().kind.set()
			&& *baseCall->annotation().kind == FunctionCallKind::TypeConversion
			&& !baseCall->arguments().empty())
		{
			auto const* argType = baseCall->arguments()[0]->annotation().type;
			if (auto const* ratType = dynamic_cast<RationalNumberType const*>(argType))
			{
				auto val = ratType->literalValue(nullptr);
				if (val >= 1 && val <= 10)
					precompileAddr = static_cast<uint64_t>(val);
			}
		}
	}
	return precompileAddr;
}

static bool isLiteralZeroAddress(
	solidity::frontend::Expression const& _baseExpr)
{
	using namespace solidity::frontend;
	auto const* baseCall = dynamic_cast<FunctionCall const*>(&_baseExpr);
	if (!baseCall || !baseCall->annotation().kind.set()
		|| *baseCall->annotation().kind != FunctionCallKind::TypeConversion
		|| baseCall->arguments().empty())
		return false;
	auto const* rational = dynamic_cast<RationalNumberType const*>(
		baseCall->arguments()[0]->annotation().type);
	return rational && rational->literalValue(nullptr) == 0;
}

static awst::WTuple s_boolBytesType(
	std::vector<awst::WType const*>{awst::WType::boolType(), awst::WType::bytesType()});

std::shared_ptr<awst::Expression> InnerCallHandlers::makeBoolBytesTuple(
	bool _success,
	std::shared_ptr<awst::Expression> _data,
	awst::SourceLocation const& _loc)
{
	auto tuple = awst::makeTupleExpression(&s_boolBytesType, _loc);
	tuple->items.push_back(awst::makeBoolConstant(_success, _loc));
	tuple->items.push_back(std::move(_data));
	return tuple;
}

std::shared_ptr<awst::Expression> InnerCallHandlers::makeBoolBytesTupleEmpty(
	awst::SourceLocation const& _loc)
{
	return makeBoolBytesTuple(true, awst::makeBytesConstant({}, _loc), _loc);
}

std::shared_ptr<awst::IntrinsicCall> InnerCallHandlers::makeExtract(
	std::shared_ptr<awst::Expression> _source, int _offset, int _length,
	awst::SourceLocation const& _loc)
{
	auto call = awst::makeExtract3(std::move(_source), awst::makeIntegerConstant(_offset, _loc), awst::makeIntegerConstant(_length, _loc), _loc);
	return call;
}

std::shared_ptr<awst::IntrinsicCall> InnerCallHandlers::makeConcat(
	std::shared_ptr<awst::Expression> _a, std::shared_ptr<awst::Expression> _b,
	awst::SourceLocation const& _loc)
{
	return awst::makeConcat(std::move(_a), std::move(_b), _loc);
}

std::shared_ptr<awst::Expression> InnerCallHandlers::addressToAppId(
	std::shared_ptr<awst::Expression> _receiver, awst::SourceLocation const& _loc)
{
	if (_receiver->wtype == awst::WType::applicationType())
		return _receiver;

	// CurrentApplicationAddress is a hash, not our \x00*24 ++ appId format —
	// detect it and use CurrentApplicationID directly.
	if (auto const* intrinsic = dynamic_cast<awst::IntrinsicCall const*>(_receiver.get()))
	{
		if (intrinsic->opCode == "global" && !intrinsic->immediates.empty())
		{
			auto const* imm = std::get_if<std::string>(&intrinsic->immediates[0]);
			if (imm && *imm == "CurrentApplicationAddress")
			{
				auto appId = awst::makeGlobal(std::string("CurrentApplicationID"), awst::WType::uint64Type(), _loc);

				auto cast = awst::makeAsApplication(std::move(appId), _loc);
				return cast;
			}
		}
	}

	std::shared_ptr<awst::Expression> bytesExpr = std::move(_receiver);
	if (bytesExpr->wtype == awst::WType::accountType())
	{
		auto toBytes = awst::makeAsBytes(std::move(bytesExpr), _loc);
		bytesExpr = std::move(toBytes);
	}

	auto btoi = awst::makeWord32ToUInt64(std::move(bytesExpr), _loc);
	return awst::makeAsApplication(std::move(btoi), _loc);
}

std::shared_ptr<awst::Expression> InnerCallHandlers::captureLastLog(
	ContractContext& _ctx, awst::SourceLocation const& _loc)
{
	std::string tmp = "__itxn_log_"
		+ std::to_string(awst::NameGen::next("InnerCallHandlers.itxnLogCounter"));
	_ctx.preEffects().push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(tmp, awst::WType::bytesType(), _loc),
		awst::makeItxn("LastLog", awst::WType::bytesType(), _loc), _loc));
	return awst::makeVarExpression(tmp, awst::WType::bytesType(), _loc);
}

std::shared_ptr<awst::Expression> InnerCallHandlers::encodeArgToBytes(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _argExpr,
	solidity::frontend::Type const* _sourceSolType,
	solidity::frontend::Type const* _paramSolType,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	if (_paramSolType)
		_argExpr = builder::ConversionPlan{
			_sourceSolType,
			_paramSolType,
			_ctx.typeMapper.map(_paramSolType),
			builder::ConversionPlan::Context::AbiArgument}.emit(
				std::move(_argExpr), _loc);

	bool isDynamicBytes = false;
	if (_paramSolType)
	{
		auto cat = _paramSolType->category();
		isDynamicBytes = (cat == Type::Category::Array
			&& dynamic_cast<ArrayType const*>(_paramSolType)
			&& dynamic_cast<ArrayType const*>(_paramSolType)->isByteArrayOrString());
	}

	if (_argExpr->wtype == awst::WType::bytesType()
		|| _argExpr->wtype->kind() == awst::WTypeKind::Bytes)
	{
		if (isDynamicBytes)
		{
			// ARC4 byte[]: uint16(len)++raw. makeEvalOnce for side-effecting args.
			_argExpr = awst::makeEvalOnce(std::move(_argExpr), _loc);
			auto lenExpr = awst::makeLen(_argExpr, _loc);
			auto itobLen = awst::makeItob(std::move(lenExpr), _loc);
			auto header = awst::makeExtract(std::move(itobLen), 6, 2, _loc);

			return awst::makeConcat(std::move(header), std::move(_argExpr), _loc);
		}
		return _argExpr;
	}
	else if (_argExpr->wtype == awst::WType::uint64Type())
	{
		// itob → 8 bytes; left-pad if param is wider (callee's arc4 len check).
		unsigned widthBytes = 8;
		if (_paramSolType)
		{
			if (auto const* intType = dynamic_cast<IntegerType const*>(_paramSolType))
				widthBytes = intType->numBits() / 8;
			else if (auto const* addr = dynamic_cast<AddressType const*>(_paramSolType))
				(void)addr, widthBytes = 32; // ARC-4 address is the full AVM account
		}
		auto itob = awst::makeItob(std::move(_argExpr), _loc);
		if (widthBytes <= 8)
			return itob;
		// pad = bzero(widthBytes - 8)  ++  itob(value)
		return awst::makeLeftPad(std::move(itob), widthBytes - 8, _loc);
	}
	else if (_argExpr->wtype == awst::WType::biguintType())
	{
		// Encode to the param's exact ARC4 width (N/8 bytes); callee arc4 decode
		// asserts len==N/8, so a 32-byte arg reverts. makeARC4Encode trims to low
		// N/8 bytes. int256/uint256 stays 32 bytes.
		auto const* solT = _paramSolType;
		if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(solT))
			solT = &udvt->underlyingType();
		if (dynamic_cast<IntegerType const*>(solT))
		{
			auto* arc4Type = _ctx.typeMapper.mapSolTypeToARC4(_paramSolType);
			auto enc = awst::makeARC4Encode(std::move(_argExpr), arc4Type, _loc);
			return awst::makeAsBytes(std::move(enc), _loc);
		}
		// Non-integer biguint (rare): keep the 32-byte left-pad.
		auto cast = awst::makeAsBytes(std::move(_argExpr), _loc);
		return awst::makeLeftPadToN(std::move(cast), 32, _loc);
	}
	else if (_argExpr->wtype == awst::WType::boolType())
	{
		// bool → ARC4 bool = setbit(0x00, 0, boolValue)
		return awst::makeSetbit(
			awst::makeBytesConstant({0x00}, _loc),
			awst::makeZero(_loc),
			std::move(_argExpr), _loc);
	}
	else if (_argExpr->wtype->kind() == awst::WTypeKind::ReferenceArray)
	{
		// ReferenceArray → ARC4 encode
		auto* refArr = dynamic_cast<awst::ReferenceArray const*>(_argExpr->wtype);
		auto* elemType = refArr ? refArr->elementType() : nullptr;
		auto* arc4ElemType = elemType ? _ctx.typeMapper.mapToARC4Type(elemType) : nullptr;

		awst::WType const* arc4ArrayType = nullptr;
		if (arc4ElemType && refArr && refArr->arraySize())
			arc4ArrayType = _ctx.typeMapper.createType<awst::ARC4StaticArray>(
				arc4ElemType, *refArr->arraySize());
		else if (arc4ElemType)
			arc4ArrayType = _ctx.typeMapper.createType<awst::ARC4DynamicArray>(arc4ElemType);

		if (arc4ArrayType)
		{
			auto encode = awst::makeARC4Encode(std::move(_argExpr), arc4ArrayType, _loc);

			auto rcast = awst::makeAsBytes(std::move(encode), _loc);
			return rcast;
		}
	}
	else if (_argExpr->wtype->kind() == awst::WTypeKind::ARC4StaticArray
		|| _argExpr->wtype->kind() == awst::WTypeKind::ARC4DynamicArray
		|| _argExpr->wtype->kind() == awst::WTypeKind::ARC4Struct
		|| _argExpr->wtype->kind() == awst::WTypeKind::ARC4Tuple)
	{
		auto rcast = awst::makeAsBytes(std::move(_argExpr), _loc);
		return rcast;
	}
	else
	{
		auto rcast = awst::makeAsBytes(std::move(_argExpr), _loc);
		return rcast;
	}
}

std::shared_ptr<awst::Expression> InnerCallHandlers::encodeEvmArgumentBody(
	ContractContext& _ctx,
	std::vector<solidity::frontend::ASTPointer<
		solidity::frontend::Expression const>> const& _args,
	std::vector<solidity::frontend::Type const*> const& _paramTypes,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	std::vector<Type const*> types;
	std::vector<std::shared_ptr<awst::Expression>> values;
	for (size_t i = 0; i < _args.size(); ++i)
	{
		auto const* sourceType = _args[i]->annotation().type;
		auto const* targetType = i < _paramTypes.size() && _paramTypes[i]
			? _paramTypes[i] : sourceType;
		if (targetType && targetType->category() == Type::Category::StringLiteral)
			targetType = targetType->mobileType();
		auto value = _ctx.buildExpr(*_args[i]);
		if (i < _paramTypes.size() && _paramTypes[i])
			value = builder::ConversionPlan{
				sourceType, _paramTypes[i], _ctx.typeMapper.map(_paramTypes[i]),
				builder::ConversionPlan::Context::AbiArgument}.emit(
					std::move(value), _loc);
		types.push_back(targetType);
		values.push_back(std::move(value));
	}
	return AbiEncoderBuilder::encodeValuesAsEvmAbi(
		_ctx, types, std::move(values), _loc);
}

std::shared_ptr<awst::TupleExpression>
InnerCallHandlers::buildEvmApplicationArgs(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _selector,
	std::vector<solidity::frontend::ASTPointer<
		solidity::frontend::Expression const>> const& _args,
	std::vector<solidity::frontend::Type const*> const& _paramTypes,
	awst::SourceLocation const& _loc)
{
	auto tuple = awst::makeTupleExpression(nullptr, _loc);
	tuple->items.push_back(std::move(_selector));
	tuple->items.push_back(encodeEvmArgumentBody(
		_ctx, _args, _paramTypes, _loc));
	std::vector<awst::WType const*> wireTypes;
	for (auto const& item: tuple->items)
		wireTypes.push_back(item->wtype);
	tuple->wtype = _ctx.typeMapper.createType<awst::WTuple>(
		std::move(wireTypes), std::nullopt);
	return tuple;
}

// Nested ARC4 type name (struct-field / array-element position).
// Differs from top-level: exact bit width (not collapsed to uint64),
// signedness preserved (e.g. nested int8 = "int8", not "uint8").
// Verified against puya's `method "..."` output.
std::string nestedArc4Name(ContractContext& _ctx, solidity::frontend::Type const* _type)
{
	using namespace solidity::frontend;
	if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(_type))
		_type = &udvt->underlyingType();   // also lets UDVT-wrapped bool/address/bytesN hit their branches below
	// int (sign-preserving) or enum → its unsigned encoding width — one carrier lookup.
	if (auto it = builder::SolIntType::fromSolOrEnum(_type))
		return (it->isSigned ? "int" : "uint") + std::to_string(it->bits);
	if (dynamic_cast<BoolType const*>(_type)) return "bool";
	if (dynamic_cast<AddressType const*>(_type)) return "address";
	if (auto const* fb = dynamic_cast<FixedBytesType const*>(_type))
		return "byte[" + std::to_string(fb->numBytes()) + "]";
	if (auto const* arrT = dynamic_cast<ArrayType const*>(_type))
	{
		if (arrT->isByteArrayOrString())
			return arrT->isString() ? "string" : "byte[]";
		std::string elem = nestedArc4Name(_ctx, arrT->baseType());
		if (arrT->isDynamicallySized())
			return elem + "[]";
		return elem + "[" + arrT->length().str() + "]";
	}
	if (auto const* structT = dynamic_cast<StructType const*>(_type))
	{
		std::string s = "(";
		bool first = true;
		for (auto const& m : structT->structDefinition().members())
		{
			if (!first) s += ",";
			s += nestedArc4Name(_ctx, m->type());
			first = false;
		}
		return s + ")";
	}
	// Unhandled (e.g. external function pointers, contracts): name it the SAME way the callee
	// does — via the ARC4 type mapping — so cross-contract selectors match (toString would
	// give e.g. "function () external" where puya publishes "byte[12]").
	return TypeCoercion::wtypeToABIName(_ctx.typeMapper.mapToARC4Type(_ctx.typeMapper.map(_type)));
}

// Top-level param name: scalars collapse to "uint64"/"uintN" (signedness dropped);
// enums → "uint64"; aggregates expand via nestedArc4Name to match puya's tuple form.
// (A plain struct emitted "struct P" previously, silently breaking dispatch.)
std::string solTypeToArc4ParamName(
	ContractContext& _ctx, solidity::frontend::Type const* _type)
{
	if (auto name = builder::TypeCoercion::intSelectorName(_type))
		return *name;
	auto* wtype = _ctx.typeMapper.map(_type);
	if (wtype == awst::WType::biguintType()) return "uint256";
	if (wtype == awst::WType::uint64Type()) return "uint64"; // enums, etc.
	if (wtype == awst::WType::boolType()) return "bool";
	if (wtype == awst::WType::accountType()) return "address";
	if (wtype == awst::WType::bytesType()) return "byte[]";
	if (wtype == awst::WType::stringType()) return "string";
	if (auto len = awst::fixedBytesLength(wtype))
		return "byte[" + std::to_string(*len) + "]";
	if (wtype->kind() == awst::WTypeKind::Bytes)
		return "byte[]";
	// Aggregates AND exotics (fn pointers, contracts): nestedArc4Name recurses the
	// former and falls back to the callee-published ARC4 mapping for the latter —
	// `toString(true)` here produced "function () external" where puya registers
	// the profile-selected function-pointer byte array, an unroutable selector.
	return nestedArc4Name(_ctx, _type);
}

std::string solTypeToArc4ReturnName(
	ContractContext& _ctx, solidity::frontend::Type const* _type)
{
	if (auto name = builder::TypeCoercion::intSelectorReturnName(_type))
		return *name;
	return solTypeToArc4ParamName(_ctx, _type);
}

std::string InnerCallHandlers::buildMethodSelector(
	ContractContext& _ctx,
	std::string const& _name,
	solidity::frontend::FunctionType const& _funcType)
{
	std::vector<std::string> paramNames, retNames;
	for (auto const& paramType : _funcType.parameterTypes())
		paramNames.push_back(solTypeToArc4ParamName(_ctx, paramType));
	for (auto const& retType : _funcType.returnParameterTypes())
		retNames.push_back(solTypeToArc4ReturnName(_ctx, retType));
	return builder::TypeCoercion::buildArc4Selector(_name, paramNames, retNames);
}

std::string InnerCallHandlers::buildMethodSelector(
	ContractContext& _ctx,
	solidity::frontend::FunctionDefinition const* _func)
{
	std::vector<std::string> paramNames, retNames;
	for (auto const& param : _func->parameters())
		paramNames.push_back(solTypeToArc4ParamName(_ctx, param->type()));
	for (auto const& retParam : _func->returnParameters())
		retNames.push_back(solTypeToArc4ReturnName(_ctx, retParam->type()));
	return builder::TypeCoercion::buildArc4Selector(_func->name(), paramNames, retNames);
}

// ── Payment ──

std::shared_ptr<awst::Expression> InnerCallHandlers::buildPaymentTransaction(
	ContractContext& /*_ctx*/,
	std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount,
	awst::SourceLocation const& _loc)
{
	static awst::WInnerTransactionFields s_payFieldsType(TxnTypePay);

	auto create = awst::makeCreateInnerTransaction(&s_payFieldsType, _loc);
	create->fields["TypeEnum"] = awst::makeIntegerConstant(TxnTypePay, _loc);
	create->fields["Fee"] = awst::makeZero(_loc);
	create->fields["Receiver"] = std::move(_receiver);
	create->fields["Amount"] = std::move(_amount);
	return create;
}

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleTransfer(
	ContractContext& _ctx, std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount, awst::SourceLocation const& _loc)
{
	auto create = buildPaymentTransaction(_ctx, std::move(_receiver), std::move(_amount), _loc);
	static awst::WInnerTransaction s_payTxnType(TxnTypePay);
	auto submit = awst::makeSubmitInnerTransaction(&s_payTxnType, _loc);
	submit->itxns.push_back(std::move(create));

	auto stmt = awst::makeExpressionStatement(submit, _loc);
	_ctx.postEffects().push_back(std::move(stmt));

	auto vc = awst::makeVoidConstant(_loc);
	return std::make_unique<GenericResultBuilder>(_ctx, std::move(vc));
}

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleSend(
	ContractContext& _ctx, std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount, awst::SourceLocation const& _loc)
{
	EvmFeaturePolicy::report(
		EvmFeature::LowLevelCallOutcome, _ctx.typeMapper.profile(), _loc);
	auto create = buildPaymentTransaction(_ctx, std::move(_receiver), std::move(_amount), _loc);
	static awst::WInnerTransaction s_payTxnType(TxnTypePay);
	auto submit = awst::makeSubmitInnerTransaction(&s_payTxnType, _loc);
	submit->itxns.push_back(std::move(create));

	auto stmt = awst::makeExpressionStatement(submit, _loc);
	_ctx.postEffects().push_back(std::move(stmt));

	return std::make_unique<SolBoolBuilder>(_ctx, awst::makeTrue(_loc));
}

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleCallWithValue(
	ContractContext& _ctx, std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount, awst::SourceLocation const& _loc)
{
	EvmFeaturePolicy::report(
		EvmFeature::LowLevelCallOutcome, _ctx.typeMapper.profile(), _loc);
	auto create = buildPaymentTransaction(_ctx, std::move(_receiver), std::move(_amount), _loc);
	static awst::WInnerTransaction s_payTxnType(TxnTypePay);
	auto submit = awst::makeSubmitInnerTransaction(&s_payTxnType, _loc);
	submit->itxns.push_back(std::move(create));

	auto stmt = awst::makeExpressionStatement(submit, _loc);
	_ctx.postEffects().push_back(std::move(stmt));

	return std::make_unique<GenericResultBuilder>(_ctx, makeBoolBytesTupleEmpty(_loc));
}

// ── .call(abi.encodeCall(...)) ──

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleDelegatecall(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc)
{
	// AVM has no DELEGATECALL: every app has isolated storage, every inner txn
	// has its own caller. Fail LOUDLY — but at RUNTIME, on reach: a compile
	// error here rejected whole real-world trees over OZ Address.sol's
	// unreachable functionDelegateCall utility (Aave's Pool never calls it;
	// puya's DCE strips the unreached body entirely). Any delegatecall that
	// actually EXECUTES still dies with an explicit message, never a silently
	// wrong stub.
	EvmFeaturePolicy::report(
		EvmFeature::DelegateCall, _ctx.typeMapper.profile(), _loc);
	for (auto const& arg : _callNode.arguments())
		_ctx.buildExpr(*arg);
	// assert(false) is a compile-time TERMINATOR: puya flags the statements
	// that consume the (bool, bytes) result as unreachable and rejects the
	// whole program (fbtc). Use a runtime-opaque always-false condition —
	// Global.Round is never 0 on any chain, and puya cannot fold it.
	auto round = awst::makeIntrinsicCall(
		"global", awst::WType::uint64Type(), _loc);
	round->immediates.push_back("Round");
	auto neverTrue = awst::makeNumericCompare(std::move(round),
		awst::NumericComparison::Eq,
		awst::makeIntegerConstant(uint64_t{0}, _loc), _loc);
	_ctx.queuePreExpression(awst::makeAssert(std::move(neverTrue), _loc,
		"delegatecall is not supported on AVM"), _loc);
	return std::make_unique<GenericResultBuilder>(_ctx, makeBoolBytesTupleEmpty(_loc));
}

// ── Top-level dispatcher ──

std::unique_ptr<InstanceBuilder> InnerCallHandlers::tryHandleAddressCall(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _receiver,
	std::string const& _memberName,
	solidity::frontend::FunctionCall const& _callNode,
	std::shared_ptr<awst::Expression> _callValue,
	solidity::frontend::Expression const& _baseExpr,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;

	// .transfer(amount)
	if (_memberName == "transfer" && _callNode.arguments().size() == 1)
	{
		auto amount = _ctx.buildExpr(*_callNode.arguments()[0]);
		amount = TypeCoercion::checkedAmountToUint64(_ctx.preEffects(), std::move(amount), _loc);
		return handleTransfer(_ctx, std::move(_receiver), std::move(amount), _loc);
	}

	// .send(amount)
	if (_memberName == "send" && _callNode.arguments().size() == 1)
	{
		auto amount = _ctx.buildExpr(*_callNode.arguments()[0]);
		amount = TypeCoercion::checkedAmountToUint64(_ctx.preEffects(), std::move(amount), _loc);
		return handleSend(_ctx, std::move(_receiver), std::move(amount), _loc);
	}

	// .call{value: X} with NO data argument → bare payment. A non-empty data
	// argument must ALSO invoke the target (payment + app call in one inner
	// group) — matching any .call{value:} here silently dropped the calldata.
	if (_memberName == "call" && _callValue && _callNode.arguments().empty())
		return handleCallWithValue(_ctx, std::move(_receiver), std::move(_callValue), _loc);

	// .call(abi.encodeCall(...)) → inner app call
	// staticcall lowers IDENTICALLY to call on the AVM (an inner ApplicationCall txn): there is no
	// inner-txn read-only flag, so the EVM static (no-state-change) guarantee can't be enforced. Route
	// staticcall through the same handling as call and warn that "static" is not respected. (Precompile
	// staticcalls, self-calls, encodeCall/encodeWithSignature typed routing all come along for free.)
	// solc rejects {value:} on staticcall, so _callValue here implies _memberName == "call".
	if ((_memberName == "call" || _memberName == "staticcall") && !_callNode.arguments().empty())
	{
		if (_memberName == "staticcall")
			EvmFeaturePolicy::report(
				EvmFeature::StaticCall, _ctx.typeMapper.profile(), _loc);
		auto const& dataArg = *_callNode.arguments()[0];

		// {value:} needs an inner PaymentTxn grouped with a real inner app call.
		// Self-calls rewrite to a direct callsub (no inner txn to attach it to)
		// and precompiles have no account to pay — fail loud, don't drop value.
		if (_callValue)
		{
			bool selfReceiver = false;
			if (auto const* intr = dynamic_cast<awst::IntrinsicCall const*>(_receiver.get()))
				if (intr->opCode == "global" && !intr->immediates.empty())
					if (auto const* im = std::get_if<std::string>(&intr->immediates[0]);
						im && *im == "CurrentApplicationAddress")
						selfReceiver = true;
			if (selfReceiver || detectPrecompileAddress(_baseExpr))
			{
				Logger::instance().error(
					std::string("`.call{value: ...}(data)` to ")
						+ (selfReceiver ? "the contract itself" : "a precompile address")
						+ " is not supported on AVM: the value payment cannot be "
						  "attached (self-calls lower to a direct subroutine call; "
						  "precompiles have no account). Split into a separate "
						  "transfer + call.", _loc);
				_ctx.buildExpr(dataArg);
				return std::make_unique<GenericResultBuilder>(_ctx, makeBoolBytesTupleEmpty(_loc));
			}
		}

		// Self-call with abi.encodeWithSignature/WithSelector: resolve to a
		// direct subroutine call (mirrors handleCallWithEncodeCall self-call
		// path; avoids fallback stub for contracts without a fallback).
		{
			bool isSelfCallEwS = false;
			if (auto const* intrinsic = dynamic_cast<awst::IntrinsicCall const*>(_receiver.get()))
				if (intrinsic->opCode == "global" && !intrinsic->immediates.empty())
					if (auto const* imm = std::get_if<std::string>(&intrinsic->immediates[0]); imm && *imm == "CurrentApplicationAddress")
						isSelfCallEwS = true;

			if (isSelfCallEwS)
			{
				if (auto const* encCallExpr = dynamic_cast<FunctionCall const*>(&dataArg))
				{
					auto const* encMA = dynamic_cast<MemberAccess const*>(&encCallExpr->expression());
					// Both recognised shapes lower to a direct InstanceMethodTarget:
					//   address(this).call(abi.encodeWithSignature("fn(types)", args...))
					//   address(this).call(abi.encodeWithSelector(this.fn.selector, args...))
					std::string fnName;
					// Full signature string from encodeWithSignature ("fn(types)")
					// — carries the param types for exact overload matching.
					std::string sigString;
					// Referenced FunctionDefinition when the encode form names a
					// SPECIFIC function (encodeCall/encodeWithSelector) — resolves
					// the exact overload directly instead of name+arity.
					FunctionDefinition const* refFunc = nullptr;
					// Expression used only to identify the target. A successful direct
					// rewrite must still evaluate it before the encoded arguments.
					Expression const* targetIdentityExpr = nullptr;
					// Method args, normalised across the three encode forms:
					//   encodeWithSignature("fn(types)", a, b, …) → args spread at indices 1..
					//   encodeWithSelector(this.fn.selector, a, b, …) → args spread at indices 1..
					//   encodeCall(C.fn, (a, b, …)) → args as a TUPLE in index 1 (or a single value)
					std::vector<ASTPointer<Expression const>> resolvedArgs;
					if (encMA && encMA->memberName() == "encodeWithSignature"
						&& !encCallExpr->arguments().empty())
					{
						if (auto const* sigLit = dynamic_cast<Literal const*>(encCallExpr->arguments()[0].get()))
						{
							sigString = sigLit->value();
							auto parenPos = sigString.find('(');
							if (parenPos != std::string::npos)
								fnName = sigString.substr(0, parenPos);
						}
						for (size_t i = 1; i < encCallExpr->arguments().size(); ++i)
							resolvedArgs.push_back(encCallExpr->arguments()[i]);
					}
					else if (encMA && encMA->memberName() == "encodeWithSelector"
						&& !encCallExpr->arguments().empty())
					{
						targetIdentityExpr = encCallExpr->arguments()[0].get();
						// `this.fn.selector` = MemberAccess("selector", MemberAccess("fn", this)).
						if (auto const* selMA = dynamic_cast<MemberAccess const*>(encCallExpr->arguments()[0].get()))
							if (selMA->memberName() == "selector")
								if (auto const* fnMA = dynamic_cast<MemberAccess const*>(&selMA->expression()))
								{
									fnName = fnMA->memberName();
									refFunc = dynamic_cast<FunctionDefinition const*>(
										fnMA->annotation().referencedDeclaration);
								}
						for (size_t i = 1; i < encCallExpr->arguments().size(); ++i)
							resolvedArgs.push_back(encCallExpr->arguments()[i]);
					}
					else if (encMA && encMA->memberName() == "encodeCall"
						&& !encCallExpr->arguments().empty())
					{
						targetIdentityExpr = encCallExpr->arguments()[0].get();
						// encodeCall(Contract.fn, (args…)): the fn ref names the exact
						// function; resolve the same-signature method on `this` by id
						// (inherited/overridden impl + its return type). Args are a
						// tuple in index 1.
						auto const* fref = encCallExpr->arguments()[0].get();
						if (auto const* m = dynamic_cast<MemberAccess const*>(fref))
						{
							fnName = m->memberName();
							refFunc = dynamic_cast<FunctionDefinition const*>(
								m->annotation().referencedDeclaration);
						}
						else if (auto const* id = dynamic_cast<Identifier const*>(fref))
						{
							fnName = id->name();
							refFunc = dynamic_cast<FunctionDefinition const*>(
								id->annotation().referencedDeclaration);
						}
						if (encCallExpr->arguments().size() >= 2)
						{
							auto const& argsExpr = *encCallExpr->arguments()[1];
							if (auto const* tup = dynamic_cast<TupleExpression const*>(&argsExpr))
							{
								for (auto const& comp : tup->components())
									if (comp) resolvedArgs.push_back(comp);
							}
							else
								resolvedArgs.push_back(encCallExpr->arguments()[1]);
						}
					}
					if (!fnName.empty())
					{
						size_t nArgs = resolvedArgs.size();
						FunctionDefinition const* target = nullptr;
						// Resolve the SAME-signature implemented method on `this`
						// (the fn ref may point at an interface/base declaration;
						// dispatch wants the concrete impl by name + full sig).
						auto sameSig = [&](FunctionDefinition const* a, FunctionDefinition const* b) {
							if (a->parameters().size() != b->parameters().size())
								return false;
							for (size_t k = 0; k < a->parameters().size(); ++k)
								if (solTypeToArc4ParamName(_ctx, a->parameters()[k]->type())
									!= solTypeToArc4ParamName(_ctx, b->parameters()[k]->type()))
									return false;
							return true;
						};
						if (_ctx.currentContract)
						{
							if (!sigString.empty())
							{
								// encodeWithSignature("f(uint256)", ...): match the
								// candidate whose CANONICAL ARC4 signature equals the
								// given string exactly — so `f(uint256)` binds
								// f(uint256), not the first same-arity `f(bool)`.
								// Exact-only (no alias normalisation): a non-match
								// simply falls through to the name+arity behaviour,
								// so this can only fix a wrong bind, never regress.
								forEachDefinedFunction(*_ctx.currentContract, [&](auto const* func)
								{
									if (target) return;
									if (!func->isImplemented() || func->name() != fnName)
										return;
									std::string got = fnName + "(";
									for (size_t k = 0; k < func->parameters().size(); ++k)
									{
										if (k) got += ",";
										got += solTypeToArc4ParamName(_ctx, func->parameters()[k]->type());
									}
									got += ")";
									if (got == sigString)
										target = func;
								});
							}
							if (!target && refFunc)
							{
								// Exact overload known (encodeCall/encodeWithSelector
								// name a specific function): match name + full param
								// signature, not just arity — an f(uint256) ref must
								// not bind f(bool). Resolves the same-signature impl
								// on `this` (the ref may point at an interface/base).
								forEachDefinedFunction(*_ctx.currentContract, [&](auto const* func)
								{
									if (target) return;
									if (func->isImplemented() && func->name() == fnName
										&& sameSig(func, refFunc))
										target = func;
								});
							}
							// Fallback: name + arity. Unchanged behaviour, and the
							// only option for encodeWithSignature (its raw string
							// sig can't be canonicalised reliably — `uint` vs
							// `uint256`, etc.); ambiguity there is inherent.
							if (!target)
								forEachDefinedFunction(*_ctx.currentContract, [&](auto const* func)
								{
									if (target) return;
									if (func->isImplemented() && func->name() == fnName
										&& func->parameters().size() == nArgs)
										target = func;
								});
						}
						if (target)
						{
							if (targetIdentityExpr)
								_ctx.evaluateForEffects(*targetIdentityExpr, _loc);
							// AVM rejects self inner-txn calls; rewrite to direct callsub.
							// Revert isolation differs: reverts propagate instead of
							// being caught as success=false.
							Logger::instance().warning(
								"`address(this).call(abi." + encMA->memberName() +
								"(...))` self-call rewritten to direct `" + fnName +
								"(...)` invocation. AVM doesn't support self inner-txn "
								"calls; revert-isolation semantics may differ.",
								_loc);

							std::string targetName =
								CallResolver::resolveMethodName(_ctx, *target);
							size_t nReturns = target->returnParameters().size();
							auto pushResolvedArgs = [&](auto& call)
							{
								for (size_t i = 0; i < resolvedArgs.size(); ++i)
								{
									auto const& argument = resolvedArgs[i];
									auto value = _ctx.buildExpr(*argument);
									if (i < target->parameters().size())
									{
										auto const* parameterType = target->parameters()[i]->type();
										auto const* parameterWType = _ctx.typeMapper.map(parameterType);
										value = builder::ConversionPlan{
											argument->annotation().type,
											parameterType,
											parameterWType,
											builder::ConversionPlan::Context::Argument}.emit(
												std::move(value), _loc);
									}
									awst::pushCallArg(call->args, std::move(value));
								}
							};
							if (nReturns == 0)
							{
								auto call = awst::makeSubroutineCall(
									awst::InstanceMethodTarget{targetName},
									awst::WType::voidType(), _loc);
								pushResolvedArgs(call);
								auto stmt = awst::makeExpressionStatement(call, _loc);
								_ctx.preEffects().push_back(std::move(stmt));
								return std::make_unique<GenericResultBuilder>(_ctx,
									makeBoolBytesTuple(true, awst::makeBytesConstant({}, _loc), _loc));
							}
							if (nReturns == 1)
							{
								auto* retType = _ctx.typeMapper.map(target->returnParameters()[0]->type());
								if (!retType) retType = awst::WType::voidType();
								auto call = awst::makeSubroutineCall(
									awst::InstanceMethodTarget{targetName},
									retType, _loc);
								pushResolvedArgs(call);
								auto dataBytes = AbiEncoderBuilder::encodeValuesAsEvmAbi(
									_ctx, {target->returnParameters()[0]->type()},
									{std::move(call)}, _loc);
								return std::make_unique<GenericResultBuilder>(_ctx,
									makeBoolBytesTuple(true, std::move(dataBytes), _loc));
							}

							// Multi-return: cache the call once, then let the recursive
							// canonical encoder lay out all static/dynamic return values.
							std::vector<awst::WType const*> tupleTypes;
							std::vector<solidity::frontend::Type const*> returnTypes;
							for (auto const& ret : target->returnParameters())
							{
								auto* pt = _ctx.typeMapper.map(ret->type());
								tupleTypes.push_back(pt ? pt : awst::WType::voidType());
								returnTypes.push_back(ret->type());
							}
							auto* tupleTypeOwned = _ctx.typeMapper.createType<awst::WTuple>(
								std::move(tupleTypes));
							auto call = awst::makeSubroutineCall(
								awst::InstanceMethodTarget{targetName},
								tupleTypeOwned, _loc);
							pushResolvedArgs(call);
							// Intentionally RAW makeSingleEvaluation, not makeEvalOnce: the fresh
							// SE id is IDENTITY-FORCING — it prevents two attrs-equal calls from
							// merging (see sol-ast-audit) — so the wrap must be unconditional;
							// makeEvalOnce's skip-leaf contract must never apply here.
							auto cachedCall = awst::makeSingleEvaluation(
								std::move(call), tupleTypeOwned, awst::nextSingleEvalId(), _loc);

							std::vector<std::shared_ptr<awst::Expression>> values;
							for (size_t i = 0; i < nReturns; ++i)
							{
								auto* itemType = tupleTypeOwned->types()[i];
								auto item = awst::makeTupleItem(cachedCall, static_cast<int>(i), itemType, _loc);
								values.push_back(std::move(item));
							}
							auto dataBytes = AbiEncoderBuilder::encodeValuesAsEvmAbi(
								_ctx, returnTypes, std::move(values), _loc);
							return std::make_unique<GenericResultBuilder>(_ctx,
								makeBoolBytesTuple(true, std::move(dataBytes), _loc));
						}
					}
				}
			}
		}

		if (auto const* encodeCallExpr = dynamic_cast<FunctionCall const*>(&dataArg))
		{
			auto const* encodeMA = dynamic_cast<MemberAccess const*>(&encodeCallExpr->expression());
			if (encodeMA && encodeMA->memberName() == "encodeCall" && encodeCallExpr->arguments().size() >= 2)
			{
				auto result = handleCallWithEncodeCall(_ctx, _receiver, *encodeCallExpr, _callValue, _loc);
				if (result) return result;
			}
			// .call(abi.encodeWithSignature/WithSelector(...)): encoder visible at call site —
			// preserve the declared argument types while adapting the byte blob to
			// the selected contract-entry transport.
			if (encodeMA
				&& (encodeMA->memberName() == "encodeWithSignature"
					|| encodeMA->memberName() == "encodeWithSelector")
				&& !encodeCallExpr->arguments().empty())
			{
				auto result = handleCallWithSignatureArgs(
					_ctx, _receiver, *encodeCallExpr,
					encodeMA->memberName() == "encodeWithSignature", _callValue, _loc);
				if (result) return result;
			}
		}

		// .call(data) to known precompile address → route like .staticcall
		if (auto precompileAddr = detectPrecompileAddress(_baseExpr))
		{
			auto inputData = _ctx.buildExpr(dataArg);
			auto result = handleStaticCallPrecompile(_ctx, *precompileAddr, std::move(inputData), _loc);
			if (result) return result;
		}
		// Non-encodeCall self-call: an exact route exists — non-selector data
		// reaches the fallback in the approval program, so call __fallback
		// directly. Not an UnknownLowLevelCall: the target is proven (self).
		bool isSelfCall = false;
		if (auto const* intrinsic = dynamic_cast<awst::IntrinsicCall const*>(_receiver.get()))
		{
			if (intrinsic->opCode == "global" && !intrinsic->immediates.empty())
			{
				auto const* imm = std::get_if<std::string>(&intrinsic->immediates[0]);
				if (imm && *imm == "CurrentApplicationAddress")
					isSelfCall = true;
			}
		}

		if (isSelfCall)
		{
			auto dataExpr = _ctx.buildExpr(dataArg);
			if (dataExpr->wtype == awst::WType::stringType())
			{
				auto cast = awst::makeAsBytes(std::move(dataExpr), _loc);
				dataExpr = std::move(cast);
			}

			// Only route to __fallback if the contract defines one; otherwise
			// an InstanceMethodTarget{"__fallback"} would be unresolvable.
			solidity::frontend::FunctionDefinition const* fallbackFunc = nullptr;
			if (_ctx.currentContract)
			{
				forEachDefinedFunction(*_ctx.currentContract, [&](auto const* func)
				{
					if (fallbackFunc) return;
					if (func->isImplemented() && func->isFallback())
						fallbackFunc = func;
				});
			}

			if (!fallbackFunc)
			{
				return std::make_unique<GenericResultBuilder>(_ctx,
					makeBoolBytesTupleEmpty(_loc));
			}

			bool fallbackTakesBytes = fallbackFunc->parameters().size() == 1;
			bool fallbackReturnsBytes = !fallbackFunc->returnParameters().empty();

			auto call = awst::makeSubroutineCall(awst::InstanceMethodTarget{"__fallback"}, fallbackReturnsBytes ? awst::WType::bytesType() : awst::WType::voidType(), _loc);
			if (fallbackTakesBytes)
				awst::pushCallArg(call->args, dataExpr);

			// Spill bytes-returning fallback result to a temp.
			if (fallbackReturnsBytes)
			{
				std::string tmpName = "__fallback_ret_" + std::to_string((awst::NameGen::next("InnerCallHandlers.s_tmpCounter") + 1));
				auto tmpTarget = awst::makeVarExpression(tmpName, awst::WType::bytesType(), _loc);
				auto assign = awst::makeAssignmentStatement(tmpTarget, std::move(call), _loc);
				_ctx.preEffects().push_back(std::move(assign));

				auto retRead = awst::makeVarExpression(tmpName, awst::WType::bytesType(), _loc);
				return std::make_unique<GenericResultBuilder>(_ctx,
					makeBoolBytesTuple(true, std::move(retRead), _loc));
			}

			auto stmt = awst::makeExpressionStatement(call, _loc);
			_ctx.preEffects().push_back(std::move(stmt));

			return std::make_unique<GenericResultBuilder>(_ctx,
				makeBoolBytesTuple(true, awst::makeBytesConstant({}, _loc), _loc));
		}

		// Non-self raw .call(data) → inner app call; splits [selector, rest].
		// Empty calls are only exactly decidable for the literal zero address;
		// other addresses need open-world account/code state that AVM does not expose.
		auto dataExpr = _ctx.buildExpr(dataArg);
		auto isEmptyConst = [](awst::Expression const* e) {
			// Unwrap ReinterpretCast (string→bytes, etc.) to inspect the inner.
			while (auto const* rc = dynamic_cast<awst::ReinterpretCast const*>(e))
				e = rc->expr.get();
			if (auto const* bc = dynamic_cast<awst::BytesConstant const*>(e))
				return bc->value.empty();
			if (auto const* sc = dynamic_cast<awst::StringConstant const*>(e))
				return sc->value.empty();
			return false;
		};
		if (isEmptyConst(dataExpr.get()))
		{
			// Empty data + {value:} = plain transfer (EVM: invokes receive()).
			if (_callValue)
				return handleCallWithValue(_ctx, std::move(_receiver), std::move(_callValue), _loc);
			if (isLiteralZeroAddress(_baseExpr))
				return std::make_unique<GenericResultBuilder>(_ctx,
					makeBoolBytesTupleEmpty(_loc));
			EvmFeaturePolicy::report(
				EvmFeature::UnknownLowLevelCall,
				_ctx.typeMapper.profile(), _loc);
			return std::make_unique<GenericResultBuilder>(_ctx,
				makeBoolBytesTuple(
					false, awst::makeBytesConstant({}, _loc), _loc));
		}
		return handleCallWithRawData(_ctx, _receiver, std::move(dataExpr), std::move(_callValue), _loc);
	}

	if (_memberName == "staticcall")
	{
		auto precompileAddr = detectPrecompileAddress(_baseExpr);
		if (precompileAddr && !_callNode.arguments().empty())
		{
			auto inputData = _ctx.buildExpr(*_callNode.arguments()[0]);
			auto result = handleStaticCallPrecompile(_ctx, *precompileAddr, std::move(inputData), _loc);
			if (result) return result;
		}

		// Hard error: stubbing as (true, "") would make require(ok) pass spuriously.
		for (auto const& arg : _callNode.arguments())
			_ctx.buildExpr(*arg);
		EvmFeaturePolicy::report(
			EvmFeature::UnknownLowLevelCall,
			_ctx.typeMapper.profile(), _loc);
		return std::make_unique<GenericResultBuilder>(_ctx,
			makeBoolBytesTuple(
				false, awst::makeBytesConstant({}, _loc), _loc));
	}

	if (_memberName == "delegatecall")
		return handleDelegatecall(_ctx, _callNode, _loc);

	return nullptr;
}

void InnerCallHandlers::fundCreatedApp(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _amount,
	awst::SourceLocation const& _loc)
{
	auto appId = awst::makeItxn("CreatedApplicationID", awst::WType::uint64Type(), _loc);

	auto* tupleType = _ctx.typeMapper.createType<awst::WTuple>(
		std::vector<awst::WType const*>{awst::WType::bytesType(), awst::WType::boolType()});
	auto appParams = awst::makeAppParamsGet(
		"AppAddress", std::move(appId), tupleType, _loc);

	auto addrBytes = awst::makeTupleItem(std::move(appParams), 0, awst::WType::bytesType(), _loc);

	auto receiver = awst::makeAsAccount(std::move(addrBytes), _loc);

	auto create = buildPaymentTransaction(_ctx, std::move(receiver), std::move(_amount), _loc);
	static awst::WInnerTransaction s_payTxnType(TxnTypePay);
	auto submit = awst::makeSubmitInnerTransaction(&s_payTxnType, _loc);
	submit->itxns.push_back(std::move(create));

	auto stmt = awst::makeExpressionStatement(std::move(submit), _loc);
	_ctx.preEffects().push_back(std::move(stmt));
}

} // namespace puyasol::builder::eb
