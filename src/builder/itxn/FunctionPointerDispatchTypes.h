#pragma once

/// @file FunctionPointerDispatchTypes.h
/// Pure helpers used by FunctionPointerBuilder to map Solidity types to
/// dispatch-call WTypes and to wrap argument expressions for the
/// inner-application-call calldata layout. Extracted from the anonymous
/// namespace in FunctionPointerBuilder.cpp; they touch no static state.

#include "awst/Node.h"
#include "builder/sol-eb/ContractContext.h"

#include <libsolidity/ast/Types.h>

#include <memory>

namespace puyasol::builder::eb
{

/// Map a function type's return parameters to the dispatch return WType.
/// void for no returns, single WType for one return, WTuple for multiple.
awst::WType const* computeReturnType(ContractContext& _ctx, solidity::frontend::FunctionType const* _funcType);

/// For a public/external target function, compute the ARC4 WType that the
/// target's AWST parameter will have. Mirrors ContractBuilder's param remap:
///   - biguint (uint128..uint256, etc.): arc4.uintN preserving bit width.
///   - bytes[12] from a FunctionType param: arc4.static_array<arc4.uint8, 12>.
///   - other: nullptr (no wrapping needed).
awst::WType const* dispatchPublicArgArc4Type(
	awst::WType const* _nativeType, solidity::frontend::Type const* _paramSolType);

/// Map a Solidity type to the dispatch-method WType.
/// _promoteSignedI64Biguint=true treats int8..int64 (signed) as biguint so
/// that sign-extension works at the ABI boundary — used for dispatch return
/// types; for arg types it stays uint64.
awst::WType const* mapDispatchType(
	solidity::frontend::Type const* _solType, bool _promoteSignedI64Biguint);

/// Encode one argument as ARC4-raw bytes for an inner-application-call
/// `ApplicationArgs[i]` field. Follows the ARC4 ABI encoding rules:
///   - uintN (N ≤ 64, native uint64): itob, then left-pad to N/8 bytes.
///   - biguint: reinterpret; if IntegerType, left-pad to N/8 bytes.
///   - bool: 1 byte, 0x80 for true else 0x00.
///   - bytes / string (dynamic): uint16(length) ++ raw bytes.
///   - other: reinterpret as bytes.
std::shared_ptr<awst::Expression> encodeArgForInnerTxn(
	std::shared_ptr<awst::Expression> _argExpr,
	solidity::frontend::Type const* _paramSolType,
	awst::SourceLocation const& _loc);

} // namespace puyasol::builder::eb
