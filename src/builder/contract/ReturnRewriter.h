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
#include "builder/ReturnWirePlan.h"

#include <libsolidity/ast/AST.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace puyasol::builder
{

class TypeMapper;

/// Per-return-element ABI wire plan (biguint element → signed?arc4.uint256 : arc4.uintN;
/// everything else native). THE single source of the wire-type decision — read by the
/// build-time encoder (SolReturnStatement), the chain-dispatch encoder, and the
/// remaining post-passes. See ReturnWirePlan.h.
std::vector<ReturnWireElem> computeReturnPlan(
	solidity::frontend::FunctionDefinition const& _func,
	awst::WType const* _returnType,
	TypeMapper& _typeMapper);

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

/// Apply `_fn` to every ReturnStatement reachable in `_stmts`, recursing into
/// IfElse / Block / WhileLoop bodies.
void forEachReturnStatement(
	std::vector<std::shared_ptr<awst::Statement>>& _stmts,
	std::function<void(awst::ReturnStatement&)> const& _fn);

void rewriteARC4Returns(
	awst::ContractMethod& _method,
	solidity::frontend::FunctionDefinition const& _func,
	TypeMapper& _typeMapper,
	std::vector<SignedReturnInfo> const& _signedReturns,
	std::vector<UnsignedMaskInfo> const& _unsignedMasks);

/// Encode the OUTER dispatch return of a CHAIN-LOWERED (modifier'd) function to its
/// ABI wire type. buildModifierChain threads NATIVE values through its subs and the
/// outer method ends with `return r` / `return (r1..rN)` — without this, a biguint
/// return publishes as puya's bare-biguint "uint512" while cross-contract callers
/// name the declared width ("uint128"/"uint256") → selector mismatch → unconditional
/// revert (oracle-found; the crosscall fuzzer had no modifier'd callees). Wire rule
/// per element: signed → arc4.uint256 (value already sign-extended by pass 4 inside
/// the body sub); unsigned biguint → arc4.uintN(declared); everything else native.
/// Call AFTER buildModifierChain; no-op when nothing needs encoding.
void encodeChainDispatchReturn(
	awst::ContractMethod& method,
	solidity::frontend::FunctionDefinition const& _func,
	TypeMapper& m_typeMapper);

} // namespace puyasol::builder
