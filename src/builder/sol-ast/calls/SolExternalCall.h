#pragma once

#include "builder/sol-ast/SolFunctionCall.h"

namespace puyasol::builder::sol_ast
{

/// External interface/contract calls via inner app transactions.
/// Builds method selector, encodes arguments, submits inner appl transaction.
class SolExternalCall: public SolFunctionCall
{
public:
	using SolFunctionCall::SolFunctionCall;
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	/// Build the ARC4 method selector string from the callee.
	std::string buildMethodSelector(
		solidity::frontend::MemberAccess const& _memberAccess);

	/// Convert an address/account expression to an application ID.
	std::shared_ptr<awst::Expression> addressToAppId(
		std::shared_ptr<awst::Expression> _addrExpr);

	/// Build and submit the inner app transaction, return the result.
	/// _solReturnType is the Solidity result type (single or tuple) — needed to decode signed narrow
	/// ints, which the callee encodes as a 32-byte uint256 (not the 8-byte uint64 their WType implies).
	std::shared_ptr<awst::Expression> submitAndReturn(
		std::shared_ptr<awst::Expression> _create,
		awst::WType const* _returnType,
		solidity::frontend::Type const* _solReturnType = nullptr);
};

} // namespace puyasol::builder::sol_ast
