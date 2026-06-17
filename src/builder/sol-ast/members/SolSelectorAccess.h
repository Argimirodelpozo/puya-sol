#pragma once

#include "builder/sol-ast/SolMemberAccess.h"

namespace puyasol::builder::sol_ast
{

/// f.selector, E.selector → keccak256("Name(type1,...)")[:4].
/// Handles function selectors, event selectors, and ternary distribution.
class SolSelectorAccess: public SolMemberAccess
{
public:
	using SolMemberAccess::SolMemberAccess;
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	/// ARC-4 sha512_256 selector (TEAL `method "sig"`) as bytes4.
	std::shared_ptr<awst::Expression> makeSelectorExpr(std::string const& _sig);

	/// Resolve canonical sig from a sub-expression (for ternary distribution).
	std::string resolveSignature(solidity::frontend::Expression const& _expr);
	std::string canonicalSelectorSig(solidity::frontend::FunctionType const& _ft);
};

} // namespace puyasol::builder::sol_ast
