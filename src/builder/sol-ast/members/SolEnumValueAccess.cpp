#include "builder/sol-ast/members/SolEnumValueAccess.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolEnumValueAccess::toAwst()
{
	auto const* enumVal = dynamic_cast<solidity::frontend::EnumValue const*>(
		m_memberAccess.annotation().referencedDeclaration);
	if (!enumVal) return nullptr;

	auto const* enumDef = dynamic_cast<solidity::frontend::EnumDefinition const*>(
		enumVal->scope());
	if (!enumDef) return nullptr;

	int index = 0;
	for (auto const& member: enumDef->members())
	{
		if (member.get() == enumVal)
			break;
		++index;
	}

	auto e = awst::makeIntegerConstant(std::to_string(index), m_loc);
	return e;
}

} // namespace puyasol::builder::sol_ast
