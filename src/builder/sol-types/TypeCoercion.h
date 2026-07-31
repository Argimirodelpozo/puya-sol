#pragma once

/// @file TypeCoercion.h
/// Centralised type coercion / conversion utilities for AWST expressions.
///
/// All WType→WType transforms live here so that callers (ContractBuilder,
/// sol-ast wrappers, sol-eb builders, AssemblyBuilder) share one
/// implementation instead of copy-pasting padding / casting / sign-extension
/// logic in every visitor.

#include "awst/Node.h"
#include "builder/ReturnWirePlan.h"

#include <libsolutil/Numeric.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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

/// 2^255 as a decimal string — the signed 256-bit boundary (|type(int256).min| and the
/// sign-bit threshold). Centralised like kPow2_256; was hardcoded at ~9 sites.
inline constexpr char const* kHalfMax_256 =
	"57896044618658097711785492504343953926634992332820282019728792003956564819968";

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

	/// Relabel an UNSIZED `bytes` value as a declared fixed `bytesN`.
	///
	/// `bytes32 role = keccak256("MINTER_ROLE")` binds a value whose wtype is
	/// unsized `bytes` to a `bytes[32]` target. The bytes are already correct —
	/// only the wtype bookkeeping disagrees — but puya type-checks the pair and
	/// rejects the whole program ("assignment target type differs from
	/// expression value type"). Reinterpret so the label matches.
	/// No-op unless the target is a SIZED bytes and the source an UNSIZED one.
	static std::shared_ptr<awst::Expression> relabelUnsizedBytes(
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

	/// Encode ONE return value to its ABI wire form per its ReturnWireElem plan
	/// (build-time return encoding, fable-review-2 D2). masked → bitAnd to width;
	/// signed → signExtendToUint256 then ARC4Encode(arc4.uint256); unsigned biguint →
	/// ARC4Encode(arc4.uintN) (with `% 2^N` first when `_asmWrap`, since Yul is
	/// unchecked); array → ARC4Encode(arc4 array); everything else passes through.
	/// The build-time replacement for the old ReturnRewriter non-chain passes 1-5.
	static std::shared_ptr<awst::Expression> encodeReturnElement(
		std::shared_ptr<awst::Expression> _value,
		ReturnWireElem const& _plan,
		awst::SourceLocation const& _loc,
		bool _asmWrap = false
	);

	/// Encode a whole return VALUE (scalar or tuple) to its ABI wire form per the
	/// per-element plan — the build-time counterpart to ReturnRewriter passes 2/3/4.
	/// Handles a scalar, a literal tuple, a ternary-of-tuples, and an opaque tuple
	/// value (`return f()`) which spills to a temp appended to `_prepend`. Returns
	/// the (possibly new) value; the caller inserts `_prepend` before the return.
	static std::shared_ptr<awst::Expression> encodeReturnValue(
		std::shared_ptr<awst::Expression> _value,
		std::vector<ReturnWireElem> const& _plan,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _prepend,
		bool _asmWrap = false
	);

	/// Value of a dynamic CALLDATA param whose mutable pointer locals are live
	/// (an assembly block seeded or wrote `__cd_off_<name>` / `__cd_len_<name>`):
	/// `extract3(__cd_blob, off, len)` — the byte range the (possibly repointed)
	/// pointer designates inside the synthetic calldata blob. Locals are biguint
	/// (Yul word type); cast to uint64 for extract3.
	static std::shared_ptr<awst::Expression> calldataPointerValueRead(
		std::string const& _name,
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

	/// Canonicalise an UNSIGNED sub-256 biguint to its type width: `value mod 2^bits`. The dual of
	/// signExtendToUint256 for the unsigned side — every width-GROWING op (shift-left, `~`, unchecked
	/// wrap, signed→unsigned cast) must apply it so the value stays in [0, 2^bits-1] and a downstream
	/// CHECKED consumer / `<= type(uintN).max` compare doesn't see a non-canonical value. Callers guard
	/// `bits < 256` (2^256 is a no-op / overflows u256). Names the invariant the v427–v432 fixes share.
	static std::shared_ptr<awst::Expression> maskUnsignedToWidth(
		std::shared_ptr<awst::Expression> _value,
		unsigned _bits,
		awst::SourceLocation const& _loc
	);

	/// Return {2^bits, 2^(bits-1)} as decimal strings for an N-bit integer type — the
	/// modulus (two's-complement wrap) and the INT_MIN / sign-bit boundary that the signed
	/// arith / negate / inc-dec / div-mod / exp paths all need. Centralises the bits==256
	/// special case (u256(1)<<256 overflows u256, so kPow2_256 / kHalfMax_256 are used).
	/// Was a ~12-line if/else copy-pasted at 5 sites.
	static std::pair<std::string, std::string> pow2NAndHalf(unsigned _bits);

	/// Bool expression "is this signed value negative?" — `value >= 2^(bits-1)`, for a value
	/// already in canonical two's-complement biguint form. The single source for the sign-bit
	/// test the signed arith / div-mod / shift / assembly-Yul paths each hand-rolled with the
	/// 2^(N-1) literal. `value` is compared once (callers pass a var/temp if they reference it
	/// again). bits==256 uses kHalfMax_256 (2^256 overflows u256).
	static std::shared_ptr<awst::Expression> isNegativeSigned(
		std::shared_ptr<awst::Expression> _value,
		unsigned _bits,
		awst::SourceLocation const& _loc
	);

	/// Coerce a binary-op integer operand (built from `_srcSol`) to the operation's
	/// `commonType` (`_commonW` = its mapped wtype), producing a CANONICAL value at the
	/// common width: convert the wtype, then sign-extend a SIGNED operand from its own
	/// source width. For a literal operand (RationalNumberType, `_srcSol` not an
	/// IntegerType) the wtype conversion alone yields the canonical form (a negative
	/// biguint constant narrows to its low 64-bit two's complement). Lets the binary-op
	/// dispatch hand `compare()`/`binary_op` uniform same-width canonical operands —
	/// the single solc-`commonType`-driven point that replaces per-operand fix-ups.
	static std::shared_ptr<awst::Expression> coerceToCommonInt(
		std::shared_ptr<awst::Expression> _value,
		solidity::frontend::Type const* _srcSol,
		awst::WType const* _commonW,
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

	/// Emit an integer constant from a non-fractional rational constant's value
	/// (`RationalNumberType::literalValue()`, a 256-bit two's complement u256),
	/// promoting `_mappedType` from uint64 to biguint when the magnitude overflows
	/// uint64. Shared by SolLiteral (number literals) and tryConstantFold (folded
	/// constant binary ops) — both width-less rationals, distinct from the
	/// fixed-width `canonicalIntConstant` above.
	static std::shared_ptr<awst::Expression> rationalIntConstant(
		solidity::u256 const& _value,
		awst::WType const* _mappedType,
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

	/// Defense-in-depth tripwire (possible_solc.md item 6): at sites lowering a
	/// SOLIDITY implicit conversion, hard-error when solc's own
	/// `isImplicitlyConvertibleTo` disagrees the pair is legal. The source
	/// program type-checked, so a trip means OUR plumbing picked the wrong
	/// src/target types (the annotation-mixup class behind past sign-extend /
	/// widening bugs) — fail at compile time, not as a runtime divergence.
	/// No-op when either type is null. `_site` tags the caller for the message.
	static void assertImplicitlyConvertible(
		solidity::frontend::Type const* _srcSolType,
		solidity::frontend::Type const* _tgtSolType,
		awst::SourceLocation const& _loc,
		char const* _site
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

	/// Truncate a MONETARY amount (`.transfer`/`.send`/`{value:}`/ASA amount) to
	/// uint64 with an overflow PRE-check: a biguint amount >= 2^64 reverts rather
	/// than silently sending `amount mod 2^64` microAlgos/units — the AVM amount
	/// field is uint64, so a >2^64 value can't be represented and truncating it
	/// is a real money bug (`transfer(100 ether)` sent 1e20 mod 2^64). Asserts
	/// (pushed to `_preStmts`) before truncating; uint64 amounts pass through.
	static std::shared_ptr<awst::Expression> checkedAmountToUint64(
		std::vector<std::shared_ptr<awst::Statement>>& _preStmts,
		std::shared_ptr<awst::Expression> _amount,
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

	/// How a BARE (native) biguint wtype is named in a signature. The two answers
	/// are both load-bearing: puya's router publishes a bare-biguint subroutine arg
	/// as "uint512" (its 64-byte stack width — what the SPLITTER chunk sigs must
	/// match), while the ABI-selector convention collapses it to "uint256". Callers
	/// choose explicitly; keeping the choice implicit in per-file copies is how the
	/// splitter namers silently disagreed.
	enum class BareBiguintName { Uint512, Uint256 };

	/// THE canonical WType→ABI-signature type name (selector computation, splitter
	/// chunk sigs, helper method sigs). Handles native wtypes (void/bool/uint64/
	/// biguint/account/string/bytes), all ARC4 kinds (alias-aware: "byte", "string",
	/// "byte[]", "address" pass through exactly as puya publishes them), WTuple and
	/// ReferenceArray. Replaces the three per-file copies (this fn, the splitter's
	/// abiTypeName, PureHelperExtractor's arc4TypeName) that had drifted.
	static std::string wtypeToABIName(
		awst::WType const* _type,
		BareBiguintName _biguint = BareBiguintName::Uint256);

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

	/// Assemble an ARC4 method selector from a name and the already-mapped ARC4
	/// type-name strings for params and returns: `name(p0,p1,...)` followed by the
	/// return suffix — `(r0,r1,...)` for >1 return, the single name for exactly 1,
	/// or `void` for none. The four caller/encoder selector builders (SolExternalCall,
	/// both InnerCallHandlers overloads, AbiEncoderBuilder) hand-rolled this identical
	/// skeleton; only their per-type name mappers differ, so those stay at each site
	/// and feed the mapped strings here.
	static std::string buildArc4Selector(
		std::string const& _name,
		std::vector<std::string> const& _paramNames,
		std::vector<std::string> const& _retNames);

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
