#include "builder/sol-ast/calls/SolAbiEncode.h"
#include "builder/sol-eb/AbiEncoderBuilder.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolAbiEncode::toAwst()
{
	using Kind = solidity::frontend::FunctionType::Kind;
	auto const* funcType = dynamic_cast<solidity::frontend::FunctionType const*>(
		m_call.expression().annotation().type);

	std::string memberName;
	switch (funcType->kind())
	{
	case Kind::ABIEncode:            memberName = "encode"; break;
	case Kind::ABIEncodePacked:      memberName = "encodePacked"; break;
	case Kind::ABIEncodeCall:        memberName = "encodeCall"; break;
	case Kind::ABIEncodeWithSelector: memberName = "encodeWithSelector"; break;
	case Kind::ABIEncodeWithSignature: memberName = "encodeWithSignature"; break;
	default:                         memberName = "encode"; break;
	}

	auto result = eb::AbiEncoderBuilder::tryHandle(m_ctx, memberName, m_call, m_loc);
	if (result)
		return result->resolve();

	// Fallback: return empty bytes
	auto empty = std::make_shared<awst::BytesConstant>();
	empty->sourceLocation = m_loc;
	empty->wtype = awst::WType::bytesType();
	empty->encoding = awst::BytesEncoding::Base16;
	return empty;
}

} // namespace puyasol::builder::sol_ast
