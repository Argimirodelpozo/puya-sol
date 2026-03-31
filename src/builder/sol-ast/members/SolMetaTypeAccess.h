#pragma once

#include "builder/sol-ast/SolMemberAccess.h"

namespace puyasol::builder::sol_ast
{

/// type(X).max, type(X).min, type(C).name, type(I).interfaceId.
class SolMetaTypeAccess: public SolMemberAccess
{
public:
	using SolMemberAccess::SolMemberAccess;
	std::shared_ptr<awst::Expression> toAwst() override;
};

} // namespace puyasol::builder::sol_ast
