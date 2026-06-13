#include "builder/contract/ParamABIValidator.h"
#include "builder/contract/ContractBuilder.h"

namespace puyasol::builder
{

// Core: synthesize the entry guards from resolved (type, name, loc)
// descriptors. Shared by the FunctionDefinition entry point and the
// public-state-var getter path. (Extracted verbatim — the only change is
// reading desc fields instead of param->annotation()/name()/location().)
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
		// Enums have uint8 ABI encoding
		if (!intType)
			if (auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(solType))
				intType = dynamic_cast<solidity::frontend::IntegerType const*>(
					enumType->encodingType());
		if (!intType || intType->numBits() >= 64)
			continue;

		unsigned bits = intType->numBits();
		auto loc = d.loc;

		if (intType->isSigned())
		{
			// Signed sub-64-bit types: validate range but don't mask
			// Valid: value <= maxPos || value >= minNeg
			// maxPos = 2^(n-1) - 1, minNeg = 2^64 - 2^(n-1)
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
			// No masking for signed types
			continue;
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

	// ABI v2 validation for bool params: assert value <= 1
	if (_useABICoderV2)
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

	// Enum range check fires regardless of abicoder version. Solidity
	// semantics: any read of an enum value with `numericValue >= numberOfMembers`
	// panics (0x21). For abicoder v2 the check is at the dispatch
	// boundary; for v1 solc inserts it at the first use site. We emit
	// it at the boundary for both versions — equivalent for params
	// that are read at least once in the body, and a strict superset
	// otherwise (the panic happens earlier, which is still correct).
	for (auto const& d : _params)
	{
		auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(d.solType);
		if (!enumType)
			continue;
		// Auto-getter enum keys are NOT range-checked under abicoder v1
		// (they index the mapping directly); user methods that read the
		// enum panic under both versions.
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
