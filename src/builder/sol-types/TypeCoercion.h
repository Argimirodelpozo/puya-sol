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
