#pragma once

#include "builder/sol-ast/SolFunctionCall.h"

namespace puyasol::builder::sol_ast
{

/// type(X) expressions — produces a metatype value.
/// Actual property access (.max, .min, .name, .interfaceId) is handled
/// by MemberAccessBuilder; this just provides the expression placeholder.
class SolMetaType: public SolFunctionCall
{
public:
	using SolFunctionCall::SolFunctionCall;
	std::shared_ptr<awst::Expression> toAwst() override;
};

} // namespace puyasol::builder::sol_ast
