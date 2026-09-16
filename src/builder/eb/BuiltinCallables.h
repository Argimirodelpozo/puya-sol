#pragma once

#include "builder/context/ContractContext.h"
#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

/// Builtin value lowering after CallOperands has sequenced the arguments.
/// Dispatch uses solc's resolved kind, never the spelling of the callee.
std::shared_ptr<awst::Expression> buildBuiltinCall(
	ContractContext& ctx,
	solidity::frontend::FunctionType::Kind kind,
	std::vector<std::shared_ptr<awst::Expression>> args,
	awst::SourceLocation const& loc);

} // namespace puyasol::builder::eb
