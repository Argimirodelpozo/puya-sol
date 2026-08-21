/// @file InnerCallShapes.cpp
/// Per-shape handlers for address.call(...):
///   - handleCallWithEncodeCall   (typed abi.encodeCall self/cross calls)
///   - handleCallWithRawData      (low-level rawBytes)
///   - handleStaticCallPrecompile (0x01..0x09 precompiles)

#include "builder/itxn/InnerCallHandlers.h"
#include "builder/EvmFeaturePolicy.h"
#include "builder/SolcFacts.h"
#include "awst/NameGen.h"
#include "builder/itxn/InnerCallInternal.h"
#include "builder/itxn/CallResolver.h"
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
	std::shared_ptr<awst::Expression> _callValue,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;

	if (_encodeCallExpr.arguments().size() < 2)
		return nullptr;

	auto const& targetFnExpr = *_encodeCallExpr.arguments()[0];
	FunctionDefinition const* targetFuncDef = nullptr;

	auto const* targetFnType = dynamic_cast<FunctionType const*>(
		targetFnExpr.annotation().type);
	if (targetFnType && targetFnType->hasDeclaration())
		targetFuncDef = dynamic_cast<FunctionDefinition const*>(
			&targetFnType->declaration());

	if (!targetFuncDef)
		return nullptr;

	// If receiver is CurrentApplicationAddress (i.e. `this`), emit a direct
	// subroutine call — AVM rejects inner txns to self.
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
		// The dispatcher hard-errors self+{value:} before routing here; a
		// callsub rewrite has no inner txn to attach the payment to.
		if (_callValue)
		{
			Logger::instance().error(
				"`.call{value: ...}(abi.encodeCall(...))` to the contract itself "
				"is not supported on AVM (self-calls lower to a direct subroutine "
				"call; the value payment cannot be attached).", _loc);
			return std::make_unique<GenericResultBuilder>(_ctx,
				makeBoolBytesTuple(true, awst::makeBytesConstant({}, _loc), _loc));
		}

		// Call directly in native types, skipping ARC4 encode/decode.
		std::vector<ASTPointer<Expression const>> callArgs;
		auto const& argsExpr = *_encodeCallExpr.arguments()[1];
		if (auto const* tupleExpr = dynamic_cast<TupleExpression const*>(&argsExpr))
		{
			for (auto const& comp : tupleExpr->components())
				if (comp) callArgs.push_back(comp);
		}
		else
			callArgs.push_back(_encodeCallExpr.arguments()[1]);

		size_t const nReturns = targetFuncDef->returnParameters().size();
		auto* retType = nReturns == 1
			? _ctx.typeMapper.map(targetFuncDef->returnParameters()[0]->type())
			: (nReturns == 0 ? awst::WType::voidType() : nullptr);
		std::vector<awst::WType const*> tupleTypes;
		if (nReturns > 1)
		{
			for (auto const& ret: targetFuncDef->returnParameters())
			{
				auto const* wt = _ctx.typeMapper.map(ret->type());
				tupleTypes.push_back(wt ? wt : awst::WType::voidType());
			}
			retType = _ctx.typeMapper.createType<awst::WTuple>(tupleTypes);
		}
		if (!retType) retType = awst::WType::voidType();
		auto call = awst::makeSubroutineCall(
			awst::InstanceMethodTarget{
				CallResolver::resolveMethodName(_ctx, *targetFuncDef)}, retType, _loc);
		for (auto const& arg : callArgs)
			awst::pushCallArg(call->args, _ctx.buildExpr(*arg));

		// Encode the native return to bytes for the (bool, bytes) result.
		// Leaving data empty for unknown types is a known limitation.
		std::shared_ptr<awst::Expression> dataBytes;
		if (retType == awst::WType::voidType())
		{
			auto stmt = awst::makeExpressionStatement(call, _loc);
			_ctx.preEffects().push_back(std::move(stmt));
			dataBytes = awst::makeBytesConstant({}, _loc);
		}
		else if (nReturns == 1)
		{
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
				// Unknown return type — emit as statement, empty data.
				auto stmt = awst::makeExpressionStatement(call, _loc);
				_ctx.preEffects().push_back(std::move(stmt));
				dataBytes = awst::makeBytesConstant({}, _loc);
			}
		}
		else
		{
			// A callsub returning a tuple must execute once. Encode every native
			// component in declaration order so abi.decode sees the complete
			// returndata rather than only returnParameters()[0].
			auto cached = awst::makeSingleEvaluation(
				std::move(call), retType, awst::nextSingleEvalId(), _loc);
			for (size_t i = 0; i < nReturns; ++i)
			{
				auto const* itemW = tupleTypes[i];
				auto item = awst::makeTupleItem(cached,
					static_cast<int>(i), itemW, _loc);
				auto const* arc4W = _ctx.typeMapper.mapToARC4Type(itemW);
				std::shared_ptr<awst::Expression> part;
				if (itemW == arc4W)
					part = awst::makeAsBytes(std::move(item), _loc);
				else
					part = awst::makeAsBytes(
						awst::makeARC4Encode(std::move(item), arc4W, _loc), _loc);
				dataBytes = dataBytes
					? awst::makeConcat(std::move(dataBytes), std::move(part), _loc)
					: std::move(part);
			}
		}

		return std::make_unique<GenericResultBuilder>(_ctx,
			makeBoolBytesTuple(true, std::move(dataBytes), _loc));
	}

	auto const& argsExpr = *_encodeCallExpr.arguments()[1];
	std::vector<ASTPointer<Expression const>> callArgs;
	if (auto const* tupleExpr = dynamic_cast<TupleExpression const*>(&argsExpr))
	{
		for (auto const& comp : tupleExpr->components())
			if (comp) callArgs.push_back(comp);
	}
	else
		callArgs.push_back(_encodeCallExpr.arguments()[1]);

	std::vector<Type const*> paramTypes;
	for (auto const& parameter: targetFuncDef->parameters())
		paramTypes.push_back(parameter->type());
	if (_ctx.typeMapper.profile().contractAbi == ContractAbi::Evm)
	{
		auto selector = awst::makeBytesConstant(
			builder::SolcFacts::externalSelector(*targetFnType), _loc,
			awst::BytesEncoding::Base16, awst::WType::bytesType());
		return submitTypedAppCall(_ctx, std::move(_receiver),
			buildEvmApplicationArgs(_ctx, std::move(selector), callArgs,
				paramTypes, _loc),
			std::move(_callValue), _loc);
	}

	auto methodConst = awst::makeMethodConstant(
		buildMethodSelector(_ctx, targetFuncDef), awst::WType::bytesType(), _loc);
	auto argsTuple = awst::makeTupleExpression(nullptr, _loc);
	argsTuple->items.push_back(std::move(methodConst));

	for (size_t ai = 0; ai < callArgs.size(); ++ai)
	{
		auto argExpr = _ctx.buildExpr(*callArgs[ai]);
		// abi.encodeCall is TYPED: solc checks the args against the target's params,
		// so encode at each param's DECLARED type (exact biguint width, pad-to-width,
		// dynamic-bytes header) exactly like the typed `c.f(...)` path — the previous
		// type-less encoding padded every biguint to 32B, so a uint128 param's callee
		// decode (16B len-assert) reverted.
		solidity::frontend::Type const* paramType =
			ai < paramTypes.size() ? paramTypes[ai] : nullptr;
		argsTuple->items.push_back(
			encodeArgToBytes(_ctx, std::move(argExpr),
				callArgs[ai]->annotation().type, paramType, _loc));
	}

	return submitTypedAppCall(_ctx, std::move(_receiver), std::move(argsTuple), std::move(_callValue), _loc);
}

// ── Shared tail: typed inner app call ──
// ApplicationArgs[0] = 4-byte selector, [1..n] = ARC4-encoded args.
// Returndata = LastLog[4:] (strip 0x151f7c75 prefix; see EVM_DIVERGENCE).
// _callValue != nullptr → [PaymentTxn, ApplicationCall] group: the payment
// precedes the app call, so the callee's msg.value (gtxns Amount at
// GroupIndex-1) and its non-payable check both see it.
std::unique_ptr<InstanceBuilder> InnerCallHandlers::submitTypedAppCall(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::TupleExpression> _argsTuple,
	std::shared_ptr<awst::Expression> _callValue,
	awst::SourceLocation const& _loc)
{
	EvmFeaturePolicy::report(
		EvmFeature::LowLevelCallOutcome, _ctx.typeMapper.profile(), _loc);
	std::vector<awst::WType const*> argTypes;
	for (auto const& item : _argsTuple->items)
		argTypes.push_back(item->wtype);
	_argsTuple->wtype = _ctx.typeMapper.createType<awst::WTuple>(std::move(argTypes), std::nullopt);

	// Receiver feeds both the payment's Receiver and the app id derivation.
	std::shared_ptr<awst::Expression> payTxn;
	if (_callValue)
	{
		_receiver = awst::makeEvalOnce(_receiver, _loc);
		payTxn = buildPaymentTransaction(_ctx, _receiver, std::move(_callValue), _loc);
	}
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
	if (payTxn)
		submit->itxns.push_back(std::move(payTxn));
	submit->itxns.push_back(std::move(create));

	auto submitStmt = awst::makeExpressionStatement(std::move(submit), _loc);
	_ctx.preEffects().push_back(std::move(submitStmt));

	// Capture THIS submit's log (see captureLastLog: tuple-of-calls clobbering).
	auto readLog = captureLastLog(_ctx, _loc);
	auto stripPrefix = awst::makeExtract(std::move(readLog), 4, 0, _loc); // len=0 = to end

	return std::make_unique<GenericResultBuilder>(_ctx,
		makeBoolBytesTuple(true, std::move(stripPrefix), _loc));
}

// ── .call(abi.encodeWithSignature/WithSelector(...)) → typed inner call ──
// Encoder is visible: each arg goes into its own ApplicationArg (ARC4).
// Self-receiver literal-sig is already rewritten by the dispatcher;
// anything else reaching here with self receiver fails at runtime (AVM rejects self txns).
std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleCallWithSignatureArgs(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _receiver,
	solidity::frontend::FunctionCall const& _encodeExpr,
	bool _isSignature,
	std::shared_ptr<awst::Expression> _callValue,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	auto const& args = _encodeExpr.arguments();
	if (args.empty())
		return nullptr;
	if (_ctx.typeMapper.profile().contractAbi == ContractAbi::Evm)
	{
		std::shared_ptr<awst::Expression> evmSelector;
		if (_isSignature)
		{
			if (auto const* literal = dynamic_cast<Literal const*>(args[0].get()))
				evmSelector = awst::makeBytesConstant(
					builder::SolcFacts::externalSelector(literal->value()), _loc,
					awst::BytesEncoding::Base16, awst::WType::bytesType());
			else
			{
				auto hash = awst::makeIntrinsicCall(
					"keccak256", awst::WType::bytesType(), _loc);
				hash->stackArgs.push_back(
					awst::makeAsBytes(_ctx.buildExpr(*args[0]), _loc));
				evmSelector = awst::makeExtract(std::move(hash), 0, 4, _loc);
			}
		}
		else
			evmSelector = awst::makeAsBytes(_ctx.buildExpr(*args[0]), _loc);

		std::vector<ASTPointer<Expression const>> callArgs;
		for (size_t i = 1; i < args.size(); ++i)
			callArgs.push_back(args[i]);
		return submitTypedAppCall(_ctx, std::move(_receiver),
			buildEvmApplicationArgs(_ctx, std::move(evmSelector), callArgs,
				{}, _loc),
			std::move(_callValue), _loc);
	}

	std::shared_ptr<awst::Expression> selector;
	if (_isSignature)
	{
		if (auto const* lit = dynamic_cast<Literal const*>(args[0].get()))
			selector = awst::makeMethodConstant(
				lit->value(), awst::WType::bytesType(), _loc);
		else
		{
			// Runtime sig: sha512_256(sig)[0:4] (same rule as MethodConstant at compile time).
			auto sigExpr = awst::makeAsBytes(_ctx.buildExpr(*args[0]), _loc);
			auto hash = awst::makeIntrinsicCall(
				"sha512_256", awst::WType::bytesType(), _loc);
			hash->stackArgs.push_back(std::move(sigExpr));
			selector = awst::makeExtract(std::move(hash), 0, 4, _loc);
		}
	}
	else
	{
		// Explicit Solidity→ARC-4 transport boundary. In --evm-selectors mode
		// `C.f.selector` is keccak-derived and must not be forwarded directly to
		// the AVM router. When the selector expression names a declaration, retain
		// the router identity here; genuinely opaque runtime selectors remain
		// untranslatable without the target ABI.
		FunctionDefinition const* target = nullptr;
		VariableDeclaration const* getter = nullptr;
		FunctionType const* targetType = nullptr;
		if (auto const* selectorAccess =
				dynamic_cast<MemberAccess const*>(args[0].get()))
			if (selectorAccess->memberName() == "selector")
			{
				auto const& functionExpr = selectorAccess->expression();
				targetType = dynamic_cast<FunctionType const*>(
					functionExpr.annotation().type);
				if (targetType && targetType->hasDeclaration())
				{
					target = dynamic_cast<FunctionDefinition const*>(
						&targetType->declaration());
					getter = dynamic_cast<VariableDeclaration const*>(
						&targetType->declaration());
				}
				if (!target)
					target = dynamic_cast<FunctionDefinition const*>(
						ASTNode::referencedDeclaration(functionExpr));
			}
		if (target)
		{
			// Even when the declaration lets us substitute the ARC-4 selector at
			// compile time, Solidity still evaluates the selector expression (for
			// example `(sideEffect(), C.f).selector`).
			_ctx.evaluateForEffects(*args[0], _loc);
			selector = awst::makeMethodConstant(
				buildMethodSelector(_ctx, target), awst::WType::bytesType(), _loc);
		}
		else if (getter && targetType)
		{
			_ctx.evaluateForEffects(*args[0], _loc);
			selector = awst::makeMethodConstant(
				buildMethodSelector(_ctx, getter->name(), *targetType),
				awst::WType::bytesType(), _loc);
		}
		else
			selector = awst::makeAsBytes(_ctx.buildExpr(*args[0]), _loc);
	}

	auto argsTuple = awst::makeTupleExpression(nullptr, _loc);
	argsTuple->items.push_back(std::move(selector));
	for (size_t i = 1; i < args.size(); ++i)
		// encodeWithSelector/Signature is TYPE-LESS (no declared params); the shared
		// encoder's nullptr path keeps backing-width encoding (biguint→32B, bare itob).
		argsTuple->items.push_back(
			encodeArgToBytes(_ctx, _ctx.buildExpr(*args[i]),
				args[i]->annotation().type, nullptr, _loc));

	return submitTypedAppCall(_ctx, std::move(_receiver), std::move(argsTuple), std::move(_callValue), _loc);
}

// ── .call(rawBytes) → inner app call ──

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleCallWithRawData(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _dataBytes,
	std::shared_ptr<awst::Expression> _callValue,
	awst::SourceLocation const& _loc)
{
	EvmFeaturePolicy::report(
		EvmFeature::LowLevelCallOutcome, _ctx.typeMapper.profile(), _loc);
	if (_dataBytes->wtype == awst::WType::stringType())
	{
		auto cast = awst::makeAsBytes(std::move(_dataBytes), _loc);
		_dataBytes = std::move(cast);
	}

	// Opaque payload splits losslessly into selector + canonical body for an EVM
	// profile. An ARC4-profile target cannot generically reconstruct individual
	// ARC4 ApplicationArgs from an opaque EVM head/tail blob.
	if (_ctx.typeMapper.profile().contractAbi == ContractAbi::Arc4)
		Logger::instance().warning(
			"low-level .call(data) with an opaque payload: forwarding "
			"[selector, rest] as-is. This matches an ARC4-profile puya-sol "
			"callee only when the target method takes a single static 32-byte "
			"argument (or raw bytes); use --contract-abi evm for generic "
			"Solidity calldata forwarding.", _loc);
	std::string tmpName = "__rawcall_data_" + std::to_string((awst::NameGen::next("InnerCallShapes.s_rawCallTmpCounter") + 1));
	auto tmpTarget = awst::makeVarExpression(tmpName, awst::WType::bytesType(), _loc);
	auto tmpAssign = awst::makeAssignmentStatement(tmpTarget, std::move(_dataBytes), _loc);
	_ctx.preEffects().push_back(std::move(tmpAssign));

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

	// selector = len >= 4 ? data[0:4] : data
	auto extractSel = awst::makeExtract3(tmpRead(), awst::makeIntegerConstant("0", _loc), awst::makeIntegerConstant("4", _loc), _loc);
	auto selector = awst::makeConditional(
		makeGe4(), std::move(extractSel), tmpRead(),
		awst::WType::bytesType(), _loc);

	// rest = len >= 4 ? data[4:] : ""
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

	// Receiver feeds both the payment's Receiver and the app id derivation.
	std::shared_ptr<awst::Expression> payTxn;
	if (_callValue)
	{
		_receiver = awst::makeEvalOnce(_receiver, _loc);
		payTxn = buildPaymentTransaction(_ctx, _receiver, std::move(_callValue), _loc);
	}
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
	if (payTxn)
		submit->itxns.push_back(std::move(payTxn));
	submit->itxns.push_back(std::move(create));

	auto submitStmt = awst::makeExpressionStatement(std::move(submit), _loc);
	_ctx.preEffects().push_back(std::move(submitStmt));

	// Capture THIS submit's log (see captureLastLog: tuple-of-calls clobbering).
	auto readLog = captureLastLog(_ctx, _loc);
	auto stripPrefix = awst::makeExtract(
		std::move(readLog), 4, 0, _loc); // ARC-4 return log prefix

	return std::make_unique<GenericResultBuilder>(_ctx,
		makeBoolBytesTuple(true, std::move(stripPrefix), _loc));
}

// ── .staticcall(data) precompile routing ──

std::unique_ptr<InstanceBuilder> InnerCallHandlers::handleStaticCallPrecompile(
	ContractContext& _ctx,
	uint64_t _precompileAddr,
	std::shared_ptr<awst::Expression> _inputData,
	awst::SourceLocation const& _loc)
{
	// T2: every precompile shape slices _inputData several times (ecrecover
	// 4×, ecAdd/ecMul 2-3×) — pin so a call-valued input evaluates once.
	_inputData = awst::makeEvalOnce(std::move(_inputData), _loc);

	std::shared_ptr<awst::Expression> resultBytes;

	switch (_precompileAddr)
	{
	case 1: // ecRecover
	{
		Logger::instance().debug("staticcall precompile 0x01: ecRecover → ecdsa_pk_recover Secp256k1", _loc);
		// Input: hash[0:32], v[32:64], r[64:96], s[96:128].
		// recovery_id is valid only for v=27/28. AVM's recovery intrinsic traps
		// outside {0,1}, whereas the EVM precompile returns empty returndata.
		auto msgHash = makeExtract(_inputData, 0, 32, _loc);
		auto vByte = makeExtract(_inputData, 63, 1, _loc);
		auto vInt = awst::makeBtoi(std::move(vByte), _loc);
		auto* u64v = awst::WType::uint64Type();
		std::string vName = "__ecrec_v_" + std::to_string(
			awst::NameGen::next("InnerCallShapes.ecrecoverV") + 1);
		_ctx.preEffects().push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(vName, u64v, _loc), std::move(vInt), _loc));
		auto vRead = [&]() { return awst::makeVarExpression(vName, u64v, _loc); };
		auto validV = awst::makeBoolBinOp(
			awst::makeNumericCompare(vRead(), awst::NumericComparison::Eq,
				awst::makeIntegerConstant("27", _loc), _loc),
			awst::BinaryBooleanOperator::Or,
			awst::makeNumericCompare(vRead(), awst::NumericComparison::Eq,
				awst::makeIntegerConstant("28", _loc), _loc), _loc);
		auto vMinus27 = awst::makeUInt64BinOp(
			vRead(), awst::UInt64BinaryOperator::Sub, awst::makeIntegerConstant("27", _loc), _loc);
		std::shared_ptr<awst::Expression> recoveryId = std::move(vMinus27);
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

		std::string tupleVar = "__ecrecover_result_" + std::to_string((awst::NameGen::next("InnerCallShapes.s_ecRecoverTmpCounter") + 1));
		std::string bytesVar = tupleVar + "_bytes";
		_ctx.preEffects().push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(bytesVar, awst::WType::bytesType(), _loc),
			awst::makeBytesConstant({}, _loc), _loc));
		auto validBlock = awst::makeBlock(_loc);
		validBlock->body.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(tupleVar, tupleTypePtr, _loc),
			std::move(ecdsaRecover), _loc));

		auto tupleRead0 = awst::makeVarExpression(tupleVar, tupleTypePtr, _loc);
		auto pubkeyX = awst::makeTupleItem(std::move(tupleRead0), 0, awst::WType::bytesType(), _loc);

		auto tupleRead1 = awst::makeVarExpression(tupleVar, tupleTypePtr, _loc);
		auto pubkeyY = awst::makeTupleItem(std::move(tupleRead1), 1, awst::WType::bytesType(), _loc);

		auto pubkeyConcat = makeConcat(std::move(pubkeyX), std::move(pubkeyY), _loc);
		auto hash = awst::makeKeccak256(std::move(pubkeyConcat), _loc);

		auto addr20 = makeExtract(std::move(hash), 12, 20, _loc); // keccak256[12:32]
		validBlock->body.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(bytesVar, awst::WType::bytesType(), _loc),
			awst::makeLeftPad(std::move(addr20), 12, _loc), _loc));
		_ctx.preEffects().push_back(awst::makeIfElse(
			std::move(validV), std::move(validBlock), nullptr, _loc));
		resultBytes = awst::makeVarExpression(
			bytesVar, awst::WType::bytesType(), _loc);
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
		// This reshaping hard-codes the 2-pair (384-byte) layout. The EVM
		// precompile handles k pairs; a longer input (Groth16 verifiers use
		// 3-4) would silently check only pairs 0-1 here — accepting invalid
		// proofs — and a shorter input would panic mid-extract. Pin the input
		// once (it is embedded ~12 times below) and assert exactly 384 bytes,
		// so anything else is a loud revert instead of a wrong pairing result.
		std::string inVar = "__ecpairing_in_"
			+ std::to_string((awst::NameGen::next("InnerCallShapes.s_ecPairingTmpCounter") + 1));
		_ctx.preEffects().push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(inVar, awst::WType::bytesType(), _loc),
			std::move(_inputData), _loc));
		auto inRead = [&]() {
			return awst::makeVarExpression(inVar, awst::WType::bytesType(), _loc);
		};
		auto lenOk = awst::makeNumericCompare(
			awst::makeLen(inRead(), _loc), awst::NumericComparison::Eq,
			awst::makeIntegerConstant("384", _loc), _loc);
		_ctx.preEffects().push_back(awst::makeExpressionStatement(
			awst::makeAssert(std::move(lenOk), _loc,
				"ecPairing input must be exactly 2 pairs (384 bytes); k-pair "
				"pairing is not supported on AVM"), _loc));
		_inputData = inRead();
		// G1s: pair0[0:64] || pair1[192:256]. G2: swap EVM (im,re) → AVM (re,im).
		auto g1_0 = makeExtract(_inputData, 0, 64, _loc);
		auto g1_1 = makeExtract(_inputData, 192, 64, _loc);
		auto g1s = makeConcat(std::move(g1_0), std::move(g1_1), _loc);

		// G2 pair 0: EVM (x_im,x_re,y_im,y_re) → AVM (x_re,x_im,y_re,y_im)
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

		// bool → ABI-encoded 32-byte result
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

} // namespace puyasol::builder::eb
