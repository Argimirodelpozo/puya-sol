/// @file PrecompileDispatch.cpp
/// EVM precompile dispatch: routes call/staticcall to specific precompile handlers.

#include "builder/assembly/AssemblyBuilder.h"
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

	// Normalize argument positions: call has extra `value` at position 2
	int argBase = _isCall ? 3 : 2;

	// Try to resolve the precompile address (arg index 1)
	auto precompileAddr = resolveConstantYulValue(_call.arguments[1]);
	if (!precompileAddr)
	{
		// Non-constant address ⇒ user-defined contract call.
		// Lower to inner app call against the address-encoded app id.
		handleAppCall(_call, _assignTarget, _loc, _out, _isCall);
		return;
	}

	// Resolve input/output memory offsets and sizes
	auto inputOffset = resolveConstantYulValue(_call.arguments[argBase]);
	auto inputSize = resolveConstantYulValue(_call.arguments[argBase + 1]);
	auto outputOffset = resolveConstantYulValue(_call.arguments[argBase + 2]);
	auto outputSize = resolveConstantYulValue(_call.arguments[argBase + 3]);

	if (!inputOffset || !inputSize || !outputOffset || !outputSize)
	{
		Logger::instance().warning(
			"precompile call with non-constant memory offsets/sizes — stubbing as success", _loc
		);
		// Set success variable — use the variable's declared type (bool for Solidity bool)
		if (!_assignTarget.empty())
		{
			auto localIt = m_locals.find(_assignTarget);
			auto* varType = (localIt != m_locals.end()) ? localIt->second : awst::WType::biguintType();

			auto assignStmt = std::make_shared<awst::AssignmentStatement>();
			assignStmt->sourceLocation = _loc;
			auto varExpr = awst::makeVarExpression(_assignTarget, varType, _loc);
			assignStmt->target = std::move(varExpr);

			if (varType == awst::WType::boolType())
			{
				assignStmt->value = awst::makeBoolConstant(true, _loc);
			}
			else
			{
				auto val = awst::makeIntegerConstant("1", _loc, awst::WType::biguintType());
				assignStmt->value = std::move(val);
			}
			_out.push_back(std::move(assignStmt));
		}
		return;
	}

	bool success = true;

	switch (*precompileAddr)
	{
	case 1: // ecRecover
		Logger::instance().debug("precompile 0x01: ecRecover", _loc);
		handleEcRecover(*inputOffset, *inputSize, *outputOffset, *outputSize, _loc, _out);
		break;

	case 2: // SHA-256
		Logger::instance().debug("precompile 0x02: SHA-256", _loc);
		handleSha256Precompile(*inputOffset, *inputSize, *outputOffset, *outputSize, _loc, _out);
		break;

	case 3: // RIPEMD-160
		Logger::instance().error(
			"precompile 0x03 (RIPEMD-160) not yet supported on AVM", _loc
		);
		success = false;
		break;

	case 4: // Identity (data copy)
		Logger::instance().debug("precompile 0x04: Identity", _loc);
		handleIdentityPrecompile(*inputOffset, *inputSize, *outputOffset, *outputSize, _loc, _out);
		break;

	case 5: // ModExp
		Logger::instance().debug("precompile 0x05: ModExp (square-and-multiply)", _loc);
		handleModExp(*inputOffset, *inputSize, *outputOffset, *outputSize, _loc, _out);
		break;

	case 6: // ecAdd
		Logger::instance().debug("precompile 0x06: ecAdd → AVM ec_add BN254g1", _loc);
		handleEcAdd(*inputOffset, *outputOffset, _loc, _out);
		break;

	case 7: // ecMul
		Logger::instance().debug("precompile 0x07: ecMul → AVM ec_scalar_mul BN254g1", _loc);
		handleEcMul(*inputOffset, *outputOffset, _loc, _out);
		break;

	case 8: // ecPairing
		Logger::instance().debug("precompile 0x08: ecPairing → AVM ec_pairing_check BN254g1", _loc);
		handleEcPairing(*inputOffset, *inputSize, *outputOffset, _loc, _out);
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
		Logger::instance().warning(
			(_isCall ? std::string("call") : std::string("staticcall")) +
			" to non-precompile address " + std::to_string(*precompileAddr) +
			" not implemented — stubbing as no-op", _loc
		);
		success = true;
		break;
	}

	// Set the success variable — use the variable's declared type (bool for Solidity bool)
	if (!_assignTarget.empty())
	{
		auto localIt = m_locals.find(_assignTarget);
		auto* varType = (localIt != m_locals.end()) ? localIt->second : awst::WType::biguintType();
		// Only set m_locals if not already tracked (preserve Solidity-declared type)
		if (localIt == m_locals.end())
			m_locals[_assignTarget] = varType;

		auto target = awst::makeVarExpression(_assignTarget, varType, _loc);

		std::shared_ptr<awst::Expression> val;
		if (varType == awst::WType::boolType())
		{
			val = awst::makeBoolConstant(success, _loc);
		}
		else
		{
			auto intVal = awst::makeIntegerConstant(success ? "1" : "0", _loc, awst::WType::biguintType());
			val = std::move(intVal);
		}

		auto assign = awst::makeAssignmentStatement(std::move(target), std::move(val), _loc);
		_out.push_back(std::move(assign));
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
	// Argument layout:
	//   call(gas, addr, value, inOff, inSize, outOff, outSize)        (7 args)
	//   staticcall(gas, addr, inOff, inSize, outOff, outSize)         (6 args)
	int argBase = _isCall ? 3 : 2;

	Logger::instance().debug(
		std::string(_isCall ? "call" : "staticcall") +
		" to runtime address — lowering to inner app call", _loc);

	// 1) Address argument → ApplicationID.
	//    puya-sol encodes addresses as (\x00*24 ++ itob(app_id)). Numerically,
	//    interpreting that as a uint256 yields exactly app_id (high bytes are
	//    zero), so casting to uint64 recovers the application id.
	auto addrAwst = buildExpression(_call.arguments[1]);

	std::shared_ptr<awst::Expression> appIdExpr;
	if (addrAwst->wtype == awst::WType::applicationType())
	{
		appIdExpr = std::move(addrAwst);
	}
	else if (addrAwst->wtype == awst::WType::accountType())
	{
		auto toBytes = awst::makeReinterpretCast(std::move(addrAwst), awst::WType::bytesType(), _loc);
		auto extract = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
		extract->immediates = {24, 8};
		extract->stackArgs.push_back(std::move(toBytes));
		auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
		btoi->stackArgs.push_back(std::move(extract));
		appIdExpr = awst::makeReinterpretCast(std::move(btoi), awst::WType::applicationType(), _loc);
	}
	else
	{
		// Treat as a numeric value already representing the app id (low 64 bits).
		auto asU64 = awst::makeReinterpretCast(std::move(addrAwst), awst::WType::uint64Type(), _loc);
		appIdExpr = awst::makeReinterpretCast(std::move(asU64), awst::WType::applicationType(), _loc);
	}

	// 2) Split calldata into selector (first 4 bytes) and body (the rest),
	//    matching Algorand's ApplicationArgs convention where args[0] is the
	//    method selector. EVM-ABI calldata layout = selector(4) ++ args.
	auto inOffAwst = buildExpression(_call.arguments[argBase]);
	if (inOffAwst->wtype != awst::WType::uint64Type())
		inOffAwst = awst::makeReinterpretCast(std::move(inOffAwst), awst::WType::uint64Type(), _loc);
	auto inSizeAwst = buildExpression(_call.arguments[argBase + 1]);
	if (inSizeAwst->wtype != awst::WType::uint64Type())
		inSizeAwst = awst::makeReinterpretCast(std::move(inSizeAwst), awst::WType::uint64Type(), _loc);

	// args[0] = first 4 bytes (selector)
	auto selector = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	selector->stackArgs.push_back(memoryVar(_loc));
	selector->stackArgs.push_back(inOffAwst);
	selector->stackArgs.push_back(awst::makeIntegerConstant("4", _loc));

	// args[1] = bytes [inOff+4 .. inOff+inSize)
	auto bodyOff = awst::makeIntrinsicCall("+", awst::WType::uint64Type(), _loc);
	bodyOff->stackArgs.push_back(inOffAwst);
	bodyOff->stackArgs.push_back(awst::makeIntegerConstant("4", _loc));

	auto bodyLen = awst::makeIntrinsicCall("-", awst::WType::uint64Type(), _loc);
	bodyLen->stackArgs.push_back(std::move(inSizeAwst));
	bodyLen->stackArgs.push_back(awst::makeIntegerConstant("4", _loc));

	auto body = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	body->stackArgs.push_back(memoryVar(_loc));
	body->stackArgs.push_back(std::move(bodyOff));
	body->stackArgs.push_back(std::move(bodyLen));

	// 3) Build CreateInnerTransaction (TypeEnum=Appl=6, OnCompletion=NoOp=0).
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
	submit->itxns.push_back(std::move(create));

	_out.push_back(awst::makeExpressionStatement(std::move(submit), _loc));

	// 4) Optional: copy itxn LastLog back into memory[outOff..outOff+outSize).
	auto outOffOpt = resolveConstantYulValue(_call.arguments[argBase + 2]);
	auto outSizeOpt = resolveConstantYulValue(_call.arguments[argBase + 3]);
	if (outOffOpt && outSizeOpt && *outSizeOpt > 0)
	{
		auto readLog = awst::makeIntrinsicCall("itxn", awst::WType::bytesType(), _loc);
		readLog->immediates = {std::string("LastLog")};

		// Slice the requested number of bytes from the front of the log.
		auto sliced = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
		sliced->stackArgs.push_back(std::move(readLog));
		sliced->stackArgs.push_back(awst::makeIntegerConstant("0", _loc));
		sliced->stackArgs.push_back(awst::makeIntegerConstant(std::to_string(*outSizeOpt), _loc));

		// outputSize is in bytes; storeResultToMemory takes 32-byte slot count,
		// rounding up to whole slots.
		int slots = static_cast<int>((*outSizeOpt + 31) / 32);
		if (slots > 0)
			storeResultToMemory(std::move(sliced), *outOffOpt, slots, _loc, _out);
	}

	// 5) Set _assignTarget to success (1). itxn submission aborts on failure,
	//    so reaching this point implies success.
	if (!_assignTarget.empty())
	{
		auto localIt = m_locals.find(_assignTarget);
		auto* varType = (localIt != m_locals.end()) ? localIt->second : awst::WType::biguintType();
		if (localIt == m_locals.end())
			m_locals[_assignTarget] = varType;

		auto target = awst::makeVarExpression(_assignTarget, varType, _loc);
		std::shared_ptr<awst::Expression> val;
		if (varType == awst::WType::boolType())
			val = awst::makeBoolConstant(true, _loc);
		else
			val = awst::makeIntegerConstant("1", _loc, awst::WType::biguintType());
		_out.push_back(awst::makeAssignmentStatement(std::move(target), std::move(val), _loc));
	}
}

} // namespace puyasol::builder
