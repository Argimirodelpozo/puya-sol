/// @file SolStringBuilder.cpp
/// Solidity string and dynamic bytes type builders.

#include "builder/sol-eb/SolStringBuilder.h"

namespace puyasol::builder::eb
{

// ─────────────────────────────────────────────────────────────────────
// SolStringBuilder
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> SolStringBuilder::compare(
	InstanceBuilder& _other, BuilderComparisonOp _op,
	awst::SourceLocation const& _loc)
{
	// String only supports Eq/Ne
	if (_op != BuilderComparisonOp::Eq && _op != BuilderComparisonOp::Ne)
		return nullptr;

	// Accept other string or bytes-backed types
	auto* otherWType = _other.wtype();
	if (otherWType != awst::WType::stringType()
		&& otherWType != awst::WType::bytesType())
		return nullptr;

	auto lhs = resolve();
	auto rhs = _other.resolve();

	// Coerce both to bytes if types differ
	auto coerceToBytes = [&](std::shared_ptr<awst::Expression>& expr) {
		if (expr->wtype != awst::WType::bytesType())
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
	return std::make_unique<SolStringBuilder>(m_ctx, m_solType, std::move(e));
}

std::unique_ptr<InstanceBuilder> SolStringBuilder::bool_eval(
	awst::SourceLocation const& _loc, bool _negate)
{
	// string is truthy if len(s) != 0
	auto len = std::make_shared<awst::IntrinsicCall>();
	len->sourceLocation = _loc;
	len->wtype = awst::WType::uint64Type();
	len->opCode = "len";
	len->stackArgs.push_back(resolve());

	auto zero = awst::makeIntegerConstant("0", _loc);

	auto cmp = std::make_shared<awst::NumericComparisonExpression>();
	cmp->sourceLocation = _loc;
	cmp->wtype = awst::WType::boolType();
	cmp->lhs = std::move(len);
	cmp->rhs = std::move(zero);
	cmp->op = _negate ? awst::NumericComparison::Eq : awst::NumericComparison::Ne;
	return std::make_unique<SolStringBuilder>(m_ctx, m_solType, std::move(cmp));
}

// ─────────────────────────────────────────────────────────────────────
// SolDynamicBytesBuilder
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> SolDynamicBytesBuilder::compare(
	InstanceBuilder& _other, BuilderComparisonOp _op,
	awst::SourceLocation const& _loc)
{
	if (_op != BuilderComparisonOp::Eq && _op != BuilderComparisonOp::Ne)
		return nullptr;

	auto* otherWType = _other.wtype();
	if (otherWType != awst::WType::bytesType()
		&& otherWType != awst::WType::stringType()
		&& !(otherWType && otherWType->kind() == awst::WTypeKind::Bytes))
		return nullptr;

	auto lhs = resolve();
	auto rhs = _other.resolve();

	auto coerceToBytes = [&](std::shared_ptr<awst::Expression>& expr) {
		if (expr->wtype != awst::WType::bytesType())
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
	return std::make_unique<SolDynamicBytesBuilder>(m_ctx, m_solType, std::move(e));
}

std::unique_ptr<InstanceBuilder> SolDynamicBytesBuilder::bool_eval(
	awst::SourceLocation const& _loc, bool _negate)
{
	auto len = std::make_shared<awst::IntrinsicCall>();
	len->sourceLocation = _loc;
	len->wtype = awst::WType::uint64Type();
	len->opCode = "len";
	len->stackArgs.push_back(resolve());

	auto zero = awst::makeIntegerConstant("0", _loc);

	auto cmp = std::make_shared<awst::NumericComparisonExpression>();
	cmp->sourceLocation = _loc;
	cmp->wtype = awst::WType::boolType();
	cmp->lhs = std::move(len);
	cmp->rhs = std::move(zero);
	cmp->op = _negate ? awst::NumericComparison::Eq : awst::NumericComparison::Ne;
	return std::make_unique<SolDynamicBytesBuilder>(m_ctx, m_solType, std::move(cmp));
}

} // namespace puyasol::builder::eb
