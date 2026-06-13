/// @file InnerCallShapes.cpp
/// Per-shape handlers for `address.call(...)` patterns extracted from
/// InnerCallHandlers.cpp. The shape dispatcher (tryHandleAddressCall)
/// and helpers stay in InnerCallHandlers.cpp; this file holds the three
/// large handlers it dispatches to:
///
///   - handleCallWithEncodeCall   (typed `abi.encodeCall(...)` self/cross calls)
///   - handleCallWithRawData       (low-level `address.call(rawBytes)`)
///   - handleStaticCallPrecompile  (static-call routing for 0x01..0x09 precompiles)

#include "builder/itxn/InnerCallHandlers.h"
#include "builder/itxn/InnerCallInternal.h"
#include "builder/sol-eb/SolBoolBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::eb
{

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleCallWithEncodeCall(
	ContractContext& _ctx,
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

		auto* retType = _ctx.typeMapper.map(targetFuncDef->returnParameters().size() > 0
			? targetFuncDef->returnParameters()[0]->type()
			: nullptr);
		if (!retType)
			retType = awst::WType::voidType();
		auto call = awst::makeSubroutineCall(
			awst::InstanceMethodTarget{targetFuncDef->name()}, retType, _loc);
		for (auto const& arg : callArgs)
			awst::pushCallArg(call->args, _ctx.buildExpr(*arg));

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
				auto cast = awst::makeAsBytes(std::move(call), _loc);
				dataBytes = std::move(cast);
			}
			else if (retType == awst::WType::uint64Type())
			{
				dataBytes = awst::makeItob(std::move(call), _loc);
			}
			else if (retType == awst::WType::bytesType()
				|| (retType && retType->kind() == awst::WTypeKind::Bytes))
			{
				auto cast = awst::makeAsBytes(std::move(call), _loc);
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
	auto methodConst = awst::makeMethodConstant(
		buildMethodSelector(_ctx, targetFuncDef), awst::WType::bytesType(), _loc);

	// Build ApplicationArgs tuple
	auto argsTuple = awst::makeTupleExpression(nullptr, _loc);
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

	return submitTypedAppCall(_ctx, std::move(_receiver), std::move(argsTuple), _loc);
}

// ── shared tail: typed inner app call + AVM-framed returndata ──
//
// ApplicationArgs[0] = 4-byte sha512_256 selector, [1..n] = ARC4-encoded
// args. Returndata is AVM-framed: the callee's ARC4 return log minus the
// 0x151f7c75 prefix (per maintainer ruling — static 32-byte values look
// identical to EVM ABI; dynamic returns are ARC4-shaped, so abi.decode
// of a dynamic low-level return diverges from EVM; see EVM_DIVERGENCE).
std::unique_ptr<InstanceBuilder> InnerCallHandlers::submitTypedAppCall(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::TupleExpression> _argsTuple,
	awst::SourceLocation const& _loc)
{
	std::vector<awst::WType const*> argTypes;
	for (auto const& item : _argsTuple->items)
		argTypes.push_back(item->wtype);
	_argsTuple->wtype = _ctx.typeMapper.createType<awst::WTuple>(std::move(argTypes), std::nullopt);

	auto appId = addressToAppId(std::move(_receiver), _loc);

	static awst::WInnerTransactionFields s_applFieldsType(TxnTypeAppl);
	auto create = awst::makeCreateInnerTransaction(&s_applFieldsType, _loc);
	create->fields["TypeEnum"] = awst::makeIntegerConstant(TxnTypeAppl, _loc);
	create->fields["Fee"] = awst::makeZero(_loc);
	create->fields["ApplicationID"] = std::move(appId);
	create->fields["OnCompletion"] = awst::makeZero(_loc);
	create->fields["ApplicationArgs"] = std::move(_argsTuple);

	static awst::WInnerTransaction s_applTxnType(TxnTypeAppl);
	auto submit = awst::makeSubmitInnerTransaction(&s_applTxnType, _loc);
	submit->itxns.push_back(std::move(create));

	auto submitStmt = awst::makeExpressionStatement(std::move(submit), _loc);
	_ctx.prePendingStatements.push_back(std::move(submitStmt));

	// Read LastLog and strip the 4-byte ARC4 return prefix
	auto readLog = awst::makeItxn("LastLog", awst::WType::bytesType(), _loc);
	auto stripPrefix = awst::makeExtract(std::move(readLog), 4, 0, _loc);

	return std::make_unique<GenericResultBuilder>(_ctx,
		makeBoolBytesTuple(true, std::move(stripPrefix), _loc));
}

// ── .call(abi.encodeWithSignature/WithSelector(...)) → typed inner call ──
//
// The encoder is visible at the call site, so the EVM-shaped blob never
// needs to exist: selector from the signature/selector argument, each
// further argument ARC4-encoded per its own type into its own
// ApplicationArg (the EVM analog encodes per static arg type too —
// Solidity does not type-check WithSignature args against the string).
// A self-receiver is not special-cased here: the literal-sig self-call
// pattern is rewritten to a direct subroutine call earlier in the
// dispatcher; anything else reaching this path with a self receiver
// fails loud at runtime (AVM rejects self inner-txn calls).
std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleCallWithSignatureArgs(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _receiver,
	solidity::frontend::FunctionCall const& _encodeExpr,
	bool _isSignature,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	auto const& args = _encodeExpr.arguments();
	if (args.empty())
		return nullptr;

	std::shared_ptr<awst::Expression> selector;
	if (_isSignature)
	{
		if (auto const* lit = dynamic_cast<Literal const*>(args[0].get()))
			selector = awst::makeMethodConstant(
				lit->value(), awst::WType::bytesType(), _loc);
		else
		{
			// Runtime signature string: sha512_256(sig)[0:4], the same
			// rule MethodConstant applies at compile time.
			auto sigExpr = awst::makeAsBytes(_ctx.buildExpr(*args[0]), _loc);
			auto hash = awst::makeIntrinsicCall(
				"sha512_256", awst::WType::bytesType(), _loc);
			hash->stackArgs.push_back(std::move(sigExpr));
			selector = awst::makeExtract(std::move(hash), 0, 4, _loc);
		}
	}
	else
		selector = awst::makeAsBytes(_ctx.buildExpr(*args[0]), _loc);

	auto argsTuple = awst::makeTupleExpression(nullptr, _loc);
	argsTuple->items.push_back(std::move(selector));
	for (size_t i = 1; i < args.size(); ++i)
		argsTuple->items.push_back(
			encodeArgToBytes(_ctx.buildExpr(*args[i]), _loc));

	return submitTypedAppCall(_ctx, std::move(_receiver), std::move(argsTuple), _loc);
}

// ── .call(rawBytes) → inner app call with raw ApplicationArgs[0] ──

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleCallWithRawData(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _dataBytes,
	awst::SourceLocation const& _loc)
{
	// Coerce string-typed data → bytes for ApplicationArgs encoding.
	if (_dataBytes->wtype == awst::WType::stringType())
	{
		auto cast = awst::makeAsBytes(std::move(_dataBytes), _loc);
		_dataBytes = std::move(cast);
	}

	// OPAQUE payload: runtime bytes whose structure the compiler cannot
	// see (forwarder/proxy patterns, storage-loaded blobs). Split into
	// [selector, rest]: the 4-byte selector can match the callee router,
	// and the remainder flows into ApplicationArgs[1] as ONE packed arg —
	// so the call genuinely works only for targets taking a single static
	// 32-byte argument (where the EVM word equals the ARC4 encoding) or a
	// single raw-bytes argument. Warn so the limitation is visible; the
	// recognizable abi.encode* shapes never reach here (typed re-encode
	// in handleCallWithEncodeCall / handleCallWithSignatureArgs).
	Logger::instance().warning(
		"low-level .call(data) with an opaque payload: forwarding "
		"[selector, rest] as-is. This matches a puya-sol callee only when "
		"the target method takes a single static 32-byte argument (or raw "
		"bytes); multi-argument and dynamic-argument targets cannot be "
		"reconstructed from an EVM-shaped blob at runtime.", _loc);
	static int s_rawCallTmpCounter = 0;
	std::string tmpName = "__rawcall_data_" + std::to_string(++s_rawCallTmpCounter);
	auto tmpTarget = awst::makeVarExpression(tmpName, awst::WType::bytesType(), _loc);
	auto tmpAssign = awst::makeAssignmentStatement(tmpTarget, std::move(_dataBytes), _loc);
	_ctx.prePendingStatements.push_back(std::move(tmpAssign));

	auto tmpRead = [&]() {
		return awst::makeVarExpression(tmpName, awst::WType::bytesType(), _loc);
	};
	auto makeLen = [&]() {
		return awst::makeLen(tmpRead(), _loc);
	};
	auto makeGe4 = [&]() {
		return awst::makeNumericCompare(
			makeLen(), awst::NumericComparison::Gte,
			awst::makeIntegerConstant("4", _loc), _loc);
	};

	// selector = len >= 4 ? extract3(data, 0, 4) : data
	auto extractSel = awst::makeExtract3(tmpRead(), awst::makeIntegerConstant("0", _loc), awst::makeIntegerConstant("4", _loc), _loc);
	auto selector = awst::makeConditional(
		makeGe4(), std::move(extractSel), tmpRead(),
		awst::WType::bytesType(), _loc);

	// rest = len >= 4 ? extract3(data, 4, len - 4) : empty
	auto restLen = awst::makeUInt64BinOp(
		makeLen(), awst::UInt64BinaryOperator::Sub,
		awst::makeIntegerConstant("4", _loc), _loc);
	auto extractRest = awst::makeExtract3(tmpRead(), awst::makeIntegerConstant("4", _loc), std::move(restLen), _loc);
	auto rest = awst::makeConditional(
		makeGe4(), std::move(extractRest),
		awst::makeBytesConstant({}, _loc),
		awst::WType::bytesType(), _loc);

	auto argsTuple = awst::makeTupleExpression(nullptr, _loc);
	argsTuple->items.push_back(std::move(selector));
	argsTuple->items.push_back(std::move(rest));

	std::vector<awst::WType const*> argTypes = {
		awst::WType::bytesType(), awst::WType::bytesType()};
	argsTuple->wtype = _ctx.typeMapper.createType<awst::WTuple>(
		std::move(argTypes), std::nullopt);

	auto appId = addressToAppId(std::move(_receiver), _loc);

	static awst::WInnerTransactionFields s_applFieldsType(TxnTypeAppl);
	auto create = awst::makeCreateInnerTransaction(&s_applFieldsType, _loc);
	create->fields["TypeEnum"] = awst::makeIntegerConstant(TxnTypeAppl, _loc);
	create->fields["Fee"] = awst::makeZero(_loc);
	create->fields["ApplicationID"] = std::move(appId);
	create->fields["OnCompletion"] = awst::makeZero(_loc);
	create->fields["ApplicationArgs"] = std::move(argsTuple);

	static awst::WInnerTransaction s_applTxnType(TxnTypeAppl);
	auto submit = awst::makeSubmitInnerTransaction(&s_applTxnType, _loc);
	submit->itxns.push_back(std::move(create));

	auto submitStmt = awst::makeExpressionStatement(std::move(submit), _loc);
	_ctx.prePendingStatements.push_back(std::move(submitStmt));

	// Read itxn LastLog as return data. Raw calls don't strip any prefix.
	auto readLog = awst::makeItxn("LastLog", awst::WType::bytesType(), _loc);

	return std::make_unique<GenericResultBuilder>(_ctx,
		makeBoolBytesTuple(true, std::move(readLog), _loc));
}

// ── .staticcall precompile routing ──

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleStaticCallPrecompile(
	ContractContext& _ctx,
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
		// recovery_id = v - 27 from the last byte of the v-word, but guard
		// against v < 27: AVM `-` panics on underflow where EVM's 0x01
		// precompile returns empty. Mirror the ecrecover() builtin's clamp:
		// (v >= 27) ? v - 27 : 0. Pin v to a temp (read twice).
		auto vByte = makeExtract(_inputData, 63, 1, _loc);
		auto vInt = awst::makeBtoi(std::move(vByte), _loc);
		auto* u64v = awst::WType::uint64Type();
		static int s_ecRecVTmp = 0;
		std::string vName = "__ecrec_v_" + std::to_string(++s_ecRecVTmp);
		auto bindV = awst::makeAssignmentExpression(
			awst::makeVarExpression(vName, u64v, _loc), std::move(vInt), _loc, u64v);
		auto vRead = [&]() { return awst::makeVarExpression(vName, u64v, _loc); };
		auto vGte27 = awst::makeNumericCompare(
			vRead(), awst::NumericComparison::Gte, awst::makeIntegerConstant("27", _loc), _loc);
		auto vMinus27 = awst::makeUInt64BinOp(
			vRead(), awst::UInt64BinaryOperator::Sub, awst::makeIntegerConstant("27", _loc), _loc);
		auto guarded = awst::makeConditional(
			std::move(vGte27), std::move(vMinus27), awst::makeZero(_loc), u64v, _loc);
		auto recoveryIdComma = awst::makeCommaExpression(u64v, _loc);
		recoveryIdComma->expressions.push_back(std::move(bindV));
		recoveryIdComma->expressions.push_back(std::move(guarded));
		std::shared_ptr<awst::Expression> recoveryId = recoveryIdComma;
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
		auto pubkeyX = awst::makeTupleItem(std::move(tupleRead0), 0, awst::WType::bytesType(), _loc);

		auto tupleRead1 = awst::makeVarExpression(tupleVar, tupleTypePtr, _loc);
		auto pubkeyY = awst::makeTupleItem(std::move(tupleRead1), 1, awst::WType::bytesType(), _loc);

		auto pubkeyConcat = makeConcat(std::move(pubkeyX), std::move(pubkeyY), _loc);
		auto hash = awst::makeKeccak256(std::move(pubkeyConcat), _loc);

		// extract last 20 bytes (offset 12)
		auto addr20 = makeExtract(std::move(hash), 12, 20, _loc);
		// Left-pad to 32 bytes: concat(bzero(12), addr20)
		resultBytes = awst::makeLeftPad(std::move(addr20), 12, _loc);
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
		boolToInt->stackArgs.push_back(awst::makeZero(_loc));
		boolToInt->stackArgs.push_back(awst::makeOne(_loc));
		boolToInt->stackArgs.push_back(std::move(ecCall));

		auto itob = awst::makeItob(std::move(boolToInt), _loc);
		resultBytes = awst::makeLeftPad(std::move(itob), 24, _loc);
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
