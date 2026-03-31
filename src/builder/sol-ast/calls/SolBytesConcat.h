#pragma once

#include "builder/sol-ast/SolFunctionCall.h"

namespace puyasol::builder::sol_ast
{

/// bytes.concat(a, b, ...) and string.concat(a, b, ...).
/// Chains concat intrinsics over the arguments.
class SolBytesConcat: public SolFunctionCall
{
public:
	using SolFunctionCall::SolFunctionCall;
	std::shared_ptr<awst::Expression> toAwst() override;
};

} // namespace puyasol::builder::sol_ast
