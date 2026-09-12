#include "builder/sol-ast/calls/SolAbiEncode.h"
#include "builder/abi/AbiEncoderBuilder.h"

namespace puyasol::builder::sol_ast
{
std::shared_ptr<awst::Expression> SolAbiEncode::toAwst()
{
	return eb::AbiEncoderBuilder::build(m_ctx, m_call, m_loc);
}
} // namespace puyasol::builder::sol_ast
