#pragma once

#include "builder/ast/SolMemberAccess.h"

namespace puyasol::builder::sol_ast
{

/// f.selector, Error.selector, and Event.selector. Compatibility mode exposes
/// ARC-4/ARC-28 identities; --evm-selectors exposes Solidity keccak identities.
class SolSelectorAccess: public SolMemberAccess
{
public:
	using SolMemberAccess::SolMemberAccess;
	std::shared_ptr<awst::Expression> toAwst() override;

	/// Project a selector while preserving receiver/options/branch effects.
	/// abi.encodeCall always requests the canonical Solidity selector.
	static std::shared_ptr<awst::Expression> selectorOf(
		eb::ContractContext&, solidity::frontend::Expression const&, awst::WType const*,
		awst::SourceLocation const&, bool canonical = false);
};

} // namespace puyasol::builder::sol_ast
