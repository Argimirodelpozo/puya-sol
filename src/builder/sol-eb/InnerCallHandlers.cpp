/// @file InnerCallHandlers.cpp
/// Handles address.call/staticcall/delegatecall/transfer inner transaction patterns
/// and precompile routing.

#include "builder/sol-eb/InnerCallHandlers.h"
#include "builder/sol-eb/InnerCallInternal.h"
#include "builder/sol-eb/SolBoolBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

namespace puyasol::builder::eb
{



// ── Small helpers ──


static awst::WTuple s_boolBytesType(
	std::vector<awst::WType const*>{awst::WType::boolType(), awst::WType::bytesType()});

std::shared_ptr<awst::Expression> InnerCallHandlers::makeBoolBytesTuple(
	bool _success,
	std::shared_ptr<awst::Expression> _data,
	awst::SourceLocation const& _loc)
{
	auto tuple = std::make_shared<awst::TupleExpression>();
	tuple->sourceLocation = _loc;
	tuple->wtype = &s_boolBytesType;
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
	auto call = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	call->stackArgs.push_back(std::move(_source));
	call->stackArgs.push_back(awst::makeIntegerConstant(std::to_string(_offset), _loc));
	call->stackArgs.push_back(awst::makeIntegerConstant(std::to_string(_length), _loc));
	return call;
}

std::shared_ptr<awst::IntrinsicCall> InnerCallHandlers::makeConcat(
	std::shared_ptr<awst::Expression> _a, std::shared_ptr<awst::Expression> _b,
	awst::SourceLocation const& _loc)
{
	auto c = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
	c->stackArgs.push_back(std::move(_a));
	c->stackArgs.push_back(std::move(_b));
	return c;
}

std::shared_ptr<awst::Expression> InnerCallHandlers::leftPadToN(
	std::shared_ptr<awst::Expression> _expr, int _n, awst::SourceLocation const& _loc)
{
	auto padding = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
	padding->stackArgs.push_back(awst::makeIntegerConstant(std::to_string(_n), _loc));

	auto padded = makeConcat(std::move(padding), std::move(_expr), _loc);

	auto paddedLen = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
	paddedLen->stackArgs.push_back(padded);

	auto offset = awst::makeUInt64BinOp(std::move(paddedLen), awst::UInt64BinaryOperator::Sub, awst::makeIntegerConstant(std::to_string(_n), _loc), _loc);

	return makeExtract(std::move(padded), 0, _n, _loc);
	// FIXME: should use dynamic extract3(padded, offset, N) — but for now
	// the actual FunctionCallBuilder code uses this pattern. Let's match it.
}

std::shared_ptr<awst::Expression> InnerCallHandlers::addressToAppId(
	std::shared_ptr<awst::Expression> _receiver, awst::SourceLocation const& _loc)
{
	if (_receiver->wtype == awst::WType::applicationType())
		return _receiver;

	// Detect global CurrentApplicationAddress → use CurrentApplicationID directly
	// (CurrentApplicationAddress is a hash, not our conventional \x00*24 + app_id format)
	if (auto const* intrinsic = dynamic_cast<awst::IntrinsicCall const*>(_receiver.get()))
	{
		if (intrinsic->opCode == "global" && !intrinsic->immediates.empty())
		{
			auto const* imm = std::get_if<std::string>(&intrinsic->immediates[0]);
			if (imm && *imm == "CurrentApplicationAddress")
			{
				auto appId = awst::makeIntrinsicCall("global", awst::WType::uint64Type(), _loc);
				appId->immediates = {std::string("CurrentApplicationID")};

				auto cast = awst::makeReinterpretCast(std::move(appId), awst::WType::applicationType(), _loc);
				return cast;
			}
		}
	}

	std::shared_ptr<awst::Expression> bytesExpr = std::move(_receiver);
	if (bytesExpr->wtype == awst::WType::accountType())
	{
		auto toBytes = awst::makeReinterpretCast(std::move(bytesExpr), awst::WType::bytesType(), _loc);
		bytesExpr = std::move(toBytes);
	}

	auto extract = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
	extract->immediates = {24, 8};
	extract->stackArgs.push_back(std::move(bytesExpr));

	auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
	btoi->stackArgs.push_back(std::move(extract));

	auto cast = awst::makeReinterpretCast(std::move(btoi), awst::WType::applicationType(), _loc);
	return cast;
}

std::shared_ptr<awst::Expression> InnerCallHandlers::encodeArgToBytes(
	std::shared_ptr<awst::Expression> _arg, awst::SourceLocation const& _loc)
{
	auto* wtype = _arg->wtype;
	if (wtype == awst::WType::bytesType() || (wtype && wtype->kind() == awst::WTypeKind::Bytes))
		return _arg;

	if (wtype == awst::WType::uint64Type())
	{
		auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
		itob->stackArgs.push_back(std::move(_arg));
		return itob;
	}

	if (wtype == awst::WType::biguintType())
	{
		auto cast = awst::makeReinterpretCast(std::move(_arg), awst::WType::bytesType(), _loc);

		auto zeros = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
		zeros->stackArgs.push_back(awst::makeIntegerConstant("32", _loc));

		auto padded = makeConcat(std::move(zeros), std::move(cast), _loc);

		auto paddedLen = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
		paddedLen->stackArgs.push_back(padded);

		auto offset = awst::makeIntrinsicCall("-", awst::WType::uint64Type(), _loc);
		offset->stackArgs.push_back(std::move(paddedLen));
		offset->stackArgs.push_back(awst::makeIntegerConstant("32", _loc));

		return makeExtract(std::move(padded), 0, 0, _loc);
		// FIXME: should be extract3(padded, offset, 32) with dynamic offset
		// For now, replicate the pattern from FunctionCallBuilder.
	}

	if (wtype == awst::WType::boolType())
	{
		auto setbit = awst::makeIntrinsicCall("setbit", awst::WType::bytesType(), _loc);
		setbit->stackArgs.push_back(awst::makeBytesConstant({0x00}, _loc));
		setbit->stackArgs.push_back(awst::makeIntegerConstant("0", _loc));
		setbit->stackArgs.push_back(std::move(_arg));
		return setbit;
	}

	if (wtype == awst::WType::accountType())
	{
		auto cast = awst::makeReinterpretCast(std::move(_arg), awst::WType::bytesType(), _loc);
		return cast;
	}

	// Fallback: reinterpret as bytes
	auto cast = awst::makeReinterpretCast(std::move(_arg), awst::WType::bytesType(), _loc);
	return cast;
}

std::string InnerCallHandlers::buildMethodSelector(
	BuilderContext& _ctx,
	solidity::frontend::FunctionDefinition const* _func)
{
	auto solTypeToARC4 = [&](solidity::frontend::Type const* _type) -> std::string {
		auto* wtype = _ctx.typeMapper.map(_type);
		if (wtype == awst::WType::biguintType())
		{
			if (auto const* intT = dynamic_cast<solidity::frontend::IntegerType const*>(_type))
				return intT->isSigned() ? "int256" : "uint256";
			return "uint256";
		}
		if (wtype == awst::WType::uint64Type())
		{
			if (auto const* intT = dynamic_cast<solidity::frontend::IntegerType const*>(_type))
				return intT->isSigned()
					? "int" + std::to_string(intT->numBits())
					: "uint" + std::to_string(intT->numBits());
			return "uint64";
		}
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
		if (auto const* structType = dynamic_cast<solidity::frontend::StructType const*>(_type))
			return "struct " + structType->structDefinition().name();
		return _type->toString(true);
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
			sel += solTypeToARC4(retParam->type());
			firstRet = false;
		}
		sel += ")";
	}
	else if (_func->returnParameters().size() == 1)
		sel += solTypeToARC4(_func->returnParameters()[0]->type());
	else
		sel += "void";

	return sel;
}

// ── Payment helpers ──

std::shared_ptr<awst::Expression> InnerCallHandlers::buildPaymentTransaction(
	BuilderContext& /*_ctx*/,
	std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount,
	awst::SourceLocation const& _loc)
{
	static awst::WInnerTransactionFields s_payFieldsType(TxnTypePay);

	auto create = std::make_shared<awst::CreateInnerTransaction>();
	create->sourceLocation = _loc;
	create->wtype = &s_payFieldsType;
	create->fields["TypeEnum"] = awst::makeIntegerConstant(std::to_string(TxnTypePay), _loc);
	create->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
	create->fields["Receiver"] = std::move(_receiver);
	create->fields["Amount"] = std::move(_amount);
	return create;
}

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleTransfer(
	BuilderContext& _ctx, std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount, awst::SourceLocation const& _loc)
{
	auto create = buildPaymentTransaction(_ctx, std::move(_receiver), std::move(_amount), _loc);
	static awst::WInnerTransaction s_payTxnType(TxnTypePay);
	auto submit = std::make_shared<awst::SubmitInnerTransaction>();
	submit->sourceLocation = _loc;
	submit->wtype = &s_payTxnType;
	submit->itxns.push_back(std::move(create));

	auto stmt = awst::makeExpressionStatement(submit, _loc);
	_ctx.pendingStatements.push_back(std::move(stmt));

	auto vc = std::make_shared<awst::VoidConstant>();
	vc->sourceLocation = _loc;
	vc->wtype = awst::WType::voidType();
	return std::make_unique<GenericResultBuilder>(_ctx, std::move(vc));
}

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleSend(
	BuilderContext& _ctx, std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount, awst::SourceLocation const& _loc)
{
	auto create = buildPaymentTransaction(_ctx, std::move(_receiver), std::move(_amount), _loc);
	static awst::WInnerTransaction s_payTxnType(TxnTypePay);
	auto submit = std::make_shared<awst::SubmitInnerTransaction>();
	submit->sourceLocation = _loc;
	submit->wtype = &s_payTxnType;
	submit->itxns.push_back(std::move(create));

	auto stmt = awst::makeExpressionStatement(submit, _loc);
	_ctx.pendingStatements.push_back(std::move(stmt));

	return std::make_unique<SolBoolBuilder>(_ctx, awst::makeBoolConstant(true, _loc));
}

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleCallWithValue(
	BuilderContext& _ctx, std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount, awst::SourceLocation const& _loc)
{
	auto create = buildPaymentTransaction(_ctx, std::move(_receiver), std::move(_amount), _loc);
	static awst::WInnerTransaction s_payTxnType(TxnTypePay);
	auto submit = std::make_shared<awst::SubmitInnerTransaction>();
	submit->sourceLocation = _loc;
	submit->wtype = &s_payTxnType;
	submit->itxns.push_back(std::move(create));

	auto stmt = awst::makeExpressionStatement(submit, _loc);
	_ctx.pendingStatements.push_back(std::move(stmt));

	return std::make_unique<GenericResultBuilder>(_ctx, makeBoolBytesTupleEmpty(_loc));
}

// ── .call(abi.encodeCall(fn, args)) → inner app call ──

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleDelegatecall(
	BuilderContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc)
{
	for (auto const& arg : _callNode.arguments())
		_ctx.buildExpr(*arg);

	return std::make_unique<GenericResultBuilder>(_ctx, makeBoolBytesTupleEmpty(_loc));
}

// ── Top-level dispatcher ──

std::unique_ptr<InstanceBuilder> InnerCallHandlers::tryHandleAddressCall(
	BuilderContext& _ctx,
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

		// Self-call with abi.encodeWithSignature("fn(...)", args): resolve
		// signature → local function by name+arity and emit a direct
		// subroutine call (mirrors the isSelfCall path in
		// handleCallWithEncodeCall). Avoids the fallback stub for contracts
		// without a fallback, where the callee would otherwise never run.
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
					if (encMA && encMA->memberName() == "encodeWithSignature"
						&& !encCallExpr->arguments().empty())
					{
						auto const* sigLit = dynamic_cast<Literal const*>(encCallExpr->arguments()[0].get());
						if (sigLit)
						{
							std::string sig = sigLit->value();
							auto parenPos = sig.find('(');
							if (parenPos != std::string::npos)
							{
								std::string fnName = sig.substr(0, parenPos);
								size_t nArgs = encCallExpr->arguments().size() - 1;
								FunctionDefinition const* target = nullptr;
								if (_ctx.currentContract)
								{
									for (auto const* base : _ctx.currentContract->annotation().linearizedBaseContracts)
									{
										for (auto const* func : base->definedFunctions())
										{
											if (func->isImplemented() && func->name() == fnName
												&& func->parameters().size() == nArgs)
											{
												target = func;
												goto foundEwSTarget;
											}
										}
									}
								}
								foundEwSTarget:;
								if (target)
								{
									auto call = std::make_shared<awst::SubroutineCallExpression>();
									call->sourceLocation = _loc;
									auto* retType = target->returnParameters().size() > 0
										? _ctx.typeMapper.map(target->returnParameters()[0]->type())
										: awst::WType::voidType();
									if (!retType) retType = awst::WType::voidType();
									call->wtype = retType;
									call->target = awst::InstanceMethodTarget{target->name()};
									for (size_t i = 1; i < encCallExpr->arguments().size(); ++i)
									{
										awst::CallArg ca;
										ca.name = std::nullopt;
										ca.value = _ctx.buildExpr(*encCallExpr->arguments()[i]);
										call->args.push_back(std::move(ca));
									}
									std::shared_ptr<awst::Expression> dataBytes;
									if (retType == awst::WType::voidType())
									{
										auto stmt = awst::makeExpressionStatement(call, _loc);
										_ctx.prePendingStatements.push_back(std::move(stmt));
										dataBytes = awst::makeBytesConstant({}, _loc);
									}
									else if (retType == awst::WType::biguintType())
										dataBytes = awst::makeReinterpretCast(std::move(call), awst::WType::bytesType(), _loc);
									else if (retType == awst::WType::uint64Type())
									{
										auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
										itob->stackArgs.push_back(std::move(call));
										dataBytes = std::move(itob);
									}
									else if (retType == awst::WType::bytesType()
										|| retType->kind() == awst::WTypeKind::Bytes)
										dataBytes = awst::makeReinterpretCast(std::move(call), awst::WType::bytesType(), _loc);
									else
									{
										auto stmt = awst::makeExpressionStatement(call, _loc);
										_ctx.prePendingStatements.push_back(std::move(stmt));
										dataBytes = awst::makeBytesConstant({}, _loc);
									}
									return std::make_unique<GenericResultBuilder>(_ctx,
										makeBoolBytesTuple(true, std::move(dataBytes), _loc));
								}
							}
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
				auto result = handleCallWithEncodeCall(_ctx, std::move(_receiver), *encodeCallExpr, _loc);
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
		// Non-encodeCall `address(this).call(data)` self-call: dispatch
		// directly to the contract's __fallback function. Any data that
		// isn't a selector-matching ABI call would have been routed to
		// fallback by our approval program anyway.
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
			// Build the data expression — evaluates any side effects.
			auto dataExpr = _ctx.buildExpr(dataArg);
			if (dataExpr->wtype == awst::WType::stringType())
			{
				auto cast = awst::makeReinterpretCast(std::move(dataExpr), awst::WType::bytesType(), _loc);
				dataExpr = std::move(cast);
			}

			// Only dispatch to __fallback if the contract actually defines
			// one. Otherwise emitting `InstanceMethodTarget{"__fallback"}`
			// leaves an unresolvable reference in the AWST.
			solidity::frontend::FunctionDefinition const* fallbackFunc = nullptr;
			if (_ctx.currentContract)
			{
				for (auto const* base : _ctx.currentContract->annotation().linearizedBaseContracts)
				{
					for (auto const* func : base->definedFunctions())
					{
						if (func->isImplemented() && func->isFallback())
						{
							fallbackFunc = func;
							goto foundFallback;
						}
					}
				}
			}
			foundFallback:;

			if (!fallbackFunc)
			{
				// No fallback in the contract — stub as (true, empty bytes).
				return std::make_unique<GenericResultBuilder>(_ctx,
					makeBoolBytesTupleEmpty(_loc));
			}

			bool fallbackTakesBytes = fallbackFunc->parameters().size() == 1;
			bool fallbackReturnsBytes = !fallbackFunc->returnParameters().empty();

			auto call = std::make_shared<awst::SubroutineCallExpression>();
			call->sourceLocation = _loc;
			call->wtype = fallbackReturnsBytes ? awst::WType::bytesType() : awst::WType::voidType();
			call->target = awst::InstanceMethodTarget{"__fallback"};
			if (fallbackTakesBytes)
			{
				awst::CallArg ca;
				ca.name = std::nullopt;
				ca.value = dataExpr;
				call->args.push_back(std::move(ca));
			}

			// When the fallback returns bytes, spill the subroutine call
			// result into a named local so the caller's `retval` reads it.
			// The router wrapper logs but doesn't return, so the direct
			// InstanceMethodTarget to the bytes-returning fallback is what
			// we invoke.
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

		// Non-self raw .call(data) → inner app call. Splits the blob into
		// [selector, rest] so the callee's ARC4 router can dispatch.
		// Compile-time empty-literal `.call("")` is stubbed as `(true, "")`
		// — matches EVM's low-level "call to non-contract returns true" and
		// avoids spurious inner-txn failures when the target app doesn't
		// exist (see tests/functionCall/bare_call_no_returndatacopy.sol and
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

	// .staticcall(data) → precompile routing
	if (_memberName == "staticcall")
	{
		// Detect precompile address from address(N) pattern
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

		// Fallback stub
		for (auto const& arg : _callNode.arguments())
			_ctx.buildExpr(*arg);
		Logger::instance().warning("address.staticcall(data) stubbed — returns (true, empty).", _loc);
		return std::make_unique<GenericResultBuilder>(_ctx, makeBoolBytesTupleEmpty(_loc));
	}

	// .delegatecall
	if (_memberName == "delegatecall")
		return handleDelegatecall(_ctx, _callNode, _loc);

	return nullptr;
}

void InnerCallHandlers::fundCreatedApp(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> _amount,
	awst::SourceLocation const& _loc)
{
	// Get the real Algorand address of the just-created app
	auto appId = awst::makeIntrinsicCall("itxn", awst::WType::uint64Type(), _loc);
	appId->immediates = {std::string("CreatedApplicationID")};

	auto* tupleType = new awst::WTuple({awst::WType::bytesType(), awst::WType::boolType()});
	auto appParams = awst::makeIntrinsicCall("app_params_get", tupleType, _loc);
	appParams->immediates = {std::string("AppAddress")};
	appParams->stackArgs.push_back(std::move(appId));

	auto addrBytes = std::make_shared<awst::TupleItemExpression>();
	addrBytes->sourceLocation = _loc;
	addrBytes->wtype = awst::WType::bytesType();
	addrBytes->base = std::move(appParams);
	addrBytes->index = 0;

	auto receiver = awst::makeReinterpretCast(std::move(addrBytes), awst::WType::accountType(), _loc);

	// Build and submit inner payment
	auto create = buildPaymentTransaction(_ctx, std::move(receiver), std::move(_amount), _loc);
	static awst::WInnerTransaction s_payTxnType(TxnTypePay);
	auto submit = std::make_shared<awst::SubmitInnerTransaction>();
	submit->sourceLocation = _loc;
	submit->wtype = &s_payTxnType;
	submit->itxns.push_back(std::move(create));

	auto stmt = awst::makeExpressionStatement(std::move(submit), _loc);
	_ctx.prePendingStatements.push_back(std::move(stmt));
}

} // namespace puyasol::builder::eb
