#pragma once

/// @file TypeCoercion.h
/// Centralised type coercion / conversion utilities for AWST expressions.
///
/// All WType→WType transforms live here so that callers (ContractBuilder,
/// sol-ast wrappers, sol-eb builders, AssemblyBuilder) share one
/// implementation instead of copy-pasting padding / casting / sign-extension
/// logic in every visitor.

#include "awst/Node.h"

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

	// ── Bytes ────────────────────────────────────────────────────

	/// Convert a StringConstant to a right-padded BytesConstant of length _n.
	/// Returns nullptr if _src is not a StringConstant.
	static std::shared_ptr<awst::BytesConstant> stringToBytesN(
		awst::Expression const* _src,
		awst::WType const* _targetType,
		int _n,
		awst::SourceLocation const& _loc
	);

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
