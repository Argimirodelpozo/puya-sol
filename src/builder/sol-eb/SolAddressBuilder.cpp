/// @file SolAddressBuilder.cpp
/// Solidity address/contract type builder.

#include "builder/sol-eb/SolAddressBuilder.h"

namespace puyasol::builder::eb
{

std::unique_ptr<InstanceBuilder> SolAddressBuilder::compare(
	InstanceBuilder& _other, BuilderComparisonOp _op,
	awst::SourceLocation const& _loc)
{
	// Address supports only Eq/Ne comparison (bytes-backed)
	if (_op != BuilderComparisonOp::Eq && _op != BuilderComparisonOp::Ne)
		return nullptr;

	// Accept other address/account types, or bytes-backed types
	auto* otherWType = _other.wtype();
	bool otherIsAccount = otherWType == awst::WType::accountType();
	bool otherIsBytes = otherWType && otherWType->kind() == awst::WTypeKind::Bytes;
	if (!otherIsAccount && !otherIsBytes)
		return nullptr;

	auto lhs = resolve();
	auto rhs = _other.resolve();

	// Coerce to same type for comparison
	auto coerceToBytes = [&](std::shared_ptr<awst::Expression>& expr) {
		if (expr->wtype != awst::WType::bytesType()
			&& expr->wtype != awst::WType::accountType())
		{
			auto cast = awst::makeReinterpretCast(std::move(expr), awst::WType::bytesType(), _loc);
			expr = std::move(cast);
		}
	};
	if (lhs->wtype != rhs->wtype)
	{
		coerceToBytes(lhs);
		coerceToBytes(rhs);
	}

	auto e = std::make_shared<awst::BytesComparisonExpression>();
	e->sourceLocation = _loc;
	e->wtype = awst::WType::boolType();
	e->lhs = std::move(lhs);
	e->rhs = std::move(rhs);
	e->op = (_op == BuilderComparisonOp::Eq)
		? awst::EqualityComparison::Eq
		: awst::EqualityComparison::Ne;
	return std::make_unique<SolAddressBuilder>(m_ctx, m_solType, std::move(e));
}

std::unique_ptr<InstanceBuilder> SolAddressBuilder::bool_eval(
	awst::SourceLocation const& _loc, bool _negate)
{
	// address is truthy if != zero_address (32 zero bytes)
	auto zero = awst::makeBytesConstant(
		std::vector<uint8_t>(32, 0), _loc, awst::BytesEncoding::Base16,
		awst::WType::accountType());

	auto e = std::make_shared<awst::BytesComparisonExpression>();
	e->sourceLocation = _loc;
	e->wtype = awst::WType::boolType();
	e->lhs = resolve();
	e->rhs = std::move(zero);
	e->op = _negate ? awst::EqualityComparison::Eq : awst::EqualityComparison::Ne;
	return std::make_unique<SolAddressBuilder>(m_ctx, m_solType, std::move(e));
}

} // namespace puyasol::builder::eb
