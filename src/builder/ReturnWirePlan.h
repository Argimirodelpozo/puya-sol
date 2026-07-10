#pragma once

/// @file ReturnWirePlan.h
/// The per-return-element ABI wire plan — shared between the function builder
/// (which computes it and stashes it in FunctionContext) and SolReturnStatement
/// (which applies it when building each `return`). Neutral location under
/// builder/ so both sol-ast/ and contract/ can include it without a cycle.
///
/// Design note (fable-review-2 D2, build-time-return-encoding): the ABI wire
/// encoding of a return value belongs at the point the ReturnStatement is built
/// (we hold the value + know the declared return types from solc), NOT in a
/// post-hoc walk over already-built AWST. This struct carries the decision so
/// the builder can act on it directly.

#include "awst/Node.h"

namespace puyasol::builder
{

struct ReturnWireElem
{
	awst::WType const* nativeType = nullptr;   // element type in method.returnType (post promotion)
	awst::WType const* wireType = nullptr;     // ABI wire type
	bool isSigned = false;                     // sign-extend to 256 bits before encode
	unsigned bits = 0;                         // declared width (wire width + asm mod-wrap + mask)
	bool encoded = false;                      // biguint/array element → needs ARC4Encode
	bool masked = false;                       // unsigned sub-word (uint64 native) → mask to `bits`
};

} // namespace puyasol::builder
