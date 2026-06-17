#pragma once

/// @file InnerCallInternal.h
/// Symbols shared between InnerCallHandlers.cpp and InnerCallShapes.cpp.

#include "builder/sol-eb/NodeBuilder.h"

#include <cstdint>
#include <optional>

namespace solidity::frontend { class Expression; }

namespace puyasol::builder::eb
{

/// Detect a known precompile address from a `.call`/`.staticcall` member-access
/// base. Returns the precompile number (0x01..0x0a) when the base is an
/// `address(N)` type-conversion of an in-range integer literal; nullopt otherwise.
std::optional<uint64_t> detectPrecompileAddress(
	solidity::frontend::Expression const& _baseExpr);

/// InstanceBuilder wrapping a pre-built AWST expression (e.g. (bool, bytes) tuple).
class GenericResultBuilder: public InstanceBuilder
{
public:
	GenericResultBuilder(ContractContext& _ctx, std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr)) {}
	solidity::frontend::Type const* solType() const override { return nullptr; }
};

/// AVM inner transaction type enum values.
inline constexpr int TxnTypePay = 1;
inline constexpr int TxnTypeAppl = 6;

} // namespace puyasol::builder::eb
