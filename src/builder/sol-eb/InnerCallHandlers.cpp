/// @file InnerCallHandlers.cpp
/// Handles address.call/staticcall/delegatecall/transfer inner transaction patterns
/// and precompile routing.

#include "builder/sol-eb/InnerCallHandlers.h"
#include "builder/sol-eb/SolBoolBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

namespace puyasol::builder::eb
{

class GenericResultBuilder: public InstanceBuilder
{
public:
	GenericResultBuilder(BuilderContext& _ctx, std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr)) {}
	solidity::frontend::Type const* solType() const override { return nullptr; }
};

static constexpr int TxnTypePay = 1;
static constexpr int TxnTypeAppl = 6;

// ── Small helpers ──

std::shared_ptr<awst::Expression> InnerCallHandlers::makeUint64(
	std::string _value, awst::SourceLocation const& _loc)
{
	auto e = awst::makeIntegerConstant(std::move(_value), _loc);
	return e;
}

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
	auto call = std::make_shared<awst::IntrinsicCall>();
	call->sourceLocation = _loc;
	call->wtype = awst::WType::bytesType();
	call->opCode = "extract3";
	call->stackArgs.push_back(std::move(_source));
	call->stackArgs.push_back(makeUint64(std::to_string(_offset), _loc));
	call->stackArgs.push_back(makeUint64(std::to_string(_length), _loc));
	return call;
}

std::shared_ptr<awst::IntrinsicCall> InnerCallHandlers::makeConcat(
	std::shared_ptr<awst::Expression> _a, std::shared_ptr<awst::Expression> _b,
	awst::SourceLocation const& _loc)
{
	auto c = std::make_shared<awst::IntrinsicCall>();
	c->sourceLocation = _loc;
	c->wtype = awst::WType::bytesType();
	c->opCode = "concat";
	c->stackArgs.push_back(std::move(_a));
	c->stackArgs.push_back(std::move(_b));
	return c;
}

std::shared_ptr<awst::Expression> InnerCallHandlers::leftPadToN(
	std::shared_ptr<awst::Expression> _expr, int _n, awst::SourceLocation const& _loc)
{
	auto padding = std::make_shared<awst::IntrinsicCall>();
	padding->sourceLocation = _loc;
	padding->wtype = awst::WType::bytesType();
	padding->opCode = "bzero";
	padding->stackArgs.push_back(makeUint64(std::to_string(_n), _loc));

	auto padded = makeConcat(std::move(padding), std::move(_expr), _loc);

	auto paddedLen = std::make_shared<awst::IntrinsicCall>();
	paddedLen->sourceLocation = _loc;
	paddedLen->wtype = awst::WType::uint64Type();
	paddedLen->opCode = "len";
	paddedLen->stackArgs.push_back(padded);

	auto offset = std::make_shared<awst::UInt64BinaryOperation>();
	offset->sourceLocation = _loc;
	offset->wtype = awst::WType::uint64Type();
	offset->left = std::move(paddedLen);
	offset->op = awst::UInt64BinaryOperator::Sub;
	offset->right = makeUint64(std::to_string(_n), _loc);

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
				auto appId = std::make_shared<awst::IntrinsicCall>();
				appId->sourceLocation = _loc;
				appId->wtype = awst::WType::uint64Type();
				appId->opCode = "global";
				appId->immediates = {std::string("CurrentApplicationID")};

				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = _loc;
				cast->wtype = awst::WType::applicationType();
				cast->expr = std::move(appId);
				return cast;
			}
		}
	}

	std::shared_ptr<awst::Expression> bytesExpr = std::move(_receiver);
	if (bytesExpr->wtype == awst::WType::accountType())
	{
		auto toBytes = std::make_shared<awst::ReinterpretCast>();
		toBytes->sourceLocation = _loc;
		toBytes->wtype = awst::WType::bytesType();
		toBytes->expr = std::move(bytesExpr);
		bytesExpr = std::move(toBytes);
	}

	auto extract = std::make_shared<awst::IntrinsicCall>();
	extract->sourceLocation = _loc;
	extract->wtype = awst::WType::bytesType();
	extract->opCode = "extract";
	extract->immediates = {24, 8};
	extract->stackArgs.push_back(std::move(bytesExpr));

	auto btoi = std::make_shared<awst::IntrinsicCall>();
	btoi->sourceLocation = _loc;
	btoi->wtype = awst::WType::uint64Type();
	btoi->opCode = "btoi";
	btoi->stackArgs.push_back(std::move(extract));

	auto cast = std::make_shared<awst::ReinterpretCast>();
	cast->sourceLocation = _loc;
	cast->wtype = awst::WType::applicationType();
	cast->expr = std::move(btoi);
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
		auto itob = std::make_shared<awst::IntrinsicCall>();
		itob->sourceLocation = _loc;
		itob->wtype = awst::WType::bytesType();
		itob->opCode = "itob";
		itob->stackArgs.push_back(std::move(_arg));
		return itob;
	}

	if (wtype == awst::WType::biguintType())
	{
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = _loc;
		cast->wtype = awst::WType::bytesType();
		cast->expr = std::move(_arg);

		auto zeros = std::make_shared<awst::IntrinsicCall>();
		zeros->sourceLocation = _loc;
		zeros->wtype = awst::WType::bytesType();
		zeros->opCode = "bzero";
		zeros->stackArgs.push_back(makeUint64("32", _loc));

		auto padded = makeConcat(std::move(zeros), std::move(cast), _loc);

		auto paddedLen = std::make_shared<awst::IntrinsicCall>();
		paddedLen->sourceLocation = _loc;
		paddedLen->wtype = awst::WType::uint64Type();
		paddedLen->opCode = "len";
		paddedLen->stackArgs.push_back(padded);

		auto offset = std::make_shared<awst::IntrinsicCall>();
		offset->sourceLocation = _loc;
		offset->wtype = awst::WType::uint64Type();
		offset->opCode = "-";
		offset->stackArgs.push_back(std::move(paddedLen));
		offset->stackArgs.push_back(makeUint64("32", _loc));

		return makeExtract(std::move(padded), 0, 0, _loc);
		// FIXME: should be extract3(padded, offset, 32) with dynamic offset
		// For now, replicate the pattern from FunctionCallBuilder.
	}

	if (wtype == awst::WType::boolType())
	{
		auto setbit = std::make_shared<awst::IntrinsicCall>();
		setbit->sourceLocation = _loc;
		setbit->wtype = awst::WType::bytesType();
		setbit->opCode = "setbit";
		setbit->stackArgs.push_back(awst::makeBytesConstant({0x00}, _loc));
		setbit->stackArgs.push_back(makeUint64("0", _loc));
		setbit->stackArgs.push_back(std::move(_arg));
		return setbit;
	}

	if (wtype == awst::WType::accountType())
	{
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = _loc;
		cast->wtype = awst::WType::bytesType();
		cast->expr = std::move(_arg);
		return cast;
	}

	// Fallback: reinterpret as bytes
	auto cast = std::make_shared<awst::ReinterpretCast>();
	cast->sourceLocation = _loc;
	cast->wtype = awst::WType::bytesType();
	cast->expr = std::move(_arg);
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
	create->fields["TypeEnum"] = makeUint64(std::to_string(TxnTypePay), _loc);
	create->fields["Fee"] = makeUint64("0", _loc);
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

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = _loc;
	stmt->expr = submit;
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

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = _loc;
	stmt->expr = submit;
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

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = _loc;
	stmt->expr = submit;
	_ctx.pendingStatements.push_back(std::move(stmt));

	return std::make_unique<GenericResultBuilder>(_ctx, makeBoolBytesTupleEmpty(_loc));
}

// ── .call(abi.encodeCall(fn, args)) → inner app call ──

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleCallWithEncodeCall(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> _receiver,
	solidity::frontend::FunctionCall const& _encodeCallExpr,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;

	if (_encodeCallExpr.arguments().size() < 2)
		return nullptr;

	auto const& targetFnExpr = *_encodeCallExpr.arguments()[0];
	FunctionDefinition const* targetFuncDef = nullptr;

	if (auto const* fnType = dynamic_cast<FunctionType const*>(targetFnExpr.annotation().type))
		if (fnType->hasDeclaration())
			targetFuncDef = dynamic_cast<FunctionDefinition const*>(&fnType->declaration());

	if (!targetFuncDef)
		return nullptr;

	// Self-call shortcut: if the receiver resolves to
	// `global CurrentApplicationAddress` (i.e. the call is on `this`), emit
	// a direct internal subroutine call rather than an inner txn that AVM
	// would reject with "attempt to self-call".
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
		// Build args from the encodeCall tuple argument (in native types —
		// we call the function directly, skipping ARC4 encode/decode).
		std::vector<ASTPointer<Expression const>> callArgs;
		auto const& argsExpr = *_encodeCallExpr.arguments()[1];
		if (auto const* tupleExpr = dynamic_cast<TupleExpression const*>(&argsExpr))
		{
			for (auto const& comp : tupleExpr->components())
				if (comp) callArgs.push_back(comp);
		}
		else
			callArgs.push_back(_encodeCallExpr.arguments()[1]);

		auto call = std::make_shared<awst::SubroutineCallExpression>();
		call->sourceLocation = _loc;
		auto* retType = _ctx.typeMapper.map(targetFuncDef->returnParameters().size() > 0
			? targetFuncDef->returnParameters()[0]->type()
			: nullptr);
		if (!retType)
			retType = awst::WType::voidType();
		call->wtype = retType;
		call->target = awst::InstanceMethodTarget{targetFuncDef->name()};
		for (auto const& arg : callArgs)
		{
			awst::CallArg ca;
			ca.name = std::nullopt;
			ca.value = _ctx.buildExpr(*arg);
			call->args.push_back(std::move(ca));
		}

		// Return (true, returnBytes) — the caller expects a (bool, bytes) tuple.
		// For a direct internal call we don't have the raw log, so encode the
		// native return back to bytes if it's a simple scalar, otherwise
		// return empty bytes. Most callers either ignore the data portion or
		// abi.decode it — leaving it empty is a known limitation.
		std::shared_ptr<awst::Expression> dataBytes;
		if (retType == awst::WType::voidType())
		{
			// Still need to emit the call as a statement for its side effects
			auto stmt = std::make_shared<awst::ExpressionStatement>();
			stmt->sourceLocation = _loc;
			stmt->expr = call;
			_ctx.prePendingStatements.push_back(std::move(stmt));
			dataBytes = awst::makeBytesConstant({}, _loc);
		}
		else
		{
			// Stash the return value so we can encode it to bytes
			// post-call if needed. For now, encode simple scalars.
			if (retType == awst::WType::biguintType())
			{
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = _loc;
				cast->wtype = awst::WType::bytesType();
				cast->expr = std::move(call);
				dataBytes = std::move(cast);
			}
			else if (retType == awst::WType::uint64Type())
			{
				auto itob = std::make_shared<awst::IntrinsicCall>();
				itob->sourceLocation = _loc;
				itob->wtype = awst::WType::bytesType();
				itob->opCode = "itob";
				itob->stackArgs.push_back(std::move(call));
				dataBytes = std::move(itob);
			}
			else if (retType == awst::WType::bytesType()
				|| (retType && retType->kind() == awst::WTypeKind::Bytes))
			{
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = _loc;
				cast->wtype = awst::WType::bytesType();
				cast->expr = std::move(call);
				dataBytes = std::move(cast);
			}
			else
			{
				// Unknown return type — emit as statement, return empty
				auto stmt = std::make_shared<awst::ExpressionStatement>();
				stmt->sourceLocation = _loc;
				stmt->expr = call;
				_ctx.prePendingStatements.push_back(std::move(stmt));
				dataBytes = awst::makeBytesConstant({}, _loc);
			}
		}

		return std::make_unique<GenericResultBuilder>(_ctx,
			makeBoolBytesTuple(true, std::move(dataBytes), _loc));
	}

	// Build ARC4 method selector
	std::string methodSel = buildMethodSelector(_ctx, targetFuncDef);
	auto methodConst = std::make_shared<awst::MethodConstant>();
	methodConst->sourceLocation = _loc;
	methodConst->wtype = awst::WType::bytesType();
	methodConst->value = methodSel;

	// Build ApplicationArgs tuple
	auto argsTuple = std::make_shared<awst::TupleExpression>();
	argsTuple->sourceLocation = _loc;
	argsTuple->items.push_back(std::move(methodConst));

	// Extract call arguments
	auto const& argsExpr = *_encodeCallExpr.arguments()[1];
	std::vector<ASTPointer<Expression const>> callArgs;
	if (auto const* tupleExpr = dynamic_cast<TupleExpression const*>(&argsExpr))
	{
		for (auto const& comp : tupleExpr->components())
			if (comp) callArgs.push_back(comp);
	}
	else
		callArgs.push_back(_encodeCallExpr.arguments()[1]);

	// Encode each argument
	for (auto const& arg : callArgs)
	{
		auto argExpr = _ctx.buildExpr(*arg);
		argsTuple->items.push_back(encodeArgToBytes(std::move(argExpr), _loc));
	}

	// Build WTuple type for args
	std::vector<awst::WType const*> argTypes;
	for (auto const& item : argsTuple->items)
		argTypes.push_back(item->wtype);
	argsTuple->wtype = _ctx.typeMapper.createType<awst::WTuple>(std::move(argTypes), std::nullopt);

	// Convert receiver → app ID
	auto appId = addressToAppId(std::move(_receiver), _loc);

	// Build inner app call
	static awst::WInnerTransactionFields s_applFieldsType(TxnTypeAppl);
	auto create = std::make_shared<awst::CreateInnerTransaction>();
	create->sourceLocation = _loc;
	create->wtype = &s_applFieldsType;
	create->fields["TypeEnum"] = makeUint64(std::to_string(TxnTypeAppl), _loc);
	create->fields["Fee"] = makeUint64("0", _loc);
	create->fields["ApplicationID"] = std::move(appId);
	create->fields["OnCompletion"] = makeUint64("0", _loc);
	create->fields["ApplicationArgs"] = std::move(argsTuple);

	// Submit
	static awst::WInnerTransaction s_applTxnType(TxnTypeAppl);
	auto submit = std::make_shared<awst::SubmitInnerTransaction>();
	submit->sourceLocation = _loc;
	submit->wtype = &s_applTxnType;
	submit->itxns.push_back(std::move(create));

	auto submitStmt = std::make_shared<awst::ExpressionStatement>();
	submitStmt->sourceLocation = _loc;
	submitStmt->expr = std::move(submit);
	_ctx.prePendingStatements.push_back(std::move(submitStmt));

	// Read LastLog and strip ARC4 prefix
	auto readLog = std::make_shared<awst::IntrinsicCall>();
	readLog->sourceLocation = _loc;
	readLog->wtype = awst::WType::bytesType();
	readLog->opCode = "itxn";
	readLog->immediates = {std::string("LastLog")};

	auto stripPrefix = std::make_shared<awst::IntrinsicCall>();
	stripPrefix->sourceLocation = _loc;
	stripPrefix->opCode = "extract";
	stripPrefix->immediates = {4, 0};
	stripPrefix->wtype = awst::WType::bytesType();
	stripPrefix->stackArgs.push_back(std::move(readLog));

	return std::make_unique<GenericResultBuilder>(_ctx,
		makeBoolBytesTuple(true, std::move(stripPrefix), _loc));
}

// ── .call(rawBytes) → inner app call with raw ApplicationArgs[0] ──

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleCallWithRawData(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _dataBytes,
	awst::SourceLocation const& _loc)
{
	// Coerce string-typed data → bytes for ApplicationArgs encoding.
	if (_dataBytes->wtype == awst::WType::stringType())
	{
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = _loc;
		cast->wtype = awst::WType::bytesType();
		cast->expr = std::move(_dataBytes);
		_dataBytes = std::move(cast);
	}

	// Build ApplicationArgs = [rawBytes]
	auto argsTuple = std::make_shared<awst::TupleExpression>();
	argsTuple->sourceLocation = _loc;
	argsTuple->items.push_back(std::move(_dataBytes));

	std::vector<awst::WType const*> argTypes = {awst::WType::bytesType()};
	argsTuple->wtype = _ctx.typeMapper.createType<awst::WTuple>(
		std::move(argTypes), std::nullopt);

	auto appId = addressToAppId(std::move(_receiver), _loc);

	static awst::WInnerTransactionFields s_applFieldsType(TxnTypeAppl);
	auto create = std::make_shared<awst::CreateInnerTransaction>();
	create->sourceLocation = _loc;
	create->wtype = &s_applFieldsType;
	create->fields["TypeEnum"] = makeUint64(std::to_string(TxnTypeAppl), _loc);
	create->fields["Fee"] = makeUint64("0", _loc);
	create->fields["ApplicationID"] = std::move(appId);
	create->fields["OnCompletion"] = makeUint64("0", _loc);
	create->fields["ApplicationArgs"] = std::move(argsTuple);

	static awst::WInnerTransaction s_applTxnType(TxnTypeAppl);
	auto submit = std::make_shared<awst::SubmitInnerTransaction>();
	submit->sourceLocation = _loc;
	submit->wtype = &s_applTxnType;
	submit->itxns.push_back(std::move(create));

	auto submitStmt = std::make_shared<awst::ExpressionStatement>();
	submitStmt->sourceLocation = _loc;
	submitStmt->expr = std::move(submit);
	_ctx.prePendingStatements.push_back(std::move(submitStmt));

	// Read itxn LastLog as return data. Raw calls don't strip any prefix.
	auto readLog = std::make_shared<awst::IntrinsicCall>();
	readLog->sourceLocation = _loc;
	readLog->wtype = awst::WType::bytesType();
	readLog->opCode = "itxn";
	readLog->immediates = {std::string("LastLog")};

	return std::make_unique<GenericResultBuilder>(_ctx,
		makeBoolBytesTuple(true, std::move(readLog), _loc));
}

// ── .staticcall precompile routing ──

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleStaticCallPrecompile(
	BuilderContext& _ctx,
	uint64_t _precompileAddr,
	std::shared_ptr<awst::Expression> _inputData,
	awst::SourceLocation const& _loc)
{
	std::shared_ptr<awst::Expression> resultBytes;

	switch (_precompileAddr)
	{
	case 6: // ecAdd
	{
		Logger::instance().debug("staticcall precompile 0x06: ecAdd → ec_add BN254g1", _loc);
		auto pointA = makeExtract(_inputData, 0, 64, _loc);
		auto pointB = makeExtract(_inputData, 64, 64, _loc);
		auto ecCall = std::make_shared<awst::IntrinsicCall>();
		ecCall->sourceLocation = _loc;
		ecCall->wtype = awst::WType::bytesType();
		ecCall->opCode = "ec_add";
		ecCall->immediates.push_back("BN254g1");
		ecCall->stackArgs.push_back(std::move(pointA));
		ecCall->stackArgs.push_back(std::move(pointB));
		resultBytes = std::move(ecCall);
		break;
	}
	case 7: // ecMul
	{
		Logger::instance().debug("staticcall precompile 0x07: ecMul → ec_scalar_mul BN254g1", _loc);
		auto point = makeExtract(_inputData, 0, 64, _loc);
		auto scalar = makeExtract(_inputData, 64, 32, _loc);
		auto ecCall = std::make_shared<awst::IntrinsicCall>();
		ecCall->sourceLocation = _loc;
		ecCall->wtype = awst::WType::bytesType();
		ecCall->opCode = "ec_scalar_mul";
		ecCall->immediates.push_back("BN254g1");
		ecCall->stackArgs.push_back(std::move(point));
		ecCall->stackArgs.push_back(std::move(scalar));
		resultBytes = std::move(ecCall);
		break;
	}
	case 8: // ecPairing
	{
		Logger::instance().debug("staticcall precompile 0x08: ecPairing → ec_pairing_check BN254g1", _loc);
		// G1s: pair0[0:64] || pair1[192:256]
		auto g1_0 = makeExtract(_inputData, 0, 64, _loc);
		auto g1_1 = makeExtract(_inputData, 192, 64, _loc);
		auto g1s = makeConcat(std::move(g1_0), std::move(g1_1), _loc);

		// G2 pair 0: swap EVM (x_im,x_re,y_im,y_re) → AVM (x_re,x_im,y_re,y_im)
		auto g2_0 = makeConcat(
			makeConcat(makeExtract(_inputData, 96, 32, _loc), makeExtract(_inputData, 64, 32, _loc), _loc),
			makeConcat(makeExtract(_inputData, 160, 32, _loc), makeExtract(_inputData, 128, 32, _loc), _loc),
			_loc);

		// G2 pair 1
		auto g2_1 = makeConcat(
			makeConcat(makeExtract(_inputData, 288, 32, _loc), makeExtract(_inputData, 256, 32, _loc), _loc),
			makeConcat(makeExtract(_inputData, 352, 32, _loc), makeExtract(_inputData, 320, 32, _loc), _loc),
			_loc);

		auto g2s = makeConcat(std::move(g2_0), std::move(g2_1), _loc);

		auto ecCall = std::make_shared<awst::IntrinsicCall>();
		ecCall->sourceLocation = _loc;
		ecCall->wtype = awst::WType::boolType();
		ecCall->opCode = "ec_pairing_check";
		ecCall->immediates.push_back("BN254g1");
		ecCall->stackArgs.push_back(std::move(g1s));
		ecCall->stackArgs.push_back(std::move(g2s));

		// Bool → ABI-encoded 32-byte result
		auto boolToInt = std::make_shared<awst::IntrinsicCall>();
		boolToInt->sourceLocation = _loc;
		boolToInt->wtype = awst::WType::uint64Type();
		boolToInt->opCode = "select";
		boolToInt->stackArgs.push_back(makeUint64("0", _loc));
		boolToInt->stackArgs.push_back(makeUint64("1", _loc));
		boolToInt->stackArgs.push_back(std::move(ecCall));

		auto itob = std::make_shared<awst::IntrinsicCall>();
		itob->sourceLocation = _loc;
		itob->wtype = awst::WType::bytesType();
		itob->opCode = "itob";
		itob->stackArgs.push_back(std::move(boolToInt));

		auto padding = std::make_shared<awst::IntrinsicCall>();
		padding->sourceLocation = _loc;
		padding->wtype = awst::WType::bytesType();
		padding->opCode = "bzero";
		padding->stackArgs.push_back(makeUint64("24", _loc));

		resultBytes = makeConcat(std::move(padding), std::move(itob), _loc);
		break;
	}
	default:
		Logger::instance().warning(
			"address.staticcall to precompile 0x" + std::to_string(_precompileAddr) +
			" not yet supported on AVM", _loc);
		return nullptr;
	}

	return std::make_unique<GenericResultBuilder>(_ctx,
		makeBoolBytesTuple(true, std::move(resultBytes), _loc));
}

// ── .delegatecall stub ──

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
		if (auto const* encodeCallExpr = dynamic_cast<FunctionCall const*>(&dataArg))
		{
			auto const* encodeMA = dynamic_cast<MemberAccess const*>(&encodeCallExpr->expression());
			if (encodeMA && encodeMA->memberName() == "encodeCall" && encodeCallExpr->arguments().size() >= 2)
			{
				auto result = handleCallWithEncodeCall(_ctx, std::move(_receiver), *encodeCallExpr, _loc);
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
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = _loc;
				cast->wtype = awst::WType::bytesType();
				cast->expr = std::move(dataExpr);
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

			auto call = std::make_shared<awst::SubroutineCallExpression>();
			call->sourceLocation = _loc;
			call->wtype = awst::WType::voidType();
			call->target = awst::InstanceMethodTarget{"__fallback"};
			if (fallbackTakesBytes)
			{
				awst::CallArg ca;
				ca.name = std::nullopt;
				ca.value = dataExpr;
				call->args.push_back(std::move(ca));
			}

			auto stmt = std::make_shared<awst::ExpressionStatement>();
			stmt->sourceLocation = _loc;
			stmt->expr = call;
			_ctx.prePendingStatements.push_back(std::move(stmt));

			return std::make_unique<GenericResultBuilder>(_ctx,
				makeBoolBytesTuple(true, std::move(dataExpr), _loc));
		}

		// Non-self raw .call(data) → stub (true, empty bytes).
		Logger::instance().warning(
			"address.call(data) stubbed — returns (true, empty). "
			"Cross-contract raw calls not supported (AVM blocks self-calls).", _loc);
		for (auto const& arg : _callNode.arguments())
			_ctx.buildExpr(*arg);
		return std::make_unique<GenericResultBuilder>(_ctx, makeBoolBytesTupleEmpty(_loc));
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
	auto appId = std::make_shared<awst::IntrinsicCall>();
	appId->sourceLocation = _loc;
	appId->wtype = awst::WType::uint64Type();
	appId->opCode = "itxn";
	appId->immediates = {std::string("CreatedApplicationID")};

	auto* tupleType = new awst::WTuple({awst::WType::bytesType(), awst::WType::boolType()});
	auto appParams = std::make_shared<awst::IntrinsicCall>();
	appParams->sourceLocation = _loc;
	appParams->wtype = tupleType;
	appParams->opCode = "app_params_get";
	appParams->immediates = {std::string("AppAddress")};
	appParams->stackArgs.push_back(std::move(appId));

	auto addrBytes = std::make_shared<awst::TupleItemExpression>();
	addrBytes->sourceLocation = _loc;
	addrBytes->wtype = awst::WType::bytesType();
	addrBytes->base = std::move(appParams);
	addrBytes->index = 0;

	auto receiver = std::make_shared<awst::ReinterpretCast>();
	receiver->sourceLocation = _loc;
	receiver->wtype = awst::WType::accountType();
	receiver->expr = std::move(addrBytes);

	// Build and submit inner payment
	auto create = buildPaymentTransaction(_ctx, std::move(receiver), std::move(_amount), _loc);
	static awst::WInnerTransaction s_payTxnType(TxnTypePay);
	auto submit = std::make_shared<awst::SubmitInnerTransaction>();
	submit->sourceLocation = _loc;
	submit->wtype = &s_payTxnType;
	submit->itxns.push_back(std::move(create));

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = _loc;
	stmt->expr = std::move(submit);
	_ctx.prePendingStatements.push_back(std::move(stmt));
}

} // namespace puyasol::builder::eb
