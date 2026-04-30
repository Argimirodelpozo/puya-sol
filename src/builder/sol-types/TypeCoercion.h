#pragma once

/// @file TypeCoercion.h
/// Centralised type coercion / conversion utilities for AWST expressions.
///
/// All WType→WType transforms live here so that callers (ContractBuilder,
/// sol-ast wrappers, sol-eb builders, AssemblyBuilder) share one
/// implementation instead of copy-pasting padding / casting / sign-extension
/// logic in every visitor.

#include "awst/Node.h"

#include <memory>
#include <string>

namespace puyasol::builder
{

/// 2^256 as a decimal string — used across the compiler for modular wrapping,
/// sign extension, and overflow detection.  Centralised here to avoid 15+
/// copies of the same 78-digit literal scattered through the codebase.
inline constexpr char const* kPow2_256 =
	"115792089237316195423570985008687907853269984665640564039457584007913129639936";

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

	/// Compute the fixed encoded byte size of an ARC4 type.
	/// Returns 0 for variable-length types.
	static int computeEncodedElementSize(awst::WType const* _type);

private:
	/// Emit `bzero(_n)` wrapped in a ReinterpretCast to _targetType so the
	/// zero region is allocated at runtime instead of baked into a pushbytes
	/// constant. Used for default values of large ARC4 static arrays and
	/// fixed-size byte arrays, whose bytecode-inlined form would exceed
	/// puya's ~4KB bytes constant limit.
	static std::shared_ptr<awst::Expression> makeZeroBytesRuntime(
		int _n,
		awst::WType const* _targetType,
		awst::SourceLocation const& _loc
	);

	/// Convert a fixed-size ARC4 static array to a dynamic ARC4 array by
	/// prepending a 2-byte big-endian length header with the statically
	/// known element count.
	static std::shared_ptr<awst::Expression> prependArc4LengthHeader(
		std::shared_ptr<awst::Expression> _expr,
		int64_t _length,
		awst::WType const* _targetType,
		awst::SourceLocation const& _loc
	);

	/// Threshold (bytes) above which default zero values are emitted as
	/// runtime `bzero(N)` instead of a baked-in BytesConstant. Chosen under
	/// the AVM/puya ~4KB pushbytes cap with headroom for surrounding ops.
	static constexpr int kLargeBytesRuntimeThreshold = 2048;
};

} // namespace puyasol::builder
