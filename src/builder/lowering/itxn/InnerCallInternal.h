#pragma once

/// @file InnerCallInternal.h
/// Symbols shared between InnerCallHandlers.cpp and InnerCallShapes.cpp.

#include <cstdint>
#include <optional>

namespace solidity::frontend { class Expression; }

namespace puyasol::builder::eb
{

/// Recognize a precompile using conservative, solc-validated constant-address facts.
std::optional<uint64_t> detectPrecompileAddress(
	solidity::frontend::Expression const& _baseExpr);

} // namespace puyasol::builder::eb
