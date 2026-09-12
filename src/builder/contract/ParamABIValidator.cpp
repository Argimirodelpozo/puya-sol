#include "builder/contract/ParamABIValidator.h"
#include "builder/contract/ContractBuilder.h"
#include "builder/codec/EvmValueCodec.h"

namespace puyasol::builder
{

std::vector<std::shared_ptr<awst::Statement>> buildABIEntryChecks(
	std::vector<ABIParamDesc> const& params, bool useABICoderV2, bool enumChecksRequireV2)
{
	std::vector<std::shared_ptr<awst::Statement>> out;
	for (auto const& parameter: params)
	{
		auto const rules = codec::scalarBoundary(parameter.solType,
			useABICoderV2 ? codec::PaddingPolicy::Validate : codec::PaddingPolicy::Clean,
			enumChecksRequireV2 ? codec::ScalarBoundarySite::NativeGetter : codec::ScalarBoundarySite::NativeParameter);
		auto const& loc = parameter.loc;
		auto value = awst::makeVarExpression(parameter.name, awst::WType::uint64Type(), loc);
		auto assertRange = [&](auto condition, char const* message) {
			out.push_back(awst::makeExpressionStatement(awst::makeAssert(std::move(condition), loc, message), loc));
		};
		if (auto const& integer = rules.integer; integer && integer->bits < 64)
		{
			unsigned const bits = integer->bits;
			if (integer->isSigned)
			{
				// This is a native uint64 two's-complement carrier, not an EVM word.
				// Preserve the existing v1 signed-input convention (no cleanup).
				if (rules.validatePadding)
				{
					uint64_t const max = (uint64_t{1} << (bits - 1)) - 1;
					assertRange(awst::makeBoolBinOp(
						awst::makeNumericCompare(value, awst::NumericComparison::Lte, awst::makeIntegerConstant(max, loc), loc),
						awst::BinaryBooleanOperator::Or,
						awst::makeNumericCompare(value, awst::NumericComparison::Gte, awst::makeIntegerConstant(~max, loc), loc), loc),
						"ABI validation");
				}
			}
			else
			{
				auto mask = awst::makeIntegerConstant((uint64_t{1} << bits) - 1, loc);
				if (rules.validatePadding)
					assertRange(awst::makeNumericCompare(value, awst::NumericComparison::Lte, mask, loc), "ABI validation");
				out.push_back(awst::makeAssignmentStatement(value,
					awst::makeUInt64BinOp(value, awst::UInt64BinaryOperator::BitAnd, mask, loc), loc));
			}
		}
		if (rules.boolean && rules.validatePadding)
			assertRange(awst::makeNumericCompare(value, awst::NumericComparison::Lte, awst::makeOne(loc), loc), "ABI bool validation");
		if (rules.validateEnum)
			assertRange(awst::makeNumericCompare(value, awst::NumericComparison::Lt,
				awst::makeIntegerConstant(rules.enumMembers, loc), loc), "ABI enum validation");
	}
	return out;
}

std::vector<std::shared_ptr<awst::Statement>> buildABIEntryChecks(
	solidity::frontend::FunctionDefinition const& _func,
	TypeMapper const& _typeMapper,
	bool _useABICoderV2,
	std::string const& _sourceFile)
{
	std::vector<ABIParamDesc> descs;
	descs.reserve(_func.parameters().size());
	for (size_t pi = 0; pi < _func.parameters().size(); ++pi)
	{
		auto const& param = _func.parameters()[pi];
		std::string name = param->name().empty()
			? "_param" + std::to_string(pi) : param->name();
		descs.push_back({param->annotation().type, std::move(name),
			makeLoc(_typeMapper, _sourceFile, param->location())});
	}
	return buildABIEntryChecks(descs, _useABICoderV2);
}

} // namespace puyasol::builder
