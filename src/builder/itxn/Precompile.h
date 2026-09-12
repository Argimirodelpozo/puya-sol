#pragma once

#include "awst/Node.h"

namespace puyasol::builder
{
class TypeMapper;

/// Shared EVM-precompile byte semantics. Adapters own operand evaluation,
/// memory copies, and publishing the resulting external return-data buffer.
/// Unsupported operations are compile errors, never application calls.
std::shared_ptr<awst::Expression> evaluatePrecompile(
	TypeMapper& types, uint64_t address, std::shared_ptr<awst::Expression> input,
	awst::SourceLocation const& loc, std::vector<std::shared_ptr<awst::Statement>>& out);

} // namespace puyasol::builder
