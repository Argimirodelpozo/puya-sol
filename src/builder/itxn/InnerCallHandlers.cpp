/// @file InnerCallHandlers.cpp
/// Handles address.call/staticcall/delegatecall/transfer inner transaction patterns
/// and precompile routing.

#include "builder/itxn/InnerCallHandlers.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/itxn/InnerCallInternal.h"
#include "builder/sol-eb/SolBoolBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

namespace puyasol::builder::eb
{

// ── Helpers ──

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

std::shared_ptr<awst::Expression> InnerCallHandlers::encodeArgToBytes(
	std::shared_ptr<awst::Expression> _arg, awst::SourceLocation const& _loc)
{
	auto* wtype = _arg->wtype;
	if (wtype == awst::WType::bytesType() || (wtype && wtype->kind() == awst::WTypeKind::Bytes))
		return _arg;

	if (wtype == awst::WType::uint64Type())
		return awst::makeItob(std::move(_arg), _loc);

	if (wtype == awst::WType::biguintType())
	{
		// biguint is minimal big-endian; ABI uint256 is exactly 32 bytes.
		auto cast = awst::makeAsBytes(std::move(_arg), _loc);
		return awst::makeLeftPadToN(std::move(cast), 32, _loc);
	}

	if (wtype == awst::WType::boolType())
	{
		auto setbit = awst::makeSetbit(
			awst::makeBytesConstant({0x00}, _loc),
			awst::makeZero(_loc),
			std::move(_arg), _loc);
		return setbit;
	}

	if (wtype == awst::WType::accountType())
	{
		auto cast = awst::makeAsBytes(std::move(_arg), _loc);
		return cast;
	}

	// Fallback: reinterpret as bytes
	auto cast = awst::makeAsBytes(std::move(_arg), _loc);
	return cast;
}

namespace
{

// Nested ARC4 type name (struct-field / array-element position).
// Differs from top-level: exact bit width (not collapsed to uint64),
// signedness preserved (e.g. nested int8 = "int8", not "uint8").
// Verified against puya's `method "..."` output.
std::string nestedArc4Name(
	ContractContext& _ctx, solidity::frontend::Type const* _type)
{
	using namespace solidity::frontend;
	if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(_type))
		_type = &udvt->underlyingType();
	if (auto const* intT = dynamic_cast<IntegerType const*>(_type))
		return (intT->isSigned() ? "int" : "uint") + std::to_string(intT->numBits());
	if (auto const* enumT = dynamic_cast<EnumType const*>(_type))
	{
		auto const* enc = dynamic_cast<IntegerType const*>(enumT->encodingType());
		return "uint" + std::to_string(enc ? enc->numBits() : 8u);
	}
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
	return _type->toString(true);
}

// Top-level param name: scalars collapse to "uint64"/"uintN" (signedness dropped);
// enums → "uint64"; aggregates expand via nestedArc4Name to match puya's tuple form.
// (A plain struct emitted "struct P" previously, silently breaking dispatch.)
std::string solTypeToARC4Impl(
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
	if (wtype->kind() == awst::WTypeKind::Bytes)
	{
		auto const* bw = static_cast<awst::BytesWType const*>(wtype);
		if (bw->length().has_value())
			return "byte[" + std::to_string(bw->length().value()) + "]";
		return "byte[]";
	}
	if (dynamic_cast<solidity::frontend::StructType const*>(_type)
		|| dynamic_cast<solidity::frontend::ArrayType const*>(_type))
		return nestedArc4Name(_ctx, _type);
	return _type->toString(true);
}

std::string solTypeToARC4RetImpl(
	ContractContext& _ctx, solidity::frontend::Type const* _type)
{
	if (auto name = builder::TypeCoercion::intSelectorReturnName(_type))
		return *name;
	return solTypeToARC4Impl(_ctx, _type);
}

} // namespace

std::string InnerCallHandlers::buildMethodSelector(
	ContractContext& _ctx,
	std::string const& _name,
	solidity::frontend::FunctionType const& _funcType)
{
	std::string sel = _name + "(";
	bool first = true;
	for (auto const& paramType : _funcType.parameterTypes())
	{
		if (!first) sel += ",";
		sel += solTypeToARC4Impl(_ctx, paramType);
		first = false;
	}
	sel += ")";

	auto const& rets = _funcType.returnParameterTypes();
	if (rets.size() > 1)
	{
		sel += "(";
		bool firstRet = true;
		for (auto const& retType : rets)
		{
			if (!firstRet) sel += ",";
			sel += solTypeToARC4RetImpl(_ctx, retType);
			firstRet = false;
		}
		sel += ")";
	}
	else if (rets.size() == 1)
		sel += solTypeToARC4RetImpl(_ctx, rets[0]);
	else
		sel += "void";

	return sel;
}

std::string InnerCallHandlers::buildMethodSelector(
	ContractContext& _ctx,
	solidity::frontend::FunctionDefinition const* _func)
{
	auto solTypeToARC4 = [&](solidity::frontend::Type const* _type) {
		return solTypeToARC4Impl(_ctx, _type);
	};
	auto solTypeToARC4Ret = [&](solidity::frontend::Type const* _type) {
		return solTypeToARC4RetImpl(_ctx, _type);
	};

	std::string sel = _func->name() + "(";
	bool first = true;
	for (auto const& param : _func->parameters())
	{
		if (!first) sel += ",";
		sel += solTypeToARC4(param->type());
		first = false;
	}
	sel += ")";

	if (_func->returnParameters().size() > 1)
	{
		sel += "(";
		bool firstRet = true;
		for (auto const& retParam : _func->returnParameters())
		{
			if (!firstRet) sel += ",";
			sel += solTypeToARC4Ret(retParam->type());
			firstRet = false;
		}
		sel += ")";
	}
	else if (_func->returnParameters().size() == 1)
		sel += solTypeToARC4Ret(_func->returnParameters()[0]->type());
	else
		sel += "void";

	return sel;
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
	_ctx.pendingStatements.push_back(std::move(stmt));

	auto vc = awst::makeVoidConstant(_loc);
	return std::make_unique<GenericResultBuilder>(_ctx, std::move(vc));
}

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleSend(
	ContractContext& _ctx, std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount, awst::SourceLocation const& _loc)
{
	auto create = buildPaymentTransaction(_ctx, std::move(_receiver), std::move(_amount), _loc);
	static awst::WInnerTransaction s_payTxnType(TxnTypePay);
	auto submit = awst::makeSubmitInnerTransaction(&s_payTxnType, _loc);
	submit->itxns.push_back(std::move(create));

	auto stmt = awst::makeExpressionStatement(submit, _loc);
	_ctx.pendingStatements.push_back(std::move(stmt));

	return std::make_unique<SolBoolBuilder>(_ctx, awst::makeTrue(_loc));
}

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleCallWithValue(
	ContractContext& _ctx, std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount, awst::SourceLocation const& _loc)
{
	auto create = buildPaymentTransaction(_ctx, std::move(_receiver), std::move(_amount), _loc);
	static awst::WInnerTransaction s_payTxnType(TxnTypePay);
	auto submit = awst::makeSubmitInnerTransaction(&s_payTxnType, _loc);
	submit->itxns.push_back(std::move(create));

	auto stmt = awst::makeExpressionStatement(submit, _loc);
	_ctx.pendingStatements.push_back(std::move(stmt));

	return std::make_unique<GenericResultBuilder>(_ctx, makeBoolBytesTupleEmpty(_loc));
}

// ── .call(abi.encodeCall(...)) ──

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleDelegatecall(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc)
{
	// AVM has no DELEGATECALL: every app has isolated storage, every inner txn
	// has its own caller. Hard-error rather than silently wrong stub.
	Logger::instance().error(
		"`.delegatecall(...)` is not supported on AVM. DELEGATECALL's "
		"shared-storage / caller-preservation semantics have no equivalent "
		"on AVM (each app has isolated storage; inner-txn callers are the "
		"calling app, not its caller). Rewrite the call site, or use "
		"`.call(...)` if the caller-of-caller distinction isn't load-bearing.",
		_loc);
	for (auto const& arg : _callNode.arguments())
		_ctx.buildExpr(*arg);
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
		amount = TypeCoercion::implicitNumericCast(std::move(amount), awst::WType::uint64Type(), _loc);
		return handleTransfer(_ctx, std::move(_receiver), std::move(amount), _loc);
	}

	// .send(amount)
	if (_memberName == "send" && _callNode.arguments().size() == 1)
	{
		auto amount = _ctx.buildExpr(*_callNode.arguments()[0]);
		amount = TypeCoercion::implicitNumericCast(std::move(amount), awst::WType::uint64Type(), _loc);
		return handleSend(_ctx, std::move(_receiver), std::move(amount), _loc);
	}

	// .call{value: X}("") → payment
	if (_memberName == "call" && _callValue)
		return handleCallWithValue(_ctx, std::move(_receiver), std::move(_callValue), _loc);

	// .call(abi.encodeCall(...)) → inner app call
	if (_memberName == "call" && !_callValue && !_callNode.arguments().empty())
	{
		auto const& dataArg = *_callNode.arguments()[0];

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
					size_t firstArgIdx = 1;  // index in encCallExpr args where method args start
					if (encMA && encMA->memberName() == "encodeWithSignature"
						&& !encCallExpr->arguments().empty())
					{
						if (auto const* sigLit = dynamic_cast<Literal const*>(encCallExpr->arguments()[0].get()))
						{
							std::string sig = sigLit->value();
							auto parenPos = sig.find('(');
							if (parenPos != std::string::npos)
								fnName = sig.substr(0, parenPos);
						}
					}
					else if (encMA && encMA->memberName() == "encodeWithSelector"
						&& !encCallExpr->arguments().empty())
					{
						// `this.fn.selector` = MemberAccess("selector", MemberAccess("fn", this)).
						if (auto const* selMA = dynamic_cast<MemberAccess const*>(encCallExpr->arguments()[0].get()))
						{
							if (selMA->memberName() == "selector")
							{
								if (auto const* fnMA = dynamic_cast<MemberAccess const*>(&selMA->expression()))
								{
									fnName = fnMA->memberName();
								}
							}
						}
					}
					if (!fnName.empty())
					{
						size_t nArgs = encCallExpr->arguments().size() - firstArgIdx;
						FunctionDefinition const* target = nullptr;
						if (_ctx.currentContract)
						{
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
							// AVM rejects self inner-txn calls; rewrite to direct callsub.
							// Revert isolation differs: reverts propagate instead of
							// being caught as success=false.
							Logger::instance().warning(
								"`address(this).call(abi.encode" +
								std::string(encMA->memberName() == "encodeWithSelector"
									? "WithSelector" : "WithSignature") +
								"(...))` self-call rewritten to direct `" + fnName +
								"(...)` invocation. AVM doesn't support self inner-txn "
								"calls; revert-isolation semantics may differ.",
								_loc);

							// Encode a return value as exactly 32 bytes (EVM ABI / abi.decode shape).
							// makeLeftPadToN ensures exactly 32 even when biguint minimal-rep is shorter.
							auto encodeAs32 = [&_loc](std::shared_ptr<awst::Expression> v)
								-> std::shared_ptr<awst::Expression>
							{
								if (v->wtype == awst::WType::uint64Type())
								{
									auto bytes = awst::makeItob(std::move(v), _loc);
									return awst::makeLeftPadToN(std::move(bytes), 32, _loc);
								}
								if (v->wtype == awst::WType::biguintType())
								{
									auto bytes = awst::makeAsBytes(std::move(v), _loc);
									return awst::makeLeftPadToN(std::move(bytes), 32, _loc);
								}
								if (v->wtype == awst::WType::boolType())
								{
									auto asInt = awst::makeAsUInt64(std::move(v), _loc);
									auto bytes = awst::makeItob(std::move(asInt), _loc);
									return awst::makeLeftPadToN(std::move(bytes), 32, _loc);
								}
								if (v->wtype && v->wtype->kind() == awst::WTypeKind::Bytes)
								{
									// bytesN: right-pad to 32 (EVM convention).
									auto bw = dynamic_cast<awst::BytesWType const*>(v->wtype);
									int len = bw && bw->length() ? *bw->length() : 32;
									auto bytes = awst::makeAsBytes(std::move(v), _loc);
									if (len < 32)
										return awst::makeRightPad(std::move(bytes), 32 - len, _loc);
									return bytes;
								}
								return awst::makeAsBytes(std::move(v), _loc);
							};

							size_t nReturns = target->returnParameters().size();
							if (nReturns == 0)
							{
								auto call = awst::makeSubroutineCall(
									awst::InstanceMethodTarget{target->name()},
									awst::WType::voidType(), _loc);
								for (size_t i = firstArgIdx; i < encCallExpr->arguments().size(); ++i)
									awst::pushCallArg(call->args,
										_ctx.buildExpr(*encCallExpr->arguments()[i]));
								auto stmt = awst::makeExpressionStatement(call, _loc);
								_ctx.prePendingStatements.push_back(std::move(stmt));
								return std::make_unique<GenericResultBuilder>(_ctx,
									makeBoolBytesTuple(true, awst::makeBytesConstant({}, _loc), _loc));
							}
							if (nReturns == 1)
							{
								auto* retType = _ctx.typeMapper.map(target->returnParameters()[0]->type());
								if (!retType) retType = awst::WType::voidType();
								auto call = awst::makeSubroutineCall(
									awst::InstanceMethodTarget{target->name()},
									retType, _loc);
								for (size_t i = firstArgIdx; i < encCallExpr->arguments().size(); ++i)
									awst::pushCallArg(call->args,
										_ctx.buildExpr(*encCallExpr->arguments()[i]));
								auto dataBytes = encodeAs32(std::move(call));
								return std::make_unique<GenericResultBuilder>(_ctx,
									makeBoolBytesTuple(true, std::move(dataBytes), _loc));
							}

							// Multi-return: SingleEvaluation so call runs once;
							// ABI-encode each return element to 32 bytes and concat.
							std::vector<awst::WType const*> tupleTypes;
							for (auto const& ret : target->returnParameters())
							{
								auto* pt = _ctx.typeMapper.map(ret->type());
								tupleTypes.push_back(pt ? pt : awst::WType::voidType());
							}
							auto* tupleTypeOwned = new awst::WTuple(std::move(tupleTypes));
							auto call = awst::makeSubroutineCall(
								awst::InstanceMethodTarget{target->name()},
								tupleTypeOwned, _loc);
							for (size_t i = firstArgIdx; i < encCallExpr->arguments().size(); ++i)
								awst::pushCallArg(call->args,
									_ctx.buildExpr(*encCallExpr->arguments()[i]));
							// nextSingleEvalId() prevents identical calls from merging (see sol-ast-audit).
							auto cachedCall = awst::makeSingleEvaluation(
								std::move(call), tupleTypeOwned, awst::nextSingleEvalId(), _loc);

							std::vector<std::shared_ptr<awst::Expression>> parts;
							for (size_t i = 0; i < nReturns; ++i)
							{
								auto* itemType = tupleTypeOwned->types()[i];
								auto item = awst::makeTupleItem(cachedCall, static_cast<int>(i), itemType, _loc);
								parts.push_back(encodeAs32(std::move(item)));
							}
							std::shared_ptr<awst::Expression> dataBytes = std::move(parts[0]);
							for (size_t i = 1; i < parts.size(); ++i)
								dataBytes = awst::makeConcat(std::move(dataBytes), std::move(parts[i]), _loc);
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
				auto result = handleCallWithEncodeCall(_ctx, _receiver, *encodeCallExpr, _loc);
				if (result) return result;
			}
			// .call(abi.encodeWithSignature/WithSelector(...)): encoder visible at call site —
			// re-encode as typed inner call instead of forwarding an EVM-shaped blob.
			// See EVM_DIVERGENCE.md "Encoding model" #1.
			if (encodeMA
				&& (encodeMA->memberName() == "encodeWithSignature"
					|| encodeMA->memberName() == "encodeWithSelector")
				&& !encodeCallExpr->arguments().empty())
			{
				auto result = handleCallWithSignatureArgs(
					_ctx, _receiver, *encodeCallExpr,
					encodeMA->memberName() == "encodeWithSignature", _loc);
				if (result) return result;
			}
		}

		// .call(data) to known precompile address → route like .staticcall
		{
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
			if (precompileAddr)
			{
				auto inputData = _ctx.buildExpr(dataArg);
				auto result = handleStaticCallPrecompile(_ctx, *precompileAddr, std::move(inputData), _loc);
				if (result) return result;
			}
		}
		// Non-encodeCall self-call: route to __fallback (non-selector data
		// would reach fallback in the approval program anyway).
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
				static int s_tmpCounter = 0;
				std::string tmpName = "__fallback_ret_" + std::to_string(++s_tmpCounter);
				auto tmpTarget = awst::makeVarExpression(tmpName, awst::WType::bytesType(), _loc);
				auto assign = awst::makeAssignmentStatement(tmpTarget, std::move(call), _loc);
				_ctx.prePendingStatements.push_back(std::move(assign));

				auto retRead = awst::makeVarExpression(tmpName, awst::WType::bytesType(), _loc);
				return std::make_unique<GenericResultBuilder>(_ctx,
					makeBoolBytesTuple(true, std::move(retRead), _loc));
			}

			auto stmt = awst::makeExpressionStatement(call, _loc);
			_ctx.prePendingStatements.push_back(std::move(stmt));

			return std::make_unique<GenericResultBuilder>(_ctx,
				makeBoolBytesTuple(true, awst::makeBytesConstant({}, _loc), _loc));
		}

		// Non-self raw .call(data) → inner app call; splits [selector, rest].
		// Compile-time empty literal → stub (true, "") to match EVM "call to
		// non-contract returns true" (bare_call_no_returndatacopy.sol,
		// calling_nonexisting_contract_throws.sol).
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
			return std::make_unique<GenericResultBuilder>(_ctx,
				makeBoolBytesTupleEmpty(_loc));
		}
		return handleCallWithRawData(_ctx, _receiver, std::move(dataExpr), _loc);
	}

	if (_memberName == "staticcall")
	{
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

		if (precompileAddr && !_callNode.arguments().empty())
		{
			auto inputData = _ctx.buildExpr(*_callNode.arguments()[0]);
			auto result = handleStaticCallPrecompile(_ctx, *precompileAddr, std::move(inputData), _loc);
			if (result) return result;
		}

		// Hard error: stubbing as (true, "") would make require(ok) pass spuriously.
		for (auto const& arg : _callNode.arguments())
			_ctx.buildExpr(*arg);
		Logger::instance().error(
			"`address.staticcall(data)` to this target is not supported on AVM. "
			"Only precompile addresses 0x01–0x08 are handled; any other target "
			"would be stubbed as `(true, \"\")`, which makes `require(ok)` pass "
			"spuriously and yields all-zero returndata.", _loc);
		return std::make_unique<GenericResultBuilder>(_ctx, makeBoolBytesTupleEmpty(_loc));
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

	auto* tupleType = new awst::WTuple({awst::WType::bytesType(), awst::WType::boolType()});
	auto appParams = awst::makeAppParamsGet(
		"AppAddress", std::move(appId), tupleType, _loc);

	auto addrBytes = awst::makeTupleItem(std::move(appParams), 0, awst::WType::bytesType(), _loc);

	auto receiver = awst::makeAsAccount(std::move(addrBytes), _loc);

	auto create = buildPaymentTransaction(_ctx, std::move(receiver), std::move(_amount), _loc);
	static awst::WInnerTransaction s_payTxnType(TxnTypePay);
	auto submit = awst::makeSubmitInnerTransaction(&s_payTxnType, _loc);
	submit->itxns.push_back(std::move(create));

	auto stmt = awst::makeExpressionStatement(std::move(submit), _loc);
	_ctx.prePendingStatements.push_back(std::move(stmt));
}

} // namespace puyasol::builder::eb
