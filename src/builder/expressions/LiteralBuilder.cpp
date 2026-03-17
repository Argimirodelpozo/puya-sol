/// @file LiteralBuilder.cpp
/// Handles Solidity literal expressions (numbers, booleans, strings, hex).

#include "builder/expressions/ExpressionBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/TypeProvider.h>

#include <algorithm>
#include <sstream>

namespace puyasol::builder
{

bool ExpressionBuilder::visit(solidity::frontend::Literal const& _node)
{
	using namespace solidity::frontend;
	auto loc = makeLoc(_node.location());
	auto const* solType = _node.annotation().type;

	switch (_node.token())
	{
	case Token::TrueLiteral:
	{
		auto e = std::make_shared<awst::BoolConstant>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::boolType();
		e->value = true;
		push(e);
		break;
	}
	case Token::FalseLiteral:
	{
		auto e = std::make_shared<awst::BoolConstant>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::boolType();
		e->value = false;
		push(e);
		break;
	}
	case Token::Number:
	{
		auto* mappedType = m_typeMapper.map(solType);
		// IntegerConstant must have uint64 or biguint type
		if (mappedType != awst::WType::uint64Type() && mappedType != awst::WType::biguintType())
			mappedType = awst::WType::biguintType();
		auto e = std::make_shared<awst::IntegerConstant>();
		e->sourceLocation = loc;
		e->wtype = mappedType;
		// Use RationalNumberType::literalValue() to get the actual computed value,
		// which includes sub-denomination multipliers (e.g. 365 days → 31536000)
		if (auto const* ratType = dynamic_cast<solidity::frontend::RationalNumberType const*>(solType))
			e->value = ratType->literalValue(nullptr).str();
		else
			e->value = _node.value();
		push(e);
		break;
	}
	case Token::StringLiteral:
	{
		auto e = std::make_shared<awst::StringConstant>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::stringType();
		e->value = _node.value();
		push(e);
		break;
	}
	case Token::HexStringLiteral:
	{
		// hex"..." literals contain raw binary data — use BytesConstant
		// so the serializer can base85-encode them (raw bytes break JSON/UTF-8)
		auto e = std::make_shared<awst::BytesConstant>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::bytesType();
		auto const& raw = _node.value();
		e->value.assign(raw.begin(), raw.end());
		e->encoding = awst::BytesEncoding::Base16;
		push(e);
		break;
	}
	default:
	{
		auto e = std::make_shared<awst::StringConstant>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::stringType();
		e->value = _node.value();
		push(e);
		break;
	}
	}
	return false;
}

} // namespace puyasol::builder
