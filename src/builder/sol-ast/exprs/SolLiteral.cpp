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
		if (auto const* ratType = dynamic_cast<RationalNumberType const*>(m_solType))
		{
			auto val = ratType->literalValue(nullptr);
			// literalValue() returns u256 (two's complement for negatives).
			// Truncate to uint64 range or use biguint as needed.
			static const solidity::u256 uint64Max("18446744073709551615");
			static const solidity::u256 pow64("18446744073709551616");   // 2^64
			if (mappedType == awst::WType::uint64Type() && val > uint64Max)
				val = val % pow64;  // truncate to 64-bit two's complement
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
