#include "builder/sol-ast/calls/SolAbiDecode.h"
#include "builder/abi/Arc4Stdlib.h"
#include "builder/abi/AbiEncoderBuilder.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolAbiDecode::toAwst()
{
	if (auto arc4Result = eb::Arc4Stdlib::tryHandleDecodeEnvelope(
			m_ctx, m_call, m_loc))
		return *arc4Result;
	auto result = eb::AbiEncoderBuilder::tryHandle(m_ctx, "decode", m_call, m_loc);
	if (result)
		return result->resolve();

	// Fallback: return empty bytes
	return awst::makeBytesConstant({}, m_loc);
}

} // namespace puyasol::builder::sol_ast
