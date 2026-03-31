#pragma once

#include "builder/sol-ast/SolMemberAccess.h"

namespace puyasol::builder::sol_ast
{

/// Struct field access — ARC4Struct (with decode) and WTuple.
class SolFieldAccess: public SolMemberAccess
{
public:
	using SolMemberAccess::SolMemberAccess;
	std::shared_ptr<awst::Expression> toAwst() override;
};

} // namespace puyasol::builder::sol_ast
