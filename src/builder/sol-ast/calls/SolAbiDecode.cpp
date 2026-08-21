#include "builder/sol-ast/calls/SolAbiDecode.h"
#include "builder/abi/AbiEncoderBuilder.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolAbiDecode::toAwst()
{
	if (auto const* access = dynamic_cast<solidity::frontend::MemberAccess const*>(
			&m_call.expression()))
		if (auto const* magic = dynamic_cast<solidity::frontend::MagicType const*>(
				access->expression().annotation().type);
			magic && magic->kind() == solidity::frontend::MagicType::Kind::ARC4)
			return eb::AbiEncoderBuilder::decodeArc4(m_ctx, m_call, m_loc);
	auto result = eb::AbiEncoderBuilder::tryHandle(m_ctx, "decode", m_call, m_loc);
	if (result)
		return result->resolve();

	// Fallback: return empty bytes
	return awst::makeBytesConstant({}, m_loc);
}

} // namespace puyasol::builder::sol_ast
