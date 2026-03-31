/// @file SolMetaType.cpp
/// type(X) expression — produces a metatype placeholder.
/// The actual property access (.max, .min, .name, .interfaceId, .creationCode, etc.)
/// is resolved in MemberAccessBuilder, which inspects the MagicType argument
/// directly from the AST annotation rather than using the base expression value.

#include "builder/sol-ast/calls/SolMetaType.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolMetaType::toAwst()
{
	// type(X) is never used standalone — it's always followed by member access.
	// Return a void placeholder; MemberAccessBuilder handles .max/.min/.name etc.
	auto vc = std::make_shared<awst::VoidConstant>();
	vc->sourceLocation = m_loc;
	vc->wtype = awst::WType::voidType();
	return vc;
}

} // namespace puyasol::builder::sol_ast
