#pragma once

/// @file InnerCallInternal.h
/// Internal symbols shared between InnerCallHandlers.cpp and
/// InnerCallShapes.cpp. Header-only / inline-linkage so each TU sees the same
/// definitions.

#include "builder/sol-eb/NodeBuilder.h"

namespace puyasol::builder::eb
{

/// Generic InstanceBuilder result for inner-call handlers that produce a
/// pre-built AWST expression (typically a (bool, bytes) tuple).
class GenericResultBuilder: public InstanceBuilder
{
public:
	GenericResultBuilder(BuilderContext& _ctx, std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr)) {}
	solidity::frontend::Type const* solType() const override { return nullptr; }
};

/// AVM inner transaction type enum values.
inline constexpr int TxnTypePay = 1;
inline constexpr int TxnTypeAppl = 6;

} // namespace puyasol::builder::eb
