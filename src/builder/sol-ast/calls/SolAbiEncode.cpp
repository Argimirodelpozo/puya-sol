#include "builder/sol-ast/calls/SolAbiEncode.h"
#include "builder/abi/AbiEncoderBuilder.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolAbiEncode::toAwst()
{
	using Kind = solidity::frontend::FunctionType::Kind;
	if (auto const* access = dynamic_cast<solidity::frontend::MemberAccess const*>(
			&m_call.expression()))
		if (auto const* magic = dynamic_cast<solidity::frontend::MagicType const*>(
				access->expression().annotation().type);
			magic && magic->kind() == solidity::frontend::MagicType::Kind::ARC4)
		{
			std::vector<std::shared_ptr<awst::Expression>> values;
			for (auto const& argument: m_call.arguments())
				values.push_back(m_ctx.buildExpr(*argument));
			return eb::AbiEncoderBuilder::arc4EncodeValues(
				m_ctx, std::move(values), m_loc);
		}
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
	return awst::makeBytesConstant({}, m_loc);
}

} // namespace puyasol::builder::sol_ast
