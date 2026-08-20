/// @file PrecompileDispatch.cpp
/// EVM precompile dispatch: routes call/staticcall to specific precompile handlers.

#include "builder/assembly/AssemblyBuilder.h"
#include "awst/NameGen.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <sstream>

namespace puyasol::builder
{

void AssemblyBuilder::handlePrecompileCall(
	solidity::yul::FunctionCall const& _call,
	std::string const& _assignTarget,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out,
	bool _isCall
)
{
	// call(gas, addr, value, inOff, inSize, outOff, outSize) — 7 args
	// staticcall(gas, addr, inOff, inSize, outOff, outSize) — 6 args
	size_t expectedArgs = _isCall ? 7 : 6;
	if (_call.arguments.size() != expectedArgs)
	{
		Logger::instance().error(
			(_isCall ? std::string("call") : std::string("staticcall")) +
			" requires " + std::to_string(expectedArgs) + " arguments", _loc
		);
		return;
	}

	int argBase = _isCall ? 3 : 2; // call has extra `value` at position 2

	auto precompileAddr = resolveConstantYulValue(_call.arguments[1]);
	if (!precompileAddr)
	{
		// Non-constant address → user-defined contract call (e.g. Solady SafeTransferLib).
		handleAppCall(_call, _assignTarget, _loc, _out, _isCall);
		return;
	}

	auto inputOffset = resolveConstantYulValue(_call.arguments[argBase]);
	auto inputSize = resolveConstantYulValue(_call.arguments[argBase + 1]);
	auto outputOffset = resolveConstantYulValue(_call.arguments[argBase + 2]);
	auto outputSize = resolveConstantYulValue(_call.arguments[argBase + 3]);

	if (!inputOffset || !inputSize || !outputOffset || !outputSize)
	{
		// Dynamic offsets: route to RT handlers (ecAdd/ecMul/ecPairing/SHA-256/Identity).
		bool rtDispatched = false;
		bool rtSuccess = true;
		auto inOffExpr  = buildExpression(_call.arguments[argBase]);
		auto inSizeExpr = buildExpression(_call.arguments[argBase + 1]);
		auto outOffExpr = buildExpression(_call.arguments[argBase + 2]);
		auto outSizeExpr = buildExpression(_call.arguments[argBase + 3]);
		switch (*precompileAddr)
		{
		case 2:
			Logger::instance().debug("precompile 0x02: SHA-256 (runtime offsets)", _loc);
			handleSha256PrecompileRT(inOffExpr, inSizeExpr, outOffExpr, outSizeExpr, _loc, _out);
			rtDispatched = true;
			break;
		case 4:
			Logger::instance().debug("precompile 0x04: Identity (runtime offsets)", _loc);
			handleIdentityPrecompileRT(inOffExpr, inSizeExpr, outOffExpr, outSizeExpr, _loc, _out);
			rtDispatched = true;
			break;
		case 5:
			Logger::instance().debug("precompile 0x05: ModExp (runtime offsets)", _loc);
			handleModExpRT(inOffExpr, inSizeExpr, outOffExpr, outSizeExpr, _loc, _out);
			rtDispatched = true;
			break;
		case 6:
			Logger::instance().debug("precompile 0x06: ecAdd (runtime offsets)", _loc);
			handleEcAddRT(inOffExpr, outOffExpr, _loc, _out);
			rtDispatched = true;
			break;
		case 7:
			Logger::instance().debug("precompile 0x07: ecMul (runtime offsets)", _loc);
			handleEcMulRT(inOffExpr, outOffExpr, _loc, _out);
			rtDispatched = true;
			break;
		case 8:
			Logger::instance().debug("precompile 0x08: ecPairing (runtime offsets)", _loc);
			handleEcPairingRT(inOffExpr, inSizeExpr, outOffExpr, _loc, _out);
			rtDispatched = true;
			break;
		default:
			break;
		}
		if (!rtDispatched)
		{
			// HARD ERROR: no RT handler → output buffer is uninitialized; stubbing
			// success would let crypto/fund-guard checks pass on garbage output.
			Logger::instance().error(
				"precompile call with non-constant memory offsets/sizes is not "
				"supported on AVM — there is no runtime-offset handler for this "
				"precompile, so its output buffer cannot be produced. Stubbing it "
				"as success would let the caller read uninitialized output as if "
				"the precompile had run.", _loc
			);
		}
		if (!_assignTarget.empty())
		{
			auto localIt = m_locals.find(_assignTarget);
			auto* varType = (localIt != m_locals.end()) ? localIt->second : awst::WType::biguintType();
			std::shared_ptr<awst::Expression> rhs = (varType == awst::WType::boolType())
				? std::shared_ptr<awst::Expression>(awst::makeBoolConstant(rtSuccess, _loc))
				: awst::makeIntegerConstant(rtSuccess ? "1" : "0", _loc, awst::WType::biguintType());
			_out.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(_assignTarget, varType, _loc),
				std::move(rhs), _loc));
		}
		return;
	}

	bool success = true;

	// Wrap constants as IntegerConstant expressions so constant/dynamic paths
	// share the same RT handlers; puya constant-folds them at the backend.
	auto wrap = [&](uint64_t v) {
		return awst::makeIntegerConstant(v, _loc);
	};

	switch (*precompileAddr)
	{
	case 1: // ecRecover (no RT variant — keep constant path)
		Logger::instance().debug("precompile 0x01: ecRecover", _loc);
		handleEcRecover(*inputOffset, *inputSize, *outputOffset, *outputSize, _loc, _out);
		break;

	case 2: // SHA-256
		Logger::instance().debug("precompile 0x02: SHA-256", _loc);
		handleSha256PrecompileRT(wrap(*inputOffset), wrap(*inputSize),
			wrap(*outputOffset), wrap(*outputSize), _loc, _out);
		break;

	case 3: // RIPEMD-160
		Logger::instance().error(
			"precompile 0x03 (RIPEMD-160) not yet supported on AVM", _loc
		);
		success = false;
		break;

	case 4: // Identity (data copy)
		Logger::instance().debug("precompile 0x04: Identity", _loc);
		handleIdentityPrecompileRT(wrap(*inputOffset), wrap(*inputSize),
			wrap(*outputOffset), wrap(*outputSize), _loc, _out);
		break;

	case 5: // ModExp
		Logger::instance().debug("precompile 0x05: ModExp (square-and-multiply)", _loc);
		handleModExpRT(wrap(*inputOffset), wrap(*inputSize),
			wrap(*outputOffset), wrap(*outputSize), _loc, _out);
		break;

	case 6: // ecAdd
		Logger::instance().debug("precompile 0x06: ecAdd → AVM ec_add BN254g1", _loc);
		handleEcAddRT(wrap(*inputOffset), wrap(*outputOffset), _loc, _out);
		break;

	case 7: // ecMul
		Logger::instance().debug("precompile 0x07: ecMul → AVM ec_scalar_mul BN254g1", _loc);
		handleEcMulRT(wrap(*inputOffset), wrap(*outputOffset), _loc, _out);
		break;

	case 8: // ecPairing
		Logger::instance().debug("precompile 0x08: ecPairing → AVM ec_pairing_check BN254g1", _loc);
		handleEcPairingRT(wrap(*inputOffset), wrap(*inputSize),
			wrap(*outputOffset), _loc, _out);
		break;

	case 9: // BLAKE2f
		Logger::instance().error(
			"precompile 0x09 (BLAKE2f) not yet supported on AVM", _loc
		);
		success = false;
		break;

	case 10: // KZG point evaluation
		Logger::instance().error(
			"precompile 0x0a (KZG point evaluation) not applicable on Algorand", _loc
		);
		success = false;
		break;

	default:
		// Fail-loud policy (M8): a success=true no-op stub makes `require(ok)`
		// pass spuriously with all-zero returndata.
		Logger::instance().error(
			(_isCall ? std::string("call") : std::string("staticcall")) +
			" to constant non-precompile address " + std::to_string(*precompileAddr) +
			" is not supported on AVM (no app lives at a small constant "
			"address); stubbing it as a no-op would silently drop the call.", _loc
		);
		success = false;
		break;
	}

	if (!_assignTarget.empty())
	{
		auto localIt = m_locals.find(_assignTarget);
		auto* varType = (localIt != m_locals.end()) ? localIt->second : awst::WType::biguintType();
		if (localIt == m_locals.end())
			m_locals[_assignTarget] = varType;
		std::shared_ptr<awst::Expression> val = (varType == awst::WType::boolType())
			? std::shared_ptr<awst::Expression>(awst::makeBoolConstant(success, _loc))
			: awst::makeIntegerConstant(success ? "1" : "0", _loc, awst::WType::biguintType());
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(_assignTarget, varType, _loc), std::move(val), _loc));
	}
}

// ─── BN254 precompile handlers ──────────────────────────────────────────────


// ─── Generic inner-app-call lowering for non-precompile addresses ───────────

void AssemblyBuilder::handleAppCall(
	solidity::yul::FunctionCall const& _call,
	std::string const& _assignTarget,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out,
	bool _isCall
)
{
	int argBase = _isCall ? 3 : 2;

	Logger::instance().debug(
		std::string(_isCall ? "call" : "staticcall") +
		" to runtime address — lowering to inner app call", _loc);

	// 1) Address → ApplicationID: puya-sol encodes as (\x00*24 ++ itob(app_id));
	//    casting to uint64 recovers app_id (high bytes are zero).
	auto addrAwst = awst::makeEvalOnce(buildExpression(_call.arguments[1]), _loc);
	// Shared with the payment leg below — same SingleEvaluation, one runtime
	// evaluation; the appId branches std::move addrAwst.
	auto addrShared = addrAwst;

	std::shared_ptr<awst::Expression> appIdExpr;
	if (addrAwst->wtype == awst::WType::applicationType())
	{
		appIdExpr = std::move(addrAwst);
	}
	else if (addrAwst->wtype == awst::WType::accountType())
	{
		auto toBytes = awst::makeAsBytes(std::move(addrAwst), _loc);
		auto extract = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
		extract->immediates = {24, 8};
		extract->stackArgs.push_back(std::move(toBytes));
		auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
		btoi->stackArgs.push_back(std::move(extract));
		appIdExpr = awst::makeReinterpretCast(std::move(btoi), awst::WType::applicationType(), _loc);
	}
	else
	{
		// Numeric value: low 64 bits = app_id. implicitNumericCast, not a raw
		// ReinterpretCast — puya rejects biguint→uint64 reinterprets (asm
		// address values are biguint).
		auto asU64 = builder::TypeCoercion::implicitNumericCast(
			std::move(addrAwst), awst::WType::uint64Type(), _loc);
		appIdExpr = awst::makeReinterpretCast(std::move(asU64), awst::WType::applicationType(), _loc);
	}

	// 2) Split calldata: args[0]=selector(4B), args[1]=rest (EVM-ABI layout).
	// offsetToUint64, not raw ReinterpretCast: puya rejects biguint→uint64
	// reinterprets (asm values are biguint).
	// Pin the input offset: it feeds the selector read AND the body read, so a
	// `mload(0x40)`-shaped argument must not run twice. An EXPRESSION-level
	// pin, deliberately: this lowering emits nothing before its own itxn
	// submit, and inserting statements ahead of the submit reorders it
	// relative to the caller's pending statements (it broke the grouped
	// payment leg's receiver).
	auto inOffAwst = awst::makeEvalOnce(
		offsetToUint64(buildExpression(_call.arguments[argBase]), _loc), _loc);
	auto inSizeAwst = offsetToUint64(buildExpression(_call.arguments[argBase + 1]), _loc);

	// Clamp inSize to >= 4 so `bodyLen = inSize - 4` can't underflow into a
	// huge uint64 (extract3 OOB panic) for inSize < 4 — a plain value-transfer
	// `call(g, to, amt, 0, 0, 0, 0)` (Solady safeTransferETH) has inSize 0.
	// The resulting empty-body app call is not a faithful ETH transfer (asm
	// value transfers are a known gap), but it no longer crashes opaquely.
	{
		auto eoInSize = awst::makeEvalOnce(std::move(inSizeAwst), _loc);
		inSizeAwst = awst::makeConditional(
			awst::makeNumericCompare(eoInSize, awst::NumericComparison::Gte,
				awst::makeIntegerConstant("4", _loc), _loc),
			eoInSize, awst::makeIntegerConstant("4", _loc),
			awst::WType::uint64Type(), _loc);
	}

	// args[0] = first 4 bytes (selector), args[1] = bytes [inOff+4 .. inOff+inSize).
	// Slot-routed (readMemStackRange is expression-only, so the statement
	// stream around the itxn is unchanged): the request buffer lives at the
	// runtime FMP, so the old slot-0 extracts sent the WRONG CALLDATA (or
	// panicked) for any --evm-memory-slots contract whose buffer passed 4096.
	// Both reads are itxn ApplicationArgs, hence single stack values.
	auto selector = readMemStackRange(scratchLayout(), inOffAwst,
		awst::makeIntegerConstant("4", _loc), _loc);

	auto bodyOff = awst::makeIntrinsicCall("+", awst::WType::uint64Type(), _loc);
	bodyOff->stackArgs.push_back(inOffAwst);
	bodyOff->stackArgs.push_back(awst::makeIntegerConstant("4", _loc));

	auto bodyLen = awst::makeIntrinsicCall("-", awst::WType::uint64Type(), _loc);
	bodyLen->stackArgs.push_back(std::move(inSizeAwst));
	bodyLen->stackArgs.push_back(awst::makeIntegerConstant("4", _loc));

	auto body = readMemStackRange(scratchLayout(), std::move(bodyOff),
		std::move(bodyLen), _loc);

	// 2b) call's `value` (arguments[2]): attach a grouped payment (M8 — it was
	// silently dropped). Receiver mirrors the high-level `.call{value:}` leg:
	// the address value as an account. Constant 0 (the common
	// `call(g,to,0,...)`) skips the leg entirely.
	std::shared_ptr<awst::Expression> payTxn;
	if (_isCall)
	{
		auto constVal = resolveConstantYulValue(_call.arguments[2]);
		if (!constVal || *constVal != 0)
		{
			std::shared_ptr<awst::Expression> receiver;
			if (addrShared->wtype == awst::WType::accountType())
				receiver = addrShared;
			else if (addrShared->wtype == awst::WType::applicationType())
			{
				auto* tupleType = m_typeMapper.createType<awst::WTuple>(
					std::vector<awst::WType const*>{
						awst::WType::bytesType(), awst::WType::boolType()});
				auto appParams = awst::makeAppParamsGet("AppAddress",
					awst::makeReinterpretCast(addrShared, awst::WType::uint64Type(), _loc),
					tupleType, _loc);
				receiver = awst::makeAsAccount(awst::makeTupleItem(
					std::move(appParams), 0, awst::WType::bytesType(), _loc), _loc);
			}
			else
				receiver = awst::makeAsAccount(
					padTo32Bytes(addrShared, _loc), _loc);

			static constexpr int TxnTypePay = 1;
			static awst::WInnerTransactionFields s_payFieldsType(TxnTypePay);
			auto payCreate = awst::makeCreateInnerTransaction(&s_payFieldsType, _loc);
			payCreate->fields["TypeEnum"] = awst::makeIntegerConstant(std::to_string(TxnTypePay), _loc);
			payCreate->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
			payCreate->fields["Receiver"] = std::move(receiver);
			// checkedAmountToUint64, not safeBtoi: silent low-8-byte
			// truncation of a payment amount is a money bug (M17 policy).
			payCreate->fields["Amount"] = builder::TypeCoercion::checkedAmountToUint64(
				_out, buildExpression(_call.arguments[2]), _loc);
			payTxn = std::move(payCreate);
		}
	}

	// 3) Build inner app-call transaction.
	static constexpr int TxnTypeAppl = 6;
	static awst::WInnerTransactionFields s_applFieldsType(TxnTypeAppl);

	auto argsTuple = std::make_shared<awst::TupleExpression>();
	argsTuple->sourceLocation = _loc;
	argsTuple->items.push_back(std::move(selector));
	argsTuple->items.push_back(std::move(body));
	std::vector<awst::WType const*> argTypes{awst::WType::bytesType(), awst::WType::bytesType()};
	argsTuple->wtype = m_typeMapper.createType<awst::WTuple>(std::move(argTypes), std::nullopt);

	auto create = std::make_shared<awst::CreateInnerTransaction>();
	create->sourceLocation = _loc;
	create->wtype = &s_applFieldsType;
	create->fields["TypeEnum"] = awst::makeIntegerConstant(std::to_string(TxnTypeAppl), _loc);
	create->fields["Fee"] = awst::makeIntegerConstant("0", _loc);
	create->fields["ApplicationID"] = std::move(appIdExpr);
	create->fields["OnCompletion"] = awst::makeIntegerConstant("0", _loc);
	create->fields["ApplicationArgs"] = std::move(argsTuple);

	static awst::WInnerTransaction s_applTxnType(TxnTypeAppl);
	auto submit = std::make_shared<awst::SubmitInnerTransaction>();
	submit->sourceLocation = _loc;
	submit->wtype = &s_applTxnType;
	if (payTxn)
		submit->itxns.push_back(std::move(payTxn));
	submit->itxns.push_back(std::move(create));

	_out.push_back(awst::makeExpressionStatement(std::move(submit), _loc));

	// 4) Copy returndata into memory[outOff..outOff+outSize) if size > 0.
	// returndataBytes strips the ARC4 return prefix (M8: the copy was
	// prefix-shifted vs what the callee returned); zero-pad so a shorter
	// returndata still fills outSize (EVM copies min(outSize, rds); the
	// zero-fill beyond rds is the documented approximation).
	// ONE path for constant and runtime out-offset/size. The constant arm used
	// to chunk the payload into whole 32-byte words via storeResultToMemory,
	// which (a) RIGHT-aligned an outSize<32 payload inside a full word where
	// EVM left-aligns it and leaves the rest of the word untouched, and (b)
	// panicked for a non-multiple-of-32 outSize (0x24: it extracted a second
	// 32-byte word from a 36-byte value). writeMemRangeDyn writes exactly
	// outSize bytes with the destination's own tail preserved.
	// The zero-fill beyond returndatasize is the documented approximation
	// (EVM copies min(outSize, rds)); outSize==0 copies nothing.
	auto outSizeOpt = resolveConstantYulValue(_call.arguments[argBase + 3]);
	if (!outSizeOpt || *outSizeOpt > 0)
	{
		auto outOff = buildExpression(_call.arguments[argBase + 2]);
		std::shared_ptr<awst::Expression> sizeOnce = outSizeOpt
			? std::shared_ptr<awst::Expression>(
				awst::makeIntegerConstant(*outSizeOpt, _loc))
			: awst::makeEvalOnce(offsetToUint64(
				buildExpression(_call.arguments[argBase + 3]), _loc), _loc);
		auto pad = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
		pad->stackArgs.push_back(sizeOnce);
		auto padded = awst::makeConcat(returndataBytes(_loc), std::move(pad), _loc);
		auto sliced = awst::makeExtract3(std::move(padded),
			awst::makeIntegerConstant("0", _loc), sizeOnce, _loc);
		writeMemRangeDyn(std::move(outOff), std::move(sliced), _loc, _out);
	}

	// 5) itxn submission aborts on failure, so reaching here implies success.
	if (!_assignTarget.empty())
	{
		auto localIt = m_locals.find(_assignTarget);
		auto* varType = (localIt != m_locals.end()) ? localIt->second : awst::WType::biguintType();
		if (localIt == m_locals.end())
			m_locals[_assignTarget] = varType;
		std::shared_ptr<awst::Expression> val = (varType == awst::WType::boolType())
			? std::shared_ptr<awst::Expression>(awst::makeBoolConstant(true, _loc))
			: awst::makeIntegerConstant("1", _loc, awst::WType::biguintType());
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(_assignTarget, varType, _loc), std::move(val), _loc));
	}
}

} // namespace puyasol::builder
