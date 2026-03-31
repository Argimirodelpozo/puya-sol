#pragma once

#include "builder/sol-ast/SolFunctionCall.h"

namespace puyasol::builder::sol_ast
{

/// require(condition) / require(condition, message) / assert(condition)
///
/// Translates to AssertExpression in AWST:
///   assert(condition, "message")
///
/// Custom errors (require(cond, Errors.Foo())) extract the error name as message.
class SolRequireAssert: public SolFunctionCall
{
public:
	SolRequireAssert(
		eb::BuilderContext& _ctx,
		solidity::frontend::FunctionCall const& _call);

	std::shared_ptr<awst::Expression> toAwst() override;
};

} // namespace puyasol::builder::sol_ast
