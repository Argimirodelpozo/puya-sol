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

	auto extract = awst::makeExtract(std::move(bytesExpr), 24, 8, _loc);
	auto btoi = awst::makeBtoi(std::move(extract), _loc);
	return awst::makeReinterpretCast(std::move(btoi), awst::WType::applicationType(), _loc);
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
		// AVM biguint is variable-length minimal big-endian; ABI uint256 is
		// exactly 32 bytes. Pad-then-trim via dynamic-offset extract3.
		auto cast = awst::makeReinterpretCast(std::move(_arg), awst::WType::bytesType(), _loc);
		return awst::makeLeftPadToN(std::move(cast), 32, _loc);
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
	ContractContext& _ctx,
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
	ContractContext& /*_ctx*/,
	std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount,
	awst::SourceLocation const& _loc)
{
	static awst::WInnerTransactionFields s_payFieldsType(TxnTypePay);

	auto create = awst::makeCreateInnerTransaction(&s_payFieldsType, _loc);
	create->fields["TypeEnum"] = awst::makeIntegerConstant(TxnTypePay, _loc);
	create->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
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

	return std::make_unique<SolBoolBuilder>(_ctx, awst::makeBoolConstant(true, _loc));
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

// ── .call(abi.encodeCall(fn, args)) → inner app call ──

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleDelegatecall(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc)
{
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
					// Two recognised self-call shapes:
					//   address(this).call(abi.encodeWithSignature("fn(types)", args...))
					//   address(this).call(abi.encodeWithSelector(this.fn.selector, args...))
					// Both lower to a direct InstanceMethodTarget call on the
					// contract's `fn`.
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
						// `this.fn.selector` is MemberAccess(memberName="selector",
						// expr=MemberAccess(memberName="fn", expr=this)).
						if (auto const* selMA = dynamic_cast<MemberAccess const*>(encCallExpr->arguments()[0].get()))
						{
							if (selMA->memberName() == "selector")
							{
								if (auto const* fnMA = dynamic_cast<MemberAccess const*>(&selMA->expression()))
								{
									// expression() is `this`; accept any base
									// (member access on `this` is implicit on
									// the contract's own scope).
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
							// AVM can't self-call (no recursive inner-txn into
							// the same app); rewrite this `address(this).call(...)`
							// pattern into a direct subroutine call to the
							// resolved method. Solidity programs that depend
							// on revert isolation across the boundary will see
							// different behaviour — the inner revert propagates
							// here instead of being caught as success=false.
							Logger::instance().warning(
								"`address(this).call(abi.encode" +
								std::string(encMA->memberName() == "encodeWithSelector"
									? "WithSelector" : "WithSignature") +
								"(...))` self-call rewritten to direct `" + fnName +
								"(...)` invocation. AVM doesn't support self inner-txn "
								"calls; revert-isolation semantics may differ.",
								_loc);

							// Helper: ABI-encode a single value (per its solidity
							// type) into an exact 32-byte big-endian word —
							// matches what the EVM ABI returns at the boundary
							// and what `abi.decode(ret, (...))` expects.
							// Uses makeLeftPadToN (not makeLeftPad) so the
							// result is *exactly* 32 bytes even when the input
							// is already 32 bytes (biguint minimal-rep can be
							// shorter, but we always emit 32).
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
									auto bytes = awst::makeReinterpretCast(
										std::move(v), awst::WType::bytesType(), _loc);
									return awst::makeLeftPadToN(std::move(bytes), 32, _loc);
								}
								if (v->wtype == awst::WType::boolType())
								{
									auto asInt = awst::makeReinterpretCast(
										std::move(v), awst::WType::uint64Type(), _loc);
									auto bytes = awst::makeItob(std::move(asInt), _loc);
									return awst::makeLeftPadToN(std::move(bytes), 32, _loc);
								}
								if (v->wtype && v->wtype->kind() == awst::WTypeKind::Bytes)
								{
									// bytesN: right-pad (EVM convention for
									// fixed-bytes return) — extract 32 bytes
									// from the right-padded result via
									// makeRightPad which uses raw concat (so
									// we'd need len-aware trimming; for the
									// common case of len <= 32, left-side stays
									// untouched and right gets zeros, total 32).
									auto bw = dynamic_cast<awst::BytesWType const*>(v->wtype);
									int len = bw && bw->length() ? *bw->length() : 32;
									auto bytes = awst::makeReinterpretCast(
										std::move(v), awst::WType::bytesType(), _loc);
									if (len < 32)
										return awst::makeRightPad(std::move(bytes), 32 - len, _loc);
									return bytes;
								}
								// Fallback — leave as-is.
								return awst::makeReinterpretCast(
									std::move(v), awst::WType::bytesType(), _loc);
							};

							size_t nReturns = target->returnParameters().size();
							if (nReturns == 0)
							{
								// Void target: just emit the call, return empty bytes.
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

							// Multi-return: call returns a tuple. Use
							// SingleEvaluation so the call runs once and each
							// TupleItemExpression reads from the cached result;
							// then ABI-encode each element to 32 bytes and concat.
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
							auto cachedCall = awst::makeSingleEvaluation(std::move(call), tupleTypeOwned, 0, _loc);

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

			auto call = awst::makeSubroutineCall(awst::InstanceMethodTarget{"__fallback"}, fallbackReturnsBytes ? awst::WType::bytesType() : awst::WType::voidType(), _loc);
			if (fallbackTakesBytes)
				awst::pushCallArg(call->args, dataExpr);

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
	ContractContext& _ctx,
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

	auto addrBytes = awst::makeTupleItem(std::move(appParams), 0, awst::WType::bytesType(), _loc);

	auto receiver = awst::makeReinterpretCast(std::move(addrBytes), awst::WType::accountType(), _loc);

	// Build and submit inner payment
	auto create = buildPaymentTransaction(_ctx, std::move(receiver), std::move(_amount), _loc);
	static awst::WInnerTransaction s_payTxnType(TxnTypePay);
	auto submit = awst::makeSubmitInnerTransaction(&s_payTxnType, _loc);
	submit->itxns.push_back(std::move(create));

	auto stmt = awst::makeExpressionStatement(std::move(submit), _loc);
	_ctx.prePendingStatements.push_back(std::move(stmt));
}

} // namespace puyasol::builder::eb
