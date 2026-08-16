#pragma once

#include "builder/sol-ast/SolMemberAccess.h"

namespace puyasol::builder::sol_ast
{

/// f.selector, Error.selector, and Event.selector. Compatibility mode exposes
/// ARC-4/ARC-28 identities; --evm-selectors exposes Solidity keccak identities.
class SolSelectorAccess: public SolMemberAccess
{
public:
	using SolMemberAccess::SolMemberAccess;
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	/// Policy-selected selector as bytes4.
	std::shared_ptr<awst::Expression> makeSelectorExpr(std::string const& _sig);

	/// Resolve canonical sig from a sub-expression (for ternary distribution).
	std::string resolveSignature(solidity::frontend::Expression const& _expr);
	std::string canonicalSelectorSig(solidity::frontend::FunctionType const& _ft);
};

} // namespace puyasol::builder::sol_ast
