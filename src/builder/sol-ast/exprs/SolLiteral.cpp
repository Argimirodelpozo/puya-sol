/// @file SolLiteral.cpp
/// Migrated from LiteralBuilder.cpp.

#include "builder/sol-ast/exprs/SolLiteral.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>
#include <libsolutil/Numeric.h>
#include <sstream>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

SolLiteral::SolLiteral(eb::BuilderContext& _ctx, Literal const& _node)
	: SolExpression(_ctx, _node), m_literal(_node)
{
}

std::shared_ptr<awst::Expression> SolLiteral::toAwst()
{
	switch (m_literal.token())
	{
	case Token::TrueLiteral:
		return awst::makeBoolConstant(true, m_loc);
	case Token::FalseLiteral:
		return awst::makeBoolConstant(false, m_loc);
	case Token::Number:
	{
		auto* mappedType = m_ctx.typeMapper.map(m_solType);
		if (mappedType != awst::WType::uint64Type() && mappedType != awst::WType::biguintType())
			mappedType = awst::WType::biguintType();
		auto e = std::make_shared<awst::IntegerConstant>();
		e->sourceLocation = m_loc;
		if (auto const* ratType = dynamic_cast<RationalNumberType const*>(m_solType))
		{
			auto val = ratType->literalValue(nullptr);
			// literalValue() returns u256 (two's complement for negatives).
			static const solidity::u256 uint64Max("18446744073709551615");
			if (mappedType == awst::WType::uint64Type() && val > uint64Max)
			{
				// Huge u256 values always promote to biguint so the full
				// 256-bit representation is preserved. *Except* for
				// negative signed integers whose storage slot is uint64
				// (signed types with bits ≤ 64): there we want the 64-bit
				// two's complement form (`val mod 2^64`) so that compiled
				// comparisons against `type(intN).min` and other uint64
				// stored variables line up. Without this, -128 would be
				// biguint(2^256 - 128) while int8_min is uint64 0xff..80,
				// and the coerced equality fails.
				bool signedSmall = false;
				if (auto const* intType = dynamic_cast<IntegerType const*>(m_solType))
					signedSmall = intType->isSigned() && intType->numBits() <= 64;
				static const solidity::u256 twoPow64("18446744073709551616");
				if (signedSmall)
				{
					// Wrap to 64 bits: val mod 2^64.
					solidity::u256 wrapped = val % twoPow64;
					e->value = wrapped.str();
				}
				else
				{
					mappedType = awst::WType::biguintType();
					e->value = val.str();
				}
			}
			else
				e->value = val.str();
		}
		else
			e->value = m_literal.value();
		e->wtype = mappedType;
		return e;
	}
	case Token::StringLiteral:
	{
		auto e = std::make_shared<awst::StringConstant>();
		e->sourceLocation = m_loc;
		e->wtype = awst::WType::stringType();
		e->value = m_literal.value();
		return e;
	}
	case Token::HexStringLiteral:
	{
		auto const& raw = m_literal.value();
		return awst::makeBytesConstant(
			std::vector<uint8_t>(raw.begin(), raw.end()), m_loc);
	}
	default:
	{
		auto e = std::make_shared<awst::StringConstant>();
		e->sourceLocation = m_loc;
		e->wtype = awst::WType::stringType();
		e->value = m_literal.value();
		return e;
	}
	}
}

} // namespace puyasol::builder::sol_ast
