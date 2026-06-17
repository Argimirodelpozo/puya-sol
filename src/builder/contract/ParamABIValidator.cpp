#include "builder/contract/ParamABIValidator.h"
#include "builder/contract/ContractBuilder.h"

namespace puyasol::builder
{

std::vector<std::shared_ptr<awst::Statement>> buildABIEntryChecks(
	std::vector<ABIParamDesc> const& _params,
	bool _useABICoderV2,
	bool _enumChecksRequireV2)
{
	std::vector<std::shared_ptr<awst::Statement>> maskStmts;

	for (auto const& d : _params)
	{
		auto const* solType = d.solType;
		if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
			solType = &udvt->underlyingType();
		auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(solType);
		if (!intType) // enums → uint8 ABI encoding
			if (auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(solType))
				intType = dynamic_cast<solidity::frontend::IntegerType const*>(
					enumType->encodingType());
		if (!intType || intType->numBits() >= 64)
			continue;

		unsigned bits = intType->numBits();
		auto loc = d.loc;

		if (intType->isSigned())
		{
			// Signed sub-64: v2 assert param≤maxPos || param≥minNeg; no masking.
			if (_useABICoderV2)
			{
				uint64_t maxPos = (uint64_t(1) << (bits - 1)) - 1;
				uint64_t minNeg = ~((uint64_t(1) << (bits - 1)) - 1); // 2^64 - 2^(n-1)

				auto paramCheck1 = awst::makeVarExpression(d.name, awst::WType::uint64Type(), loc);
				auto maxPosConst = awst::makeIntegerConstant(maxPos, loc);
				auto cmpPos = awst::makeNumericCompare(paramCheck1, awst::NumericComparison::Lte, std::move(maxPosConst), loc);

				auto paramCheck2 = awst::makeVarExpression(d.name, awst::WType::uint64Type(), loc);
				auto minNegConst = awst::makeIntegerConstant(minNeg, loc);
				auto cmpNeg = awst::makeNumericCompare(paramCheck2, awst::NumericComparison::Gte, std::move(minNegConst), loc);

				// OR the two conditions
				auto orExpr = awst::makeBoolBinOp(std::move(cmpPos), awst::BinaryBooleanOperator::Or, std::move(cmpNeg), loc);
				auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(orExpr), loc, "ABI validation"), loc);
				maskStmts.push_back(std::move(assertStmt));
			}
				continue; // no masking for signed types
		}

		uint64_t mask = (uint64_t(1) << bits) - 1;

		if (_useABICoderV2)
		{
			auto paramCheck = awst::makeVarExpression(d.name, awst::WType::uint64Type(), loc);

			auto maxVal = awst::makeIntegerConstant(mask, loc);

			auto cmp = awst::makeNumericCompare(paramCheck, awst::NumericComparison::Lte, std::move(maxVal), loc);

			auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), loc, "ABI validation"), loc);
			maskStmts.push_back(std::move(assertStmt));
		}

		auto paramVar = awst::makeVarExpression(d.name, awst::WType::uint64Type(), loc);

		auto maskConst = awst::makeIntegerConstant(mask, loc);

		auto bitAnd = awst::makeUInt64BinOp(paramVar, awst::UInt64BinaryOperator::BitAnd, std::move(maskConst), loc);

		auto target = awst::makeVarExpression(d.name, awst::WType::uint64Type(), loc);

		auto assign = awst::makeAssignmentStatement(std::move(target), std::move(bitAnd), loc);
		maskStmts.push_back(std::move(assign));
	}

	if (_useABICoderV2) // bool params: assert value ≤ 1
	{
		for (auto const& d : _params)
		{
			auto const* solType = d.solType;
			if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
				solType = &udvt->underlyingType();
			if (!dynamic_cast<solidity::frontend::BoolType const*>(solType))
				continue;

			auto paramVar = awst::makeVarExpression(d.name, awst::WType::uint64Type(), d.loc);

			auto one = awst::makeOne(d.loc);

			auto cmp = awst::makeNumericCompare(paramVar, awst::NumericComparison::Lte, std::move(one), d.loc);

			auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), d.loc, "ABI bool validation"), d.loc);
			maskStmts.push_back(std::move(assertStmt));
		}
	}

	// Enum range check: emit at boundary for both v1 and v2 (strict superset of
	// solc's first-use-site check). Auto-getter keys are NOT checked under v1
	// (they index the mapping directly) — _enumChecksRequireV2 gates that.
	for (auto const& d : _params)
	{
		auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(d.solType);
		if (!enumType)
			continue;
		if (_enumChecksRequireV2 && !_useABICoderV2)
			continue;
		unsigned memberCount = enumType->numberOfMembers();

		auto paramVar = awst::makeVarExpression(d.name, awst::WType::uint64Type(), d.loc);

		auto maxVal = awst::makeIntegerConstant(memberCount - 1, d.loc);

		auto cmp = awst::makeNumericCompare(paramVar, awst::NumericComparison::Lte, std::move(maxVal), d.loc);

		auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), d.loc, "ABI enum validation"), d.loc);
		maskStmts.push_back(std::move(assertStmt));
	}

	return maskStmts;
}

std::vector<std::shared_ptr<awst::Statement>> buildABIEntryChecks(
	solidity::frontend::FunctionDefinition const& _func,
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
			makeLoc(_sourceFile, param->location())});
	}
	return buildABIEntryChecks(descs, _useABICoderV2);
}

} // namespace puyasol::builder
