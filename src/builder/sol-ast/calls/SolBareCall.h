#pragma once

#include "builder/sol-ast/SolFunctionCall.h"

namespace puyasol::builder::sol_ast
{

/// address.call(data), address.staticcall(data), address.delegatecall(data).
/// Also handles .call{value: X}("") for payment inner transactions.
/// Delegates to InnerCallHandlers.
class SolBareCall: public SolFunctionCall
{
public:
	using SolFunctionCall::SolFunctionCall;
	std::shared_ptr<awst::Expression> toAwst() override;
};

} // namespace puyasol::builder::sol_ast
