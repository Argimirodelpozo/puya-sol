#pragma once

/// @file Ripemd160Builder.h
/// AWST subroutine builder for the Solidity `ripemd160(bytes)` builtin.
///
/// AVM has no RIPEMD-160 opcode, so we synthesize one as a Subroutine root
/// node that can be called from any contract / library / free function via
/// SubroutineID. Reference C implementation: tiny-ripemd160 by Dave Turner
/// (MIT) — https://github.com/DaveCTurner/tiny-ripemd160. Algorithm follows
/// the original RIPEMD-160 spec (Bosselaers/Preneel, 1996).
///
/// 32-bit words live in uint64 vars with explicit `& 0xFFFFFFFF` masking
/// after every arithmetic step that can overflow into bits 32+. AVM has no
/// native uint32, no bswap, and no ROL — all simulated.

#include "awst/Node.h"

#include <memory>
#include <string>

namespace puyasol::builder::builtin
{

/// Stable id used in SubroutineCallExpression.target. Same id every call so
/// puya emits a single shared body; reachability DCE drops it when no
/// caller references it.
std::string const& ripemd160SubroutineId();

/// Stable display name (used as `Subroutine.name`).
std::string const& ripemd160SubroutineName();

/// Build the AWST Subroutine that computes RIPEMD-160 of an arbitrary-length
/// `bytes` input and returns a 20-byte digest. Takes one source location
/// (used for every node inside the body — there's no real source for these).
std::shared_ptr<awst::Subroutine> buildRipemd160Subroutine(
	awst::SourceLocation loc);

} // namespace puyasol::builder::builtin
