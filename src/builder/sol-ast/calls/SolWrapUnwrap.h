#pragma once

#include "builder/sol-ast/SolFunctionCall.h"

namespace puyasol::builder::sol_ast
{

/// User-defined value type wrap/unwrap: Fr.wrap(x), Fr.unwrap(y).
/// These are no-ops since UDVT and underlying type map to the same WType.
class SolWrapUnwrap: public SolFunctionCall
{
public:
	using SolFunctionCall::SolFunctionCall;
	std::shared_ptr<awst::Expression> toAwst() override;
};

} // namespace puyasol::builder::sol_ast
