/// @file SolStructBuilder.cpp
/// Solidity struct type builder.

#include "builder/sol-eb/SolStructBuilder.h"

namespace puyasol::builder::eb
{

std::unique_ptr<InstanceBuilder> SolStructBuilder::compare(
	InstanceBuilder& _other, BuilderComparisonOp _op,
	awst::SourceLocation const& _loc)
{
	if (_op != BuilderComparisonOp::Eq && _op != BuilderComparisonOp::Ne)
		return nullptr;

	auto const* otherStruct = dynamic_cast<solidity::frontend::StructType const*>(_other.solType());
	if (!otherStruct)
		return nullptr;

	if (wtype() && wtype()->kind() == awst::WTypeKind::ARC4Struct)
	{
		auto lhs = resolve();
		auto rhs = _other.resolve();

		auto lhsBytes = awst::makeAsBytes(std::move(lhs), _loc);

		auto rhsBytes = awst::makeAsBytes(std::move(rhs), _loc);

		auto e = awst::makeBytesComparison(std::move(lhsBytes),
			(_op == BuilderComparisonOp::Eq) ? awst::EqualityComparison::Eq : awst::EqualityComparison::Ne,
			std::move(rhsBytes), _loc);
		return std::make_unique<SolStructBuilder>(m_ctx, m_structType, std::move(e));
	}

	return nullptr; // WTuple: not directly comparable
}

} // namespace puyasol::builder::eb
