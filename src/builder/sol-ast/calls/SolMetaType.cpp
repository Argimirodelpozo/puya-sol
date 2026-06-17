/// @file SolMetaType.cpp
/// type(X) — metatype placeholder; .max/.min/.name/.interfaceId etc.
/// resolved by MemberAccessBuilder from the AST annotation.

#include "builder/sol-ast/calls/SolMetaType.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolMetaType::toAwst()
{
	// Always followed by member access; void placeholder for MemberAccessBuilder.
	auto vc = awst::makeVoidConstant(m_loc);
	return vc;
}

} // namespace puyasol::builder::sol_ast
