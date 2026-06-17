#pragma once

/// @file InnerCallInternal.h
/// Symbols shared between InnerCallHandlers.cpp and InnerCallShapes.cpp.

#include "builder/sol-eb/NodeBuilder.h"

namespace puyasol::builder::eb
{

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
