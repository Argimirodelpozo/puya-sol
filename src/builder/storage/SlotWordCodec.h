#pragma once

/// @file SlotWordCodec.h
/// Shared codec between a value's NATIVE repr and its EVM packed-slot bytes.
/// Used by the asm storage dispatcher (per-var typed cells) and slot-handle
/// element access (packed array elements / struct fields through repointed
/// storage pointers). One codec, exact inverse transforms — the packed model's
/// correctness rests on never mixing reprs.

#include "awst/Node.h"

#include "builder/sol-types/SolcFwd.h"

namespace puyasol::builder
{

struct SlotWordCodec
{
	/// value (of `wtype`) → its `size` big-endian packed bytes (the exact bytes
	/// the EVM would keep in the slot at the value's position).
	/// Handled: uint64/bool (incl. >8-byte numeric e.g. contract-as-20-bytes),
	/// biguint (canonical TC), account (trailing-20), bytes[N] (raw),
	/// ARC4UIntN (backing bytes), ARC4Bool (canonical 0/1 byte).
	/// Unknown types: loud error + zero bytes.
	static std::shared_ptr<awst::Expression> nativeToPackedBytes(
		std::shared_ptr<awst::Expression> _value,
		awst::WType const* _wtype,
		unsigned _size,
		awst::SourceLocation const& _loc);

	/// BYTE-shaped native (bytes[N] / arc4 byte[K]): its packed form is
	/// LEFT-aligned in its window, so callers packing a whole-word canonical
	/// must pack to the element's WINDOW size, not 32 — a 32-wide pack parks
	/// the bytes at the word's high end where no leaf window reads them.
	static bool isByteShaped(awst::WType const* _wtype);

	/// `size` packed bytes → value of `wtype`. `solType` supplies signedness:
	/// sub-64 signed → 64-bit-TC uint64 cells; 64<bits<256 signed → canonical
	/// 256-bit-TC biguint. Unknown types: loud error + nullptr.
	static std::shared_ptr<awst::Expression> packedBytesToNative(
		std::shared_ptr<awst::Expression> _raw,
		awst::WType const* _wtype,
		solidity::frontend::Type const* _solType,
		unsigned _size,
		awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder
