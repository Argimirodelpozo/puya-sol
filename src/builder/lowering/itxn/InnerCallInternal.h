#pragma once

/// @file InnerCallInternal.h
/// Symbols shared between InnerCallHandlers.cpp and InnerCallShapes.cpp.

#include "builder/eb/NodeBuilder.h"

#include <cstdint>
#include <optional>

namespace solidity::frontend { class Expression; }

namespace puyasol::builder::eb
{

/// Recognize a precompile using conservative, solc-validated constant-address facts.
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

} // namespace puyasol::builder::eb
