#pragma once

#include "builder/ast/SolFunctionCall.h"

namespace puyasol::builder::sol_ast
{

/// Struct constructor call: MyStruct({field1: val1, field2: val2})
/// or positional: MyStruct(val1, val2).
///
/// Produces an ARC4 value or a scratch pointer using solc's member layout.
class SolStructConstruction: public SolFunctionCall
{
public:
	using SolFunctionCall::SolFunctionCall;
	std::shared_ptr<awst::Expression> toAwst() override;
};

} // namespace puyasol::builder::sol_ast
