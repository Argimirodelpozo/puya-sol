#pragma once

/// @file ReturnRewriter.h
/// Five post-translation passes to match declared ABI return types:
///   1. ReferenceArray → ARC4Encode.
///   2. biguint single → ARC4UIntN(N) (ABI "uintN" not "uint512").
///   3. biguint tuple elements → per-element ARC4Encode with retTmp spill.
///   4. Signed → sign-extend to 256 bits + ARC4UIntN(256).
///   5. Unsigned sub-word → mask to declared width (AVM preserves uint64).
/// signedReturns/unsignedMasks carry bit-width + tuple index.

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
