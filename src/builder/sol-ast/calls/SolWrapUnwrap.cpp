#include "builder/sol-ast/calls/SolWrapUnwrap.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolWrapUnwrap::toAwst()
{
	if (m_call.arguments().empty())
	{
		auto vc = std::make_shared<awst::VoidConstant>();
		vc->sourceLocation = m_loc;
		vc->wtype = awst::WType::voidType();
		return vc;
	}
	auto val = buildExpr(*m_call.arguments()[0]);
	auto* targetType = m_ctx.typeMapper.map(m_call.annotation().type);
	return TypeCoercion::implicitNumericCast(std::move(val), targetType, m_loc);
}

} // namespace puyasol::builder::sol_ast
