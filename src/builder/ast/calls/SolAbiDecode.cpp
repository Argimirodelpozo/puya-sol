#include "builder/ast/calls/SolAbiDecode.h"
#include "builder/lowering/abi/Arc4Stdlib.h"
#include "builder/lowering/abi/AbiEncoderBuilder.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolAbiDecode::toAwst()
{
	if (auto arc4Result = eb::Arc4Stdlib::tryHandleDecodeEnvelope(
			m_ctx, m_call, m_loc))
		return *arc4Result;
	return eb::AbiEncoderBuilder::build(m_ctx, m_call, m_loc);
}

} // namespace puyasol::builder::sol_ast
