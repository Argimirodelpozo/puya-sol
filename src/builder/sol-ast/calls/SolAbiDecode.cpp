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
	auto empty = std::make_shared<awst::BytesConstant>();
	empty->sourceLocation = m_loc;
	empty->wtype = awst::WType::bytesType();
	empty->encoding = awst::BytesEncoding::Base16;
	return empty;
}

} // namespace puyasol::builder::sol_ast
