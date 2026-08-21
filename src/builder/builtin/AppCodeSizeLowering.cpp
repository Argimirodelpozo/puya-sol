#include "builder/builtin/AppCodeSizeLowering.h"

#include "awst/NameGen.h"
#include "builder/sol-types/TypeMapper.h"

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> AppCodeSizeLowering::lower(
	TypeMapper& _typeMapper,
	std::shared_ptr<awst::Expression> _application,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _effects)
{
	std::string const idName = "__app_code_id_" + std::to_string(
		awst::NameGen::next("AppCodeSizeLowering.s_idCounter") + 1);
	_effects.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(idName, awst::WType::uint64Type(), _loc),
		awst::makeAsUInt64(std::move(_application), _loc), _loc));

	auto* tupleType = _typeMapper.createType<awst::WTuple>(
		std::vector<awst::WType const*>{
			awst::WType::uint64Type(), awst::WType::boolType()});
	auto query = [&]() {
		return awst::makeAppParamsGet(
			"AppExtraProgramPages",
			awst::makeVarExpression(idName, awst::WType::uint64Type(), _loc),
			tupleType, _loc);
	};

	// Keep the tuple values inside the conditional branches instead of
	// assigning the pair to a long-lived temp.  In Yul this expression is often
	// nested beside returndata/memory expressions; a live tuple then expands
	// across that whole expression and can corrupt stack allocation.  The
	// metadata lookup is deliberately repeated only on the existing-app path:
	// one scalar lookup for existence, one for the page count.
	auto exists = awst::makeTupleItem(
		query(), 1, awst::WType::boolType(), _loc);
	auto extraPages = awst::makeTupleItem(
		query(), 0, awst::WType::uint64Type(), _loc);
	auto pageCount = awst::makeUInt64BinOp(
		std::move(extraPages), awst::UInt64BinaryOperator::Add,
		awst::makeIntegerConstant("1", _loc), _loc);
	auto allocatedBytes = awst::makeUInt64BinOp(
		std::move(pageCount), awst::UInt64BinaryOperator::Mult,
		awst::makeIntegerConstant("2048", _loc), _loc);

	auto sizeIfExists = awst::makeConditional(
		std::move(exists), std::move(allocatedBytes),
		awst::makeZero(_loc, awst::WType::uint64Type()),
		awst::WType::uint64Type(), _loc);

	// AVM uses app reference 0 as an alias for the current application.  In
	// the compiler's zero-padded contract-address convention, however, a low
	// 64-bit id of zero is an EOA/non-contract.  Mask that alias explicitly.
	auto nonZeroId = awst::makeNumericCompare(
		awst::makeVarExpression(idName, awst::WType::uint64Type(), _loc),
		awst::NumericComparison::Ne,
		awst::makeZero(_loc, awst::WType::uint64Type()), _loc);
	return awst::makeConditional(
		std::move(nonZeroId), std::move(sizeIfExists),
		awst::makeZero(_loc, awst::WType::uint64Type()),
		awst::WType::uint64Type(), _loc);
}

} // namespace puyasol::builder
