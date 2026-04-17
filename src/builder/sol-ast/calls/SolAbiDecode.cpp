#include "builder/sol-ast/calls/SolAbiDecode.h"
#include "builder/sol-eb/AbiEncoderBuilder.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolAbiDecode::toAwst()
{
	auto result = eb::AbiEncoderBuilder::tryHandle(m_ctx, "decode", m_call, m_loc);
	if (result)
		return result->resolve();

	// Fallback: return empty bytes
	return awst::makeBytesConstant({}, m_loc);
}

} // namespace puyasol::builder::sol_ast
