#pragma once

#include "builder/sol-ast/SolFunctionCall.h"

namespace puyasol::builder::sol_ast
{

/// abi.encode(), abi.encodePacked(), abi.encodeCall(),
/// abi.encodeWithSelector(), abi.encodeWithSignature().
/// Delegates to AbiEncoderBuilder for the actual encoding logic.
class SolAbiEncode: public SolFunctionCall
{
public:
	using SolFunctionCall::SolFunctionCall;
	std::shared_ptr<awst::Expression> toAwst() override;
};

} // namespace puyasol::builder::sol_ast
