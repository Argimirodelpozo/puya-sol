#pragma once

#include "awst/Node.h"

#include <functional>

namespace puyasol::builder
{
class TypeMapper;

/// Shared ecrecover guard: r/s must be in [1, N-1] before ecdsa_pk_recover,
/// which would otherwise panic instead of returning empty/zero output.
/// Each reader must return a fresh 32-byte expression (read twice); callers own pinning.
std::shared_ptr<awst::Expression> secp256k1RangeCondition(
	std::function<std::shared_ptr<awst::Expression>()> const& readR,
	std::function<std::shared_ptr<awst::Expression>()> const& readS,
	awst::SourceLocation const& loc);

/// Shared EVM-precompile byte semantics. Adapters own operand evaluation,
/// memory copies, and publishing the resulting external return-data buffer.
/// Unsupported operations are compile errors, never application calls.
std::shared_ptr<awst::Expression> evaluatePrecompile(
	TypeMapper& types, uint64_t address, std::shared_ptr<awst::Expression> input,
	awst::SourceLocation const& loc, std::vector<std::shared_ptr<awst::Statement>>& out);

} // namespace puyasol::builder
