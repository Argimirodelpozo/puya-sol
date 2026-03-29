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

		auto lhsBytes = std::make_shared<awst::ReinterpretCast>();
		lhsBytes->sourceLocation = _loc;
		lhsBytes->wtype = awst::WType::bytesType();
		lhsBytes->expr = std::move(lhs);

		auto rhsBytes = std::make_shared<awst::ReinterpretCast>();
		rhsBytes->sourceLocation = _loc;
		rhsBytes->wtype = awst::WType::bytesType();
		rhsBytes->expr = std::move(rhs);

		auto e = std::make_shared<awst::BytesComparisonExpression>();
		e->sourceLocation = _loc;
		e->wtype = awst::WType::boolType();
		e->lhs = std::move(lhsBytes);
		e->rhs = std::move(rhsBytes);
		e->op = (_op == BuilderComparisonOp::Eq)
			? awst::EqualityComparison::Eq
			: awst::EqualityComparison::Ne;
		return std::make_unique<SolStructBuilder>(m_ctx, m_structType, std::move(e));
	}

	// For WTuple: not directly comparable — return nullptr to fall through
	return nullptr;
}

} // namespace puyasol::builder::eb
