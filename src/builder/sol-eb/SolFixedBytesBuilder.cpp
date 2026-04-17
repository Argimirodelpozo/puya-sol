/// @file SolFixedBytesBuilder.cpp
/// Solidity fixed-size bytes type builder (bytes1..bytes32).

#include "builder/sol-eb/SolFixedBytesBuilder.h"
#include "builder/sol-types/TypeMapper.h"

namespace puyasol::builder::eb
{

SolFixedBytesBuilder::SolFixedBytesBuilder(
	BuilderContext& _ctx,
	solidity::frontend::FixedBytesType const* _bytesType,
	std::shared_ptr<awst::Expression> _expr)
	: InstanceBuilder(_ctx, std::move(_expr)),
	  m_bytesType(_bytesType),
	  m_numBytes(_bytesType->numBytes())
{
}

std::unique_ptr<InstanceBuilder> SolFixedBytesBuilder::binary_op(
	InstanceBuilder& _other, BuilderBinaryOp _op,
	awst::SourceLocation const& _loc, bool _reverse)
{
	// Only handle bitwise ops on bytes-backed types
	bool isBitwiseOp = (_op == BuilderBinaryOp::BitOr
		|| _op == BuilderBinaryOp::BitXor
		|| _op == BuilderBinaryOp::BitAnd);
	if (!isBitwiseOp)
		return nullptr;

	// Accept other bytes-backed types
	bool otherIsBytes = _other.wtype() && _other.wtype()->kind() == awst::WTypeKind::Bytes;
	if (!otherIsBytes)
		return nullptr;

	auto lhs = resolve();
	auto rhs = _other.resolve();
	if (_reverse)
		std::swap(lhs, rhs);

	auto e = std::make_shared<awst::BytesBinaryOperation>();
	e->sourceLocation = _loc;
	e->wtype = awst::WType::bytesType();
	e->left = std::move(lhs);
	e->right = std::move(rhs);

	switch (_op)
	{
	case BuilderBinaryOp::BitOr: e->op = awst::BytesBinaryOperator::BitOr; break;
	case BuilderBinaryOp::BitXor: e->op = awst::BytesBinaryOperator::BitXor; break;
	case BuilderBinaryOp::BitAnd: e->op = awst::BytesBinaryOperator::BitAnd; break;
	default: e->op = awst::BytesBinaryOperator::BitOr; break;
	}
	return std::make_unique<SolFixedBytesBuilder>(m_ctx, m_bytesType, std::move(e));
}

std::unique_ptr<InstanceBuilder> SolFixedBytesBuilder::compare(
	InstanceBuilder& _other, BuilderComparisonOp _op,
	awst::SourceLocation const& _loc)
{
	// Accept other bytes-backed or account types
	bool otherIsBytes = _other.wtype() && _other.wtype()->kind() == awst::WTypeKind::Bytes;
	bool otherIsAccount = _other.wtype() == awst::WType::accountType();
	if (!otherIsBytes && !otherIsAccount)
		return nullptr;

	// Equality/inequality: BytesComparisonExpression
	if (_op == BuilderComparisonOp::Eq || _op == BuilderComparisonOp::Ne)
	{
		auto lhs = resolve();
		auto rhs = _other.resolve();

		// EVM stores bytesN as 32-byte words, left-aligned. Comparing
		// bytes3("abc") to bytes4("abc") is true on EVM because both
		// pad to "abc + zeros" in 32-byte form. On AVM our BytesConstants
		// hold N raw bytes, so a 3-byte vs 4-byte comparison is always
		// false. Right-pad any literal whose underlying constant is
		// shorter than the other side so the byte-level == matches EVM.
		auto padConstant = [&](std::shared_ptr<awst::Expression>& expr, size_t targetLen) {
			auto* bc = dynamic_cast<awst::BytesConstant*>(expr.get());
			if (!bc) return;
			if (bc->value.size() >= targetLen) return;
			auto* newType = m_ctx.typeMapper.createType<awst::BytesWType>(
				static_cast<int>(targetLen));
			auto val = bc->value;
			val.resize(targetLen, 0);
			expr = awst::makeBytesConstant(std::move(val), bc->sourceLocation, bc->encoding, newType);
		};
		auto bytesLen = [](awst::Expression const& e) -> size_t {
			if (auto const* bw = dynamic_cast<awst::BytesWType const*>(e.wtype))
				if (bw->length().has_value())
					return *bw->length();
			return 0;
		};
		size_t lhsLen = bytesLen(*lhs);
		size_t rhsLen = bytesLen(*rhs);
		size_t common = std::max(lhsLen, rhsLen);
		if (common > 0)
		{
			padConstant(lhs, common);
			padConstant(rhs, common);
		}

		// Coerce to same wtype if needed
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
		return std::make_unique<SolFixedBytesBuilder>(m_ctx, m_bytesType, std::move(e));
	}

	// Ordering: use AVM b</b>/b<=/b>= intrinsics
	std::string opCode;
	switch (_op)
	{
	case BuilderComparisonOp::Lt: opCode = "b<"; break;
	case BuilderComparisonOp::Lte: opCode = "b<="; break;
	case BuilderComparisonOp::Gt: opCode = "b>"; break;
	case BuilderComparisonOp::Gte: opCode = "b>="; break;
	default: return nullptr;
	}

	auto e = std::make_shared<awst::IntrinsicCall>();
	e->sourceLocation = _loc;
	e->wtype = awst::WType::boolType();
	e->opCode = std::move(opCode);
	e->stackArgs.push_back(resolve());
	e->stackArgs.push_back(_other.resolve());
	return std::make_unique<SolFixedBytesBuilder>(m_ctx, m_bytesType, std::move(e));
}

std::unique_ptr<InstanceBuilder> SolFixedBytesBuilder::bool_eval(
	awst::SourceLocation const& _loc, bool _negate)
{
	// bytes != zero_bytes (or == if negated)
	auto zero = awst::makeBytesConstant(
		std::vector<uint8_t>(m_numBytes, 0), _loc, awst::BytesEncoding::Base16,
		m_expr->wtype); // same bytes[N] type

	auto e = std::make_shared<awst::BytesComparisonExpression>();
	e->sourceLocation = _loc;
	e->wtype = awst::WType::boolType();
	e->lhs = resolve();
	e->rhs = std::move(zero);
	e->op = _negate ? awst::EqualityComparison::Eq : awst::EqualityComparison::Ne;
	return std::make_unique<SolFixedBytesBuilder>(m_ctx, m_bytesType, std::move(e));
}

} // namespace puyasol::builder::eb
