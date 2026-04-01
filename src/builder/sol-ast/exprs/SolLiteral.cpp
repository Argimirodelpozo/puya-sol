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
	{
		auto e = std::make_shared<awst::BoolConstant>();
		e->sourceLocation = m_loc;
		e->wtype = awst::WType::boolType();
		e->value = true;
		return e;
	}
	case Token::FalseLiteral:
	{
		auto e = std::make_shared<awst::BoolConstant>();
		e->sourceLocation = m_loc;
		e->wtype = awst::WType::boolType();
		e->value = false;
		return e;
	}
	case Token::Number:
	{
		auto* mappedType = m_ctx.typeMapper.map(m_solType);
		if (mappedType != awst::WType::uint64Type() && mappedType != awst::WType::biguintType())
			mappedType = awst::WType::biguintType();
		auto e = std::make_shared<awst::IntegerConstant>();
		e->sourceLocation = m_loc;
		e->wtype = mappedType;
		if (auto const* ratType = dynamic_cast<RationalNumberType const*>(m_solType))
		{
			auto val = ratType->literalValue(nullptr);
			if (val < 0 && mappedType == awst::WType::biguintType())
			{
				// Negative constant → two's complement for biguint
				static const solidity::u256 pow256(
					"115792089237316195423570985008687907853269984665640564039457584007913129639936");
				solidity::u256 tcVal = pow256 + solidity::u256(val);
				std::ostringstream oss;
				oss << tcVal;
				e->value = oss.str();
			}
			else
				e->value = val.str();
		}
		else
			e->value = m_literal.value();
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
		auto e = std::make_shared<awst::BytesConstant>();
		e->sourceLocation = m_loc;
		e->wtype = awst::WType::bytesType();
		auto const& raw = m_literal.value();
		e->value.assign(raw.begin(), raw.end());
		e->encoding = awst::BytesEncoding::Base16;
		return e;
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
