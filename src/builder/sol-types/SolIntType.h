#pragma once

/// @file SolIntType.h
/// First-class descriptor for a Solidity integer type: {bit-width, signedness}.
///
/// The AWST `WType` layer collapses every integer to a WIDTH TIER (`uint64` for
/// N<=64, `biguint` for N>64) and drops signedness entirely on the native path.
/// Historically that information was reconstructed at each use site from three
/// uncoordinated side channels:
///   1. the original solc `IntegerType` (re-`dynamic_cast`'d everywhere),
///   2. an ARC4 alias STRING (`"int" + bits`, re-parsed via `substr(0,3)=="int"`),
///   3. a separate `paramBitWidths` map threaded through the contexts.
///
/// `SolIntType` is the one carrier that names {bits, isSigned} explicitly, with
/// the width-tier / native-WType / modulus queries the arithmetic and coercion
/// layers repeatedly need. Construct it from whichever representation you have
/// (`fromSol` / `fromArc4`) and query it instead of re-deriving.

#include "awst/WType.h"

#include <optional>
#include <utility>

namespace solidity::frontend
{
class Type;
}

namespace puyasol::builder
{

struct SolIntType
{
	unsigned bits = 0;
	bool isSigned = false;

	/// True when this integer is backed by `biguint` (N>64) rather than `uint64`.
	/// This is the boundary the arithmetic paths branch on ("needs the biguint path").
	bool biguintBacked() const { return bits > 64; }

	/// The native AWST WType a value of this integer type carries: `uint64` for
	/// N<=64, `biguint` for N>64. (Signedness is not represented in the WType.)
	awst::WType const* nativeWType() const
	{
		return biguintBacked() ? awst::WType::biguintType() : awst::WType::uint64Type();
	}

	/// {2^bits, 2^(bits-1)} as decimal strings — the two's-complement wrap modulus
	/// and the sign-bit / INT_MIN boundary. Delegates to the centralised
	/// `TypeCoercion::pow2NAndHalf` (defined out-of-line to avoid a header cycle).
	std::pair<std::string, std::string> pow2NAndHalf() const;

	/// Build from a solc `Type` (unwrapping UDVTs). Returns nullopt when `_type`
	/// is not an integer type (e.g. a bool / address / rational literal), so
	/// callers can fall through to their own handling.
	static std::optional<SolIntType> fromSol(solidity::frontend::Type const* _type);

	/// Build from an ARC4 integer WType — reads signedness from `ARC4UIntN::isSigned()`
	/// (NOT by string-slicing the alias). Returns nullopt for a null / non-int WType.
	static std::optional<SolIntType> fromArc4(awst::WType const* _type);
};

} // namespace puyasol::builder
