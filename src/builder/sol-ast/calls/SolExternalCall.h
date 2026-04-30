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

	/// Encode a single argument to bytes for ApplicationArgs.
	std::shared_ptr<awst::Expression> encodeArgToBytes(
		std::shared_ptr<awst::Expression> _argExpr,
		solidity::frontend::Type const* _paramSolType);

	/// Convert an address/account expression to an application ID.
	std::shared_ptr<awst::Expression> addressToAppId(
		std::shared_ptr<awst::Expression> _addrExpr);

	/// Build and submit the inner app transaction, return the result.
	std::shared_ptr<awst::Expression> submitAndReturn(
		std::shared_ptr<awst::Expression> _create,
		awst::WType const* _returnType);
};

} // namespace puyasol::builder::sol_ast
