/// @file PrecompileDispatch.cpp
/// Yul call operands are evaluated once before selecting the shared transport.

#include "builder/assembly/AssemblyBuilder.h"
#include "builder/EvmFeaturePolicy.h"
#include "builder/itxn/ApplicationCall.h"
#include "builder/itxn/Precompile.h"
#include "builder/itxn/NativePayment.h"
#include "Logger.h"
#include <libyul/AST.h>

namespace puyasol::builder
{

void AssemblyBuilder::handlePrecompileCall(
	solidity::yul::FunctionCall const& _call, std::string const& _assignTarget,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out, bool _isCall)
{
	if (_call.arguments.size() != (_isCall ? 7u : 6u))
	{
		Logger::instance().error("invalid Yul call argument count", _loc);
		return;
	}
	EvmFeaturePolicy::report(_isCall ? EvmFeature::LowLevelCallOutcome
		: EvmFeature::StaticCall, m_typeMapper.profile(), _loc);

	// Including the gas/value operands: ignoring a value never means that
	// its expression's effects may be skipped. Result operands come first.
	auto args = buildCallOperands(_call, _out);
	for (auto const& arg: args) if (!arg) return;
	invalidateMemConstants();
	size_t base = _isCall ? 3 : 2;
	auto outputSize = offsetToUint64(args[base + 3], _loc);
	auto outputOffset = checkedMemoryRangeOffset(scratchLayout(),
		offsetToUint64(args[base + 2], _loc), outputSize, _loc, false);
	// Validate the requested output window even when the result is shorter.
	_out.push_back(awst::makeExpressionStatement(outputOffset, _loc));
	auto input = readMemRangeDyn(args[base], args[base + 1], _loc, _out);
	auto address = resolveConstantOffset(args[1]);
	std::shared_ptr<awst::Expression> result;
	if (address && *address >= 1 && *address <= 10)
	{
		if (_isCall)
			_out.push_back(awst::makeExpressionStatement(awst::makeAssert(
				awst::makeNumericCompare(ensureBiguint(args[2], _loc), awst::NumericComparison::Eq,
					awst::makeBiguintConstant("0", _loc), _loc), _loc,
				"value-bearing precompile calls are not supported on AVM"), _loc));
		result = evaluatePrecompile(m_typeMapper, *address, input, _loc, _out);
		if (!result) return;
		result = ApplicationCall::setReturnData(m_typeMapper, std::move(result), _loc, _out);
	}
	else
	{
		std::shared_ptr<awst::Expression> payment;
		if (_isCall && resolveConstantOffset(args[2]) != 0)
			payment = buildNativePayment(m_typeMapper.profile(), _out, args[1], args[2], _loc);
		result = ApplicationCall::submitRaw(m_typeMapper, args[1], input, std::move(payment), _loc, _out);
	}
	// EVM copies min(outSize, result.size), never zero-filling the destination
	// tail. Return data retains the entire result independently of this window.
	auto length = awst::makeLen(result, _loc);
	auto copySize = awst::makeConditional(
		awst::makeNumericCompare(outputSize, awst::NumericComparison::Lt, length, _loc),
		outputSize, length, awst::WType::uint64Type(), _loc);
	writeMemRangeDyn(outputOffset,
		awst::makeExtract3(result, awst::makeZero(_loc), copySize, _loc), _loc, _out);
	if (!_assignTarget.empty())
		emitPlainYulAssignment(_assignTarget, awst::makeBiguintConstant("1", _loc), _loc, _out);
}

} // namespace puyasol::builder
