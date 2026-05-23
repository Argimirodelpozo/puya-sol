/// @file SolStructBuilder.cpp
/// Solidity struct type builder.

#include "builder/sol-eb/SolStructBuilder.h"

namespace puyasol::builder::eb
{

std::unique_ptr<InstanceBuilder> SolStructBuilder::compare(
	InstanceBuilder& _other, BuilderComparisonOp _op,
	awst::SourceLocation const& _loc)
{
	// Structs only support Eq/Ne (compare encoded bytes)
	if (_op != BuilderComparisonOp::Eq && _op != BuilderComparisonOp::Ne)
		return nullptr;

	// Only compare structs of the same kind
	auto const* otherStruct = dynamic_cast<solidity::frontend::StructType const*>(_other.solType());
	if (!otherStruct)
		return nullptr;

	// For ARC4Struct: compare the encoded bytes representation
	if (wtype() && wtype()->kind() == awst::WTypeKind::ARC4Struct)
	{
		// Encode both to bytes for comparison
		auto lhs = resolve();
		auto rhs = _other.resolve();

		auto lhsBytes = awst::makeAsBytes(std::move(lhs), _loc);

		auto rhsBytes = awst::makeAsBytes(std::move(rhs), _loc);

		auto e = awst::makeBytesComparison(std::move(lhsBytes),
			(_op == BuilderComparisonOp::Eq) ? awst::EqualityComparison::Eq : awst::EqualityComparison::Ne,
			std::move(rhsBytes), _loc);
		return std::make_unique<SolStructBuilder>(m_ctx, m_structType, std::move(e));
	}

	// For WTuple: not directly comparable — return nullptr to fall through
	return nullptr;
}

} // namespace puyasol::builder::eb
