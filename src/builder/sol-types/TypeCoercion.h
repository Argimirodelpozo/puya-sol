#pragma once

/// @file TypeCoercion.h
/// Centralised type coercion / conversion utilities for AWST expressions.
///
/// All WType→WType transforms live here so that callers (ContractBuilder,
/// sol-ast wrappers, sol-eb builders, AssemblyBuilder) share one
/// implementation instead of copy-pasting padding / casting / sign-extension
/// logic in every visitor.

#include "awst/Node.h"

#include <libsolutil/Numeric.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace solidity::frontend
{
class Type;
}

namespace puyasol::builder
{

/// 2^256 as a decimal string — used across the compiler for modular wrapping,
/// sign extension, and overflow detection.  Centralised here to avoid 15+
/// copies of the same 78-digit literal scattered through the codebase.
inline constexpr char const* kPow2_256 =
	"115792089237316195423570985008687907853269984665640564039457584007913129639936";

/// Construct a biguint IntegerConstant holding 2^256. Wraps the common
/// `makeIntegerConstant(kPow2_256, loc, biguintType())` call used by
/// ~8 sites for modular-arithmetic wrapping. The biguint type is fixed
/// here so callers can't accidentally type-mismatch by omitting it.
inline std::shared_ptr<awst::IntegerConstant> makePow256(
	awst::SourceLocation const& _loc)
{
	return awst::makeIntegerConstant(kPow2_256, _loc, awst::WType::biguintType());
}

class TypeCoercion
{
public:
	// ── Numeric ──────────────────────────────────────────────────

	/// Insert implicit numeric cast if needed (uint64 ↔ biguint).
	/// Returns the expression unchanged when no cast is needed.
	static std::shared_ptr<awst::Expression> implicitNumericCast(
		std::shared_ptr<awst::Expression> _expr,
		awst::WType const* _targetType,
		awst::SourceLocation const& _loc
	);

	/// Sign-extend an N-bit signed integer to 256-bit two's complement.
	/// Masks to N bits, then conditionally adds (2^256 − 2^N) mod 2^256.
	static std::shared_ptr<awst::Expression> signExtendToUint256(
		std::shared_ptr<awst::Expression> _value,
		unsigned _bits,
		awst::SourceLocation const& _loc
	);

	/// Sign-extend an N-bit (N<64) signed value held in a uint64 to the 64-bit
	/// two's-complement form. Input must be in [0, 2^N-1] (e.g. the raw result
	/// of decoding a packed arc4.intN field). If the N-bit sign bit is set, adds
	/// (2^64 − 2^N) to set the high bits; the sum stays < 2^64 for all inputs.
	static std::shared_ptr<awst::Expression> signExtendToUint64(
		std::shared_ptr<awst::Expression> _value,
		unsigned _bits,
		awst::SourceLocation const& _loc
	);

	/// Emit a canonical AWST integer constant from a 256-bit two's-complement value
	/// for an N-bit Solidity integer type. Centralises the "solc value -> canonical
	/// constant" rule that SolLiteral, tryConstantFold and type(T).min/max each
	/// hand-rolled (and had to keep mutually consistent): N<=64 -> low 64-bit TC
	/// (uint64 wtype, e.g. int8 -1 -> 0xff..ff), N>64 -> 256-bit TC (biguint wtype).
	/// `_tcValue` is the value already in 256-bit two's complement (as solc's
	/// literalValue() / IntegerType::min() return it for negatives).
	static std::shared_ptr<awst::Expression> canonicalIntConstant(
		solidity::u256 const& _tcValue,
		unsigned _bits,
		awst::SourceLocation const& _loc
	);

	/// Sign-extend a decoded signed sub-256 *array element* from its raw N-bit
	/// two's complement to the canonical 256-bit biguint, matching how scalar
	/// signed params are decoded — so `a[i]` compares/arithmetics equal to a
	/// scalar of the same type. No-op (returns `_value` unchanged) for unsigned,
	/// `int256` (already canonical), `<=64`-bit (uint64-backed, which carry their
	/// own sign handling) and non-integer element types. `_solElemType` is the
	/// Solidity element type; UDVTs are unwrapped to their underlying type.
	static std::shared_ptr<awst::Expression> signExtendSignedElement(
		std::shared_ptr<awst::Expression> _value,
		solidity::frontend::Type const* _solElemType,
		awst::SourceLocation const& _loc
	);

	/// Sign-extend a signed intN value widened to a wider signed intM (re-fills the sign the
	/// uint64-backed / zero-extending value model drops). No-op unless both Solidity types are
	/// signed ints with srcBits < tgtBits. Handles both target tiers (≤64 uint64, >64 biguint).
	/// Call at implicit-widening sites that have the Solidity src+target types (assignment, arg).
	static std::shared_ptr<awst::Expression> signExtendSignedWiden(
		std::shared_ptr<awst::Expression> _value,
		solidity::frontend::Type const* _srcSolType,
		solidity::frontend::Type const* _tgtSolType,
		awst::SourceLocation const& _loc
	);

	/// Truncate an array index to uint64 with an out-of-bounds PRE-check: a wide (biguint) index
	/// >= 2^64 reverts (it can't be a valid index) rather than silently truncating its high bits.
	/// Asserts (pushed to `_preStmts`) before truncating. Use at every array index-access site.
	static std::shared_ptr<awst::Expression> checkedIndexToUint64(
		std::vector<std::shared_ptr<awst::Statement>>& _preStmts,
		std::shared_ptr<awst::Expression> _idx,
		awst::SourceLocation const& _loc
	);

	// ── Bytes ────────────────────────────────────────────────────

	/// Convert a StringConstant to a right-padded BytesConstant of length _n.
	/// Returns nullptr if _src is not a StringConstant.
	static std::shared_ptr<awst::BytesConstant> stringToBytesN(
		awst::Expression const* _src,
		awst::WType const* _targetType,
		int _n,
		awst::SourceLocation const& _loc
	);

	/// Low-N-byte big-endian bytes of a non-negative integer-literal decimal
	/// string (for `bytesN x = <intlit>`). Re-parses via solidity::u256
	/// (boost::multiprecision) rather than a hand-rolled digit loop; bytes
	/// beyond N are dropped, missing high bytes stay 0. N<=32 so it fits u256.
	static std::vector<uint8_t> intLiteralToBytesN(std::string const& _decimal, int _n);

	/// Create a ReinterpretCast wrapping _expr with _targetType.
	static std::shared_ptr<awst::ReinterpretCast> reinterpretCast(
		std::shared_ptr<awst::Expression> _expr,
		awst::WType const* _targetType,
		awst::SourceLocation const& _loc
	);

	/// Coerce a string literal to raw bytes if needed for byte-level operations.
	/// Converts StringConstant → BytesConstant so it can be used in ARC4Encode
	/// or byte array element assignment without type mismatch.
	/// Returns the original expression unchanged if no coercion is needed.
	static std::shared_ptr<awst::Expression> stringToBytes(
		std::shared_ptr<awst::Expression> _expr,
		awst::SourceLocation const& _loc
	);

	/// Coerce an expression's type to match a target type for assignment.
	/// Handles: IntegerConstant→BytesConstant(bytes[N]), string→bytes,
	/// uint64/biguint numeric casts, ReinterpretCast for bytes-compatible types.
	/// Returns the original expression if no coercion needed.
	static std::shared_ptr<awst::Expression> coerceForAssignment(
		std::shared_ptr<awst::Expression> _expr,
		awst::WType const* _targetType,
		awst::SourceLocation const& _loc
	);

	// ── ARC4 / ABI ───────────────────────────────────────────────

	/// Canonical ABI type name for selector computation.
	static std::string wtypeToABIName(awst::WType const* _type);

	/// Canonical ARC4 method-selector type name for a Solidity INTEGER type,
	/// matching exactly what the callee's on-chain router emits (verified via the
	/// TEAL `method "..."` strings): `<= 64`-bit → "uint64" (width AND signedness
	/// collapsed), `> 64`-bit → "uint" + numBits (exact width, signedness
	/// dropped). E.g. uint8/int40/int64 → "uint64"; uint128/int128 → "uint128".
	/// Returns nullopt if `_type` (UDVT-unwrapped) is not an integer, so callers
	/// fall through to their own handling. The three caller-side selector
	/// builders (SolExternalCall, AbiEncoderBuilder::buildARC4MethodSelector,
	/// InnerCallHandlers::buildMethodSelector) MUST share this or cross-contract
	/// selectors mismatch and calls mis-route.
	static std::optional<std::string> intSelectorName(
		solidity::frontend::Type const* _type);

	/// Like intSelectorName but for a RETURN-position integer. Identical for
	/// unsigned, but a SIGNED return is named "uint256" (any width) — the callee
	/// encodes signed returns as the full 256-bit two's complement. Params and
	/// returns differ for signed ints, so the caller-side selector builders must
	/// use this for return types and intSelectorName for params.
	static std::optional<std::string> intSelectorReturnName(
		solidity::frontend::Type const* _type);

	// ── Defaults ─────────────────────────────────────────────────

	/// Type-correct default value expression (0 / false / empty bytes / …).
	static std::shared_ptr<awst::Expression> makeDefaultValue(
		awst::WType const* _type,
		awst::SourceLocation const& _loc
	);

private:
	/// Threshold (bytes) above which default zero values are emitted as
	/// runtime `bzero(N)` instead of a baked-in BytesConstant. Chosen under
	/// the AVM/puya ~4KB pushbytes cap with headroom for surrounding ops.
	static constexpr int kLargeBytesRuntimeThreshold = 2048;
};

} // namespace puyasol::builder
