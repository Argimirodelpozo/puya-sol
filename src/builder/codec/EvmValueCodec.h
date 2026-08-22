#pragma once

#include <set>

#include "awst/Node.h"

#include "builder/sol-types/SolcFwd.h"

#include <memory>
#include <vector>

namespace puyasol::builder
{
class TypeMapper;
}

namespace puyasol::builder::codec
{

/// Strip arbitrarily nested user-defined value types.  All EVM wire and
/// memory layout decisions are properties of the underlying type.
solidity::frontend::Type const* underlyingType(
	solidity::frontend::Type const* type);

/// True for a value represented by one EVM word.  Aggregate recursion lives
/// in the ABI and memory source adapters; this is their common leaf predicate.
bool isWordType(solidity::frontend::Type const* type);

/// What a word's bytes OUTSIDE the value's own width mean at this read site.
///
/// The two callers want opposite things and solc agrees with both. Decoding
/// ABI input is a trust boundary, and solc's via-IR decoder emits
/// `validator_revert_t_*` there. Reading a value back out of MEMORY is not:
/// solc emits `cleanup_t_*`, a mask, because a program may legally have
/// dirtied those bytes through inline assembly. Validating a memory read made
/// `uint8[1] memory m; assembly { mstore(m, 257) } m[0]` revert where the EVM
/// yields 0x01, and made every `bytes memory` element read revert as soon as
/// a non-zero byte followed it in the array.
///
/// Enums are validated under BOTH policies, matching solc: `cleanup_t_enum` is
/// `validator_assert_t_enum`, because an out-of-range enum is a Panic even on
/// a plain read.
enum class PaddingPolicy
{
	Validate,   ///< revert unless the surrounding bytes are canonical padding
	Clean,      ///< discard them (mask / zero-extend / sign-extend)
};

/// Decode one 32-byte EVM word to puya-sol's native value representation.
/// Canonical bool and enum checks are appended to `out`.
std::shared_ptr<awst::Expression> valueFromEvmWord(
	TypeMapper& typeMapper,
	solidity::frontend::Type const* solType,
	std::shared_ptr<awst::Expression> word,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out,
	PaddingPolicy padding);

/// Convert a native or ARC4-backed scalar value to its canonical 32-byte EVM
/// word.  bytesN values are left-aligned; signed integers are sign-extended.
std::shared_ptr<awst::Expression> valueToEvmWord(
	TypeMapper& typeMapper,
	solidity::frontend::Type const* solType,
	std::shared_ptr<awst::Expression> value,
	awst::SourceLocation const& loc);

/// Convert a decoded native value to the ARC4 type used inside an ARC4
/// aggregate.  bytes/string are framed directly as uint16-length + payload,
/// avoiding backend-specific encode special cases.
std::shared_ptr<awst::Expression> valueToArc4(
	TypeMapper& typeMapper,
	solidity::frontend::Type const* solType,
	std::shared_ptr<awst::Expression> value,
	awst::WType const* arc4Type,
	awst::SourceLocation const& loc);

/// Inverse of valueToArc4 for scalar/reference leaves.  Aggregate ARC4 values
/// already are the compiler's native representation and pass through.
std::shared_ptr<awst::Expression> valueFromArc4(
	TypeMapper& typeMapper,
	solidity::frontend::Type const* solType,
	std::shared_ptr<awst::Expression> value,
	awst::SourceLocation const& loc);

/// Width-agnostic two's-complement sign extension to an EVM word.
std::shared_ptr<awst::Expression> signExtendToWord(
	std::shared_ptr<awst::Expression> bytes,
	awst::SourceLocation const& loc);

/// True when `type` can round-trip through canonical EVM ABI encoding.
///
/// One predicate, not two: EvmAbiEncode::canEncode and EvmAbiDecode::canDecode
/// were byte-identical apart from the name of the recursive call, and there is
/// no encode/decode asymmetry in the question being asked.
bool canRoundTripEvmAbi(
	solidity::frontend::Type const* type,
	std::set<int64_t>& visiting);

} // namespace puyasol::builder::codec
