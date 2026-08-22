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

/// For a public/external target, compute the ARC4 WType for the AWST parameter,
/// mirroring ContractBuilder's param remap:
///   - biguint → arc4.uintN (preserving bit width)
///   - bytes[N] FunctionType param → arc4.static_array<arc4.uint8, N>
///   - other → nullptr (no wrapping).
awst::WType const* dispatchPublicArgArc4Type(
	TypeMapper& _typeMapper,
	awst::WType const* _nativeType,
	solidity::frontend::Type const* _paramSolType);

// (mapDispatchType and encodeArgForInnerTxn DELETED 2026-07-20: both had
// drifted from their canonical twins — dispatch types now come from
// TypeMapper::map / computeReturnType on BOTH sides, and external fn-ptr
// args go through InnerCallHandlers::encodeArgToBytes. Do not re-add copies.)

} // namespace puyasol::builder::eb
