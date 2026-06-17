#pragma once

/// @file FunctionPointerDispatchTypes.h
/// Pure type-mapping helpers for FunctionPointerBuilder (no static state).

#include "awst/Node.h"
#include "builder/sol-eb/ContractContext.h"

#include <libsolidity/ast/Types.h>

#include <memory>

namespace puyasol::builder::eb
{

/// Map return parameters to dispatch WType: void / single / WTuple.
awst::WType const* computeReturnType(ContractContext& _ctx, solidity::frontend::FunctionType const* _funcType);

/// For a public/external target, compute the ARC4 WType for the AWST parameter,
/// mirroring ContractBuilder's param remap:
///   - biguint → arc4.uintN (preserving bit width)
///   - bytes[12] FunctionType param → arc4.static_array<arc4.uint8, 12>
///   - other → nullptr (no wrapping).
awst::WType const* dispatchPublicArgArc4Type(
	awst::WType const* _nativeType, solidity::frontend::Type const* _paramSolType);

/// Map a Solidity type to the dispatch WType.
/// _promoteSignedI64Biguint: treat int8..int64 as biguint (for sign-extension
/// at the ABI boundary); used for return types, not arg types.
awst::WType const* mapDispatchType(
	solidity::frontend::Type const* _solType, bool _promoteSignedI64Biguint);

/// Encode an argument to ARC4-raw bytes for an inner-txn ApplicationArgs field:
///   - uintN (≤64): itob, left-pad to N/8 bytes.
///   - biguint: reinterpret; if IntegerType, left-pad to N/8 bytes.
///   - bool: 1 byte, 0x80=true / 0x00=false.
///   - bytes/string: uint16(length) ++ raw bytes.
///   - other: reinterpret as bytes.
std::shared_ptr<awst::Expression> encodeArgForInnerTxn(
	std::shared_ptr<awst::Expression> _argExpr,
	solidity::frontend::Type const* _paramSolType,
	awst::SourceLocation const& _loc);

} // namespace puyasol::builder::eb
