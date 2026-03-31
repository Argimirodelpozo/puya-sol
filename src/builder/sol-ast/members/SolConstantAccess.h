#pragma once

#include "builder/sol-ast/SolMemberAccess.h"

namespace puyasol::builder::sol_ast
{

/// Library/contract constant inlining: Contract.CONSTANT → inline value.
/// Also handles event member access placeholders (L.E → VoidConstant).
/// Also handles contract member name access (token.transfer → BytesConstant).
class SolConstantAccess: public SolMemberAccess
{
public:
	using SolMemberAccess::SolMemberAccess;
	std::shared_ptr<awst::Expression> toAwst() override;
};

} // namespace puyasol::builder::sol_ast
