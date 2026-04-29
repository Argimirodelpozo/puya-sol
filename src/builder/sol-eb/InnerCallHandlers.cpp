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
	auto call = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	call->stackArgs.push_back(std::move(_source));
	call->stackArgs.push_back(makeUint64(std::to_string(_offset), _loc));
	call->stackArgs.push_back(makeUint64(std::to_string(_length), _loc));
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
	padding->stackArgs.push_back(makeUint64(std::to_string(_n), _loc));

	auto padded = makeConcat(std::move(padding), std::move(_expr), _loc);

	auto paddedLen = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
	paddedLen->stackArgs.push_back(padded);

	auto offset = awst::makeUInt64BinOp(std::move(paddedLen), awst::UInt64BinaryOperator::Sub, makeUint64(std::to_string(_n), _loc), _loc);

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
	// Dynamic-length `bytes` (no fixed length): encode as ARC-4 `byte[]`
	// — uint16 length prefix + raw payload. Without the prefix the
	// callee's ABI router fails to decode (`extract_uint16` on empty or
	// under-length input). FixedBytes types (bytes32, etc.) round-trip
	// cleanly without a header and are handled below.
	if (wtype == awst::WType::bytesType()
		|| wtype == awst::WType::stringType()
		|| (wtype
			&& wtype->kind() == awst::WTypeKind::Bytes
			&& !static_cast<awst::BytesWType const*>(wtype)->length().has_value()))
	{
		auto lenExpr = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
		lenExpr->stackArgs.push_back(_arg);

		auto itobLen = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
		itobLen->stackArgs.push_back(std::move(lenExpr));

		auto header = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
		header->immediates = {6, 2};
		header->stackArgs.push_back(std::move(itobLen));

		auto encoded = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
		encoded->stackArgs.push_back(std::move(header));
		encoded->stackArgs.push_back(std::move(_arg));
		return encoded;
	}
	// FixedBytes (bytes1..bytes32): pass through as-is.
	if (wtype && wtype->kind() == awst::WTypeKind::Bytes)
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
		zeros->stackArgs.push_back(makeUint64("32", _loc));

		auto padded = makeConcat(std::move(zeros), std::move(cast), _loc);

		auto paddedLen = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
		paddedLen->stackArgs.push_back(padded);

		auto offset = awst::makeIntrinsicCall("-", awst::WType::uint64Type(), _loc);
		offset->stackArgs.push_back(std::move(paddedLen));
		offset->stackArgs.push_back(makeUint64("32", _loc));

		return makeExtract(std::move(padded), 0, 0, _loc);
		// FIXME: should be extract3(padded, offset, 32) with dynamic offset
		// For now, replicate the pattern from FunctionCallBuilder.
	}

	if (wtype == awst::WType::boolType())
	{
		auto setbit = awst::makeIntrinsicCall("setbit", awst::WType::bytesType(), _loc);
		setbit->stackArgs.push_back(awst::makeBytesConstant({0x00}, _loc));
		setbit->stackArgs.push_back(makeUint64("0", _loc));
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
			auto stmt = awst::makeExpressionStatement(call, _loc);
			_ctx.prePendingStatements.push_back(std::move(stmt));
			dataBytes = awst::makeBytesConstant({}, _loc);
		}
		else
		{
			// Stash the return value so we can encode it to bytes
			// post-call if needed. For now, encode simple scalars.
			if (retType == awst::WType::biguintType())
			{
				auto cast = awst::makeReinterpretCast(std::move(call), awst::WType::bytesType(), _loc);
				dataBytes = std::move(cast);
			}
			else if (retType == awst::WType::uint64Type())
			{
				auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
				itob->stackArgs.push_back(std::move(call));
				dataBytes = std::move(itob);
			}
			else if (retType == awst::WType::bytesType()
				|| (retType && retType->kind() == awst::WTypeKind::Bytes))
			{
				auto cast = awst::makeReinterpretCast(std::move(call), awst::WType::bytesType(), _loc);
				dataBytes = std::move(cast);
			}
			else
			{
				// Unknown return type — emit as statement, return empty
				auto stmt = awst::makeExpressionStatement(call, _loc);
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

	auto submitStmt = awst::makeExpressionStatement(std::move(submit), _loc);
	_ctx.prePendingStatements.push_back(std::move(submitStmt));

	// Read LastLog and strip ARC4 prefix
	auto readLog = awst::makeIntrinsicCall("itxn", awst::WType::bytesType(), _loc);
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
		auto cast = awst::makeReinterpretCast(std::move(_dataBytes), awst::WType::bytesType(), _loc);
		_dataBytes = std::move(cast);
	}

	// Split the runtime blob into [selector, rest] so the callee's ARC4
	// router matches on the 4-byte method selector and the remainder flows
	// into ApplicationArgs[1] (a single packed arg — works for the common
	// single-arg forward case). Empty / short blobs produce empty selectors
	// that will miss the router's match table and fall through to fallback.
	static int s_rawCallTmpCounter = 0;
	std::string tmpName = "__rawcall_data_" + std::to_string(++s_rawCallTmpCounter);
	auto tmpTarget = awst::makeVarExpression(tmpName, awst::WType::bytesType(), _loc);
	auto tmpAssign = awst::makeAssignmentStatement(tmpTarget, std::move(_dataBytes), _loc);
	_ctx.prePendingStatements.push_back(std::move(tmpAssign));

	auto tmpRead = [&]() {
		return awst::makeVarExpression(tmpName, awst::WType::bytesType(), _loc);
	};
	auto makeLen = [&]() {
		auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
		lenCall->stackArgs.push_back(tmpRead());
		return lenCall;
	};
	auto makeGe4 = [&]() {
		return awst::makeNumericCompare(
			makeLen(), awst::NumericComparison::Gte,
			awst::makeIntegerConstant("4", _loc), _loc);
	};

	// selector = len >= 4 ? extract3(data, 0, 4) : data
	auto extractSel = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	extractSel->stackArgs.push_back(tmpRead());
	extractSel->stackArgs.push_back(awst::makeIntegerConstant("0", _loc));
	extractSel->stackArgs.push_back(awst::makeIntegerConstant("4", _loc));

	auto selector = std::make_shared<awst::ConditionalExpression>();
	selector->sourceLocation = _loc;
	selector->wtype = awst::WType::bytesType();
	selector->condition = makeGe4();
	selector->trueExpr = std::move(extractSel);
	selector->falseExpr = tmpRead();

	// rest = len >= 4 ? extract3(data, 4, len - 4) : empty
	auto restLen = awst::makeUInt64BinOp(
		makeLen(), awst::UInt64BinaryOperator::Sub,
		awst::makeIntegerConstant("4", _loc), _loc);
	auto extractRest = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	extractRest->stackArgs.push_back(tmpRead());
	extractRest->stackArgs.push_back(awst::makeIntegerConstant("4", _loc));
	extractRest->stackArgs.push_back(std::move(restLen));

	auto rest = std::make_shared<awst::ConditionalExpression>();
	rest->sourceLocation = _loc;
	rest->wtype = awst::WType::bytesType();
	rest->condition = makeGe4();
	rest->trueExpr = std::move(extractRest);
	rest->falseExpr = awst::makeBytesConstant({}, _loc);

	auto argsTuple = std::make_shared<awst::TupleExpression>();
	argsTuple->sourceLocation = _loc;
	argsTuple->items.push_back(std::move(selector));
	argsTuple->items.push_back(std::move(rest));

	std::vector<awst::WType const*> argTypes = {
		awst::WType::bytesType(), awst::WType::bytesType()};
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

	auto submitStmt = awst::makeExpressionStatement(std::move(submit), _loc);
	_ctx.prePendingStatements.push_back(std::move(submitStmt));

	// Read itxn LastLog as return data. Raw calls don't strip any prefix.
	auto readLog = awst::makeIntrinsicCall("itxn", awst::WType::bytesType(), _loc);
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
	case 1: // ecRecover
	{
		Logger::instance().debug("staticcall precompile 0x01: ecRecover → ecdsa_pk_recover Secp256k1", _loc);
		// Input (128 bytes): hash(0..32), v(32..64), r(64..96), s(96..128)
		auto msgHash = makeExtract(_inputData, 0, 32, _loc);
		// recovery_id = v - 27, taken as uint64 from last byte of v-word
		auto vByte = makeExtract(_inputData, 63, 1, _loc);
		auto vInt = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
		vInt->stackArgs.push_back(std::move(vByte));
		auto recoveryId = awst::makeUInt64BinOp(
			std::move(vInt), awst::UInt64BinaryOperator::Sub,
			makeUint64("27", _loc), _loc);
		auto r = makeExtract(_inputData, 64, 32, _loc);
		auto s = makeExtract(_inputData, 96, 32, _loc);

		awst::WType const* tupleTypePtr = _ctx.typeMapper.createType<awst::WTuple>(
			std::vector<awst::WType const*>{awst::WType::bytesType(), awst::WType::bytesType()});
		auto ecdsaRecover = awst::makeIntrinsicCall("ecdsa_pk_recover", tupleTypePtr, _loc);
		ecdsaRecover->immediates.push_back("Secp256k1");
		ecdsaRecover->stackArgs.push_back(std::move(msgHash));
		ecdsaRecover->stackArgs.push_back(std::move(recoveryId));
		ecdsaRecover->stackArgs.push_back(std::move(r));
		ecdsaRecover->stackArgs.push_back(std::move(s));

		static int s_ecRecoverTmpCounter = 0;
		std::string tupleVar = "__ecrecover_result_" + std::to_string(++s_ecRecoverTmpCounter);
		auto tupleTarget = awst::makeVarExpression(tupleVar, tupleTypePtr, _loc);
		auto assignTuple = awst::makeAssignmentStatement(tupleTarget, std::move(ecdsaRecover), _loc);
		_ctx.prePendingStatements.push_back(std::move(assignTuple));

		auto tupleRead0 = awst::makeVarExpression(tupleVar, tupleTypePtr, _loc);
		auto pubkeyX = std::make_shared<awst::TupleItemExpression>();
		pubkeyX->sourceLocation = _loc;
		pubkeyX->wtype = awst::WType::bytesType();
		pubkeyX->base = std::move(tupleRead0);
		pubkeyX->index = 0;

		auto tupleRead1 = awst::makeVarExpression(tupleVar, tupleTypePtr, _loc);
		auto pubkeyY = std::make_shared<awst::TupleItemExpression>();
		pubkeyY->sourceLocation = _loc;
		pubkeyY->wtype = awst::WType::bytesType();
		pubkeyY->base = std::move(tupleRead1);
		pubkeyY->index = 1;

		auto pubkeyConcat = makeConcat(std::move(pubkeyX), std::move(pubkeyY), _loc);
		auto hash = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), _loc);
		hash->stackArgs.push_back(std::move(pubkeyConcat));

		// extract last 20 bytes (offset 12)
		auto addr20 = makeExtract(std::move(hash), 12, 20, _loc);
		// Left-pad to 32 bytes: concat(bzero(12), addr20)
		auto pad12 = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
		pad12->stackArgs.push_back(makeUint64("12", _loc));
		resultBytes = makeConcat(std::move(pad12), std::move(addr20), _loc);
		break;
	}
	case 6: // ecAdd
	{
		Logger::instance().debug("staticcall precompile 0x06: ecAdd → ec_add BN254g1", _loc);
		auto pointA = makeExtract(_inputData, 0, 64, _loc);
		auto pointB = makeExtract(_inputData, 64, 64, _loc);
		auto ecCall = awst::makeIntrinsicCall("ec_add", awst::WType::bytesType(), _loc);
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
		auto ecCall = awst::makeIntrinsicCall("ec_scalar_mul", awst::WType::bytesType(), _loc);
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

		auto ecCall = awst::makeIntrinsicCall("ec_pairing_check", awst::WType::boolType(), _loc);
		ecCall->immediates.push_back("BN254g1");
		ecCall->stackArgs.push_back(std::move(g1s));
		ecCall->stackArgs.push_back(std::move(g2s));

		// Bool → ABI-encoded 32-byte result
		auto boolToInt = awst::makeIntrinsicCall("select", awst::WType::uint64Type(), _loc);
		boolToInt->stackArgs.push_back(makeUint64("0", _loc));
		boolToInt->stackArgs.push_back(makeUint64("1", _loc));
		boolToInt->stackArgs.push_back(std::move(ecCall));

		auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
		itob->stackArgs.push_back(std::move(boolToInt));

		auto padding = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
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
