/// @file SolLiteral.cpp — literal expression translation.

#include "builder/sol-ast/exprs/SolLiteral.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

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
		// A number literal's type is RationalNumberType (never a concrete intN here:
		// m_solType = annotation().type), so no signed sub-word wrap is reachable —
		// negative literals are UnaryOperation, type(T).min is SolMetaTypeAccess, and
		// explicit casts are SolTypeConversion. The shared helper just promotes to
		// biguint when the magnitude overflows uint64.
		if (auto const* ratType = dynamic_cast<RationalNumberType const*>(m_solType))
			return builder::TypeCoercion::rationalIntConstant(
				ratType->literalValue(nullptr), mappedType, m_loc);
		// Non-rational fallthrough (defensive): emit the raw literal text.
		auto e = std::make_shared<awst::IntegerConstant>();
		e->sourceLocation = m_loc;
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
