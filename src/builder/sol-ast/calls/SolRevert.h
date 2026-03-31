#pragma once

#include "builder/sol-ast/SolFunctionCall.h"

namespace puyasol::builder::sol_ast
{

/// revert() / revert("message") / revert CustomError()
///
/// Translates to assert(false, "message") in AWST.
class SolRevert: public SolFunctionCall
{
public:
	SolRevert(
		eb::BuilderContext& _ctx,
		solidity::frontend::FunctionCall const& _call);

	std::shared_ptr<awst::Expression> toAwst() override;
};

} // namespace puyasol::builder::sol_ast
