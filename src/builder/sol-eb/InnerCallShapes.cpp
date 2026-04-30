/// @file InnerCallShapes.cpp
/// Per-shape handlers for `address.call(...)` patterns extracted from
/// InnerCallHandlers.cpp. The shape dispatcher (tryHandleAddressCall)
/// and helpers stay in InnerCallHandlers.cpp; this file holds the three
/// large handlers it dispatches to:
///
///   - handleCallWithEncodeCall   (typed `abi.encodeCall(...)` self/cross calls)
///   - handleCallWithRawData       (low-level `address.call(rawBytes)`)
///   - handleStaticCallPrecompile  (static-call routing for 0x01..0x09 precompiles)

#include "builder/sol-eb/InnerCallHandlers.h"
#include "builder/sol-eb/InnerCallInternal.h"
#include "builder/sol-eb/SolBoolBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::eb
{

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
	create->fields["TypeEnum"] = awst::makeIntegerConstant(std::to_string(TxnTypeAppl), _loc);
	create->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
	create->fields["ApplicationID"] = std::move(appId);
	create->fields["OnCompletion"] = awst::makeIntegerConstant("0", _loc);
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
	create->fields["TypeEnum"] = awst::makeIntegerConstant(std::to_string(TxnTypeAppl), _loc);
	create->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
	create->fields["ApplicationID"] = std::move(appId);
	create->fields["OnCompletion"] = awst::makeIntegerConstant("0", _loc);
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
			awst::makeIntegerConstant("27", _loc), _loc);
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
		pad12->stackArgs.push_back(awst::makeIntegerConstant("12", _loc));
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
		boolToInt->stackArgs.push_back(awst::makeIntegerConstant("0", _loc));
		boolToInt->stackArgs.push_back(awst::makeIntegerConstant("1", _loc));
		boolToInt->stackArgs.push_back(std::move(ecCall));

		auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
		itob->stackArgs.push_back(std::move(boolToInt));

		auto padding = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
		padding->stackArgs.push_back(awst::makeIntegerConstant("24", _loc));

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


} // namespace puyasol::builder::eb
