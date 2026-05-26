#pragma once

/// @file ReturnRewriter.h
/// Post-translation passes that rewrite a function's return values to
/// match its declared ABI return type. Runs after the body has been
/// translated to AWST but before puya consumes the contract method.
///
/// Five orthogonal passes (each gated on a different condition):
///   1. ReferenceArray returns wrap in ARC4Encode for ARC4 methods.
///   2. biguint single-return wraps in ARC4UIntN with the declared bit width
///      (so the ABI sees uint256, not puya's default uint512).
///   3. biguint tuple-element wrap — same idea but per element, with a
///      retTmp spill so non-literal tuple returns can be re-encoded.
///   4. Signed return values are sign-extended to 256 bits, then (for the
///      single-return ABI case) wrapped in ARC4UIntN(256).
///   5. Unsigned sub-word returns are masked to their declared bit width
///      (EVM cleans on ABI encode; AVM preserves full uint64).
///
/// `signedReturns` / `unsignedMasks` are populated by the caller while
/// it derives `method.returnType` from `_func.returnParameters()` — each
/// entry records the original Solidity bit width and the tuple index
/// (or 0 for single-return functions).

#include "awst/Node.h"

#include <libsolidity/ast/AST.h>

#include <cstddef>
#include <vector>

namespace puyasol::builder
{

class TypeMapper;

struct SignedReturnInfo
{
	unsigned bits;
	std::size_t index;
};

struct UnsignedMaskInfo
{
	unsigned bits;
	std::size_t index;
};

void rewriteARC4Returns(
	awst::ContractMethod& _method,
	solidity::frontend::FunctionDefinition const& _func,
	TypeMapper& _typeMapper,
	std::vector<SignedReturnInfo> const& _signedReturns,
	std::vector<UnsignedMaskInfo> const& _unsignedMasks,
	bool _funcHasInlineAssembly);

} // namespace puyasol::builder
