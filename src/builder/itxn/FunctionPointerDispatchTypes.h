#pragma once

/// @file FunctionPointerDispatchTypes.h
/// Pure type-mapping helpers for FunctionPointerBuilder (no static state).

#include "awst/Node.h"
#include "builder/sol-eb/ContractContext.h"

#include "builder/sol-types/SolcFwd.h"

#include <memory>

namespace puyasol::builder::eb
{

/// Map return parameters to dispatch WType: void / single / WTuple.
awst::WType const* computeReturnType(ContractContext& _ctx, solidity::frontend::FunctionType const* _funcType);



// (mapDispatchType and encodeArgForInnerTxn DELETED 2026-07-20: both had
// drifted from their canonical twins — dispatch types now come from
// TypeMapper::map / computeReturnType on BOTH sides, and external fn-ptr
// args go through InnerCallHandlers::encodeArgToBytes. Do not re-add copies.)

} // namespace puyasol::builder::eb
