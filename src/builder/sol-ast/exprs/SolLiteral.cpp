/// @file SolLiteral.cpp — literal expression translation.

#include "builder/sol-ast/exprs/SolLiteral.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>
#include <libsolutil/Numeric.h>
#include <sstream>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

SolLiteral::SolLiteral(eb::ContractContext& _ctx, Literal const& _node)
	: SolExpression(_ctx, _node), m_literal(_node)
{
}

std::shared_ptr<awst::Expression> SolLiteral::toAwst()
{
	switch (m_literal.token())
	{
	case Token::TrueLiteral:
		return awst::makeTrue(m_loc);
	case Token::FalseLiteral:
		return awst::makeFalse(m_loc);
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
				// Signed int≤64 literals: use 64-bit two's-complement (val mod 2^64) so
				// comparisons against type(intN).min and other uint64 vars line up.
				// Without it, -128 → biguint(2^256-128) ≠ uint64 int8_min 0xff..80.
				bool signedSmall = false;
				if (auto const* intType = dynamic_cast<IntegerType const*>(m_solType))
					signedSmall = intType->isSigned() && intType->numBits() <= 64;
				static const solidity::u256 twoPow64("18446744073709551616");
				if (signedSmall)
				{
					solidity::u256 wrapped = val % twoPow64; // wrap to 64 bits
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
		return awst::makeStringConstant(m_literal.value(), m_loc);
	}
	case Token::HexStringLiteral:
	{
		auto const& raw = m_literal.value();
		return awst::makeBytesConstant(
			std::vector<uint8_t>(raw.begin(), raw.end()), m_loc);
	}
	default:
	{
		return awst::makeStringConstant(m_literal.value(), m_loc);
	}
	}
}

} // namespace puyasol::builder::sol_ast
