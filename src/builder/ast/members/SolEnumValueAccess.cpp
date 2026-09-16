#include "builder/ast/members/SolEnumValueAccess.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolEnumValueAccess::toAwst()
{
	auto const* type = dynamic_cast<solidity::frontend::EnumType const*>(m_solType);
	if (!type) return nullptr;
	return awst::makeIntegerConstant(type->memberValue(memberName()), m_loc, awst::WType::biguintType());
}

} // namespace puyasol::builder::sol_ast
