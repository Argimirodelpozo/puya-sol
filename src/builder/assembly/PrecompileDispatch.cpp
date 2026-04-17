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
		Logger::instance().error(
			(_isCall ? std::string("call") : std::string("staticcall")) +
			" with non-constant address not supported", _loc
		);
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

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _loc;
		assign->target = std::move(target);
		assign->value = std::move(val);
		_out.push_back(std::move(assign));
	}
}

// ─── BN254 precompile handlers ──────────────────────────────────────────────


} // namespace puyasol::builder
