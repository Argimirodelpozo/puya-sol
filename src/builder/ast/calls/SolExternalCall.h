#pragma once

#include "builder/ast/SolFunctionCall.h"

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

};

} // namespace puyasol::builder::sol_ast
