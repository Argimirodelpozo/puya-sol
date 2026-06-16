#pragma once

#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

#include <memory>
#include <string>
#include <vector>

namespace puyasol::builder::eb
{

/// Thin InstanceBuilder wrapper around a bare AWST expression — used as
/// the return shape for abi.* handlers that have already produced the
/// final bytes expression and just need an InstanceBuilder envelope.
/// Defined in the header so sibling abi.* handler TUs can construct it.
class GenericAbiResult: public InstanceBuilder
{
public:
	GenericAbiResult(ContractContext& _ctx, std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr)) {}
	solidity::frontend::Type const* solType() const override { return nullptr; }
};

/// Handles abi.encode*, abi.decode functions.
///
/// Dispatched from visit(FunctionCall) when the callee is a MemberAccess on
/// MagicType(ABI) — i.e., `abi.encodePacked(...)`, `abi.encode(...)`, etc.
class AbiEncoderBuilder
{
public:
	/// Try to handle an abi.* member call.
	/// @param _memberName  "encodePacked", "encode", "encodeCall", "encodeWithSelector",
	///                     "encodeWithSignature", "decode"
	/// Returns nullptr if not handled.
	static std::unique_ptr<InstanceBuilder> tryHandle(
		ContractContext& _ctx,
		std::string const& _memberName,
		solidity::frontend::FunctionCall const& _callNode,
		awst::SourceLocation const& _loc);

	/// Build ARC4 method selector from a FunctionDefinition.
	/// Exposed for fn-pointer dispatch (cross-contract inner txn).
	static std::string buildARC4MethodSelector(
		ContractContext& _ctx,
		solidity::frontend::FunctionDefinition const* _funcDef);

	/// Concatenate a list of byte expressions using concat intrinsics.
	/// Public because the selector+calldata handler TU calls it directly.
	static std::shared_ptr<awst::Expression> concatByteExprs(
		std::vector<std::shared_ptr<awst::Expression>> _parts,
		awst::SourceLocation const& _loc);

	/// ARC4-encode an already-built list of argument values into a single bytes
	/// expression: 0 values → empty bytes; 1 value → that value's ARC4 bytes (NO
	/// tuple wrapper); N values → an ARC4 tuple. Each value is encoded at
	/// mapToARC4Type(value->wtype), so a value that is already ARC4 just
	/// reinterpret-casts to bytes. Shared by encodeArgsAsArc4 and abi.encodeCall
	/// (which pre-coerces each value to its declared parameter type first).
	static std::shared_ptr<awst::Expression> arc4EncodeValues(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>> _vals,
		awst::SourceLocation const& _loc);

	/// ARC4-encode the call arguments in the half-open range [_startIdx, end) at
	/// their own value types (single → bare value bytes; multiple → ARC4 tuple).
	/// Used by abi.encode (start 0) and abi.encodeWith{Selector,Signature} (start
	/// 1, after the leading selector/signature arg). The ARC4 counterpart to the
	/// EVM-layout encodeArgsHeadTail.
	static std::shared_ptr<awst::Expression> encodeArgsAsArc4(
		ContractContext& _ctx,
		solidity::frontend::FunctionCall const& _callNode,
		size_t _startIdx,
		awst::SourceLocation const& _loc);

	/// ARC4-encode a list of call arguments coerced to the callee's DECLARED
	/// parameter types (NOT the value wtypes): each arg is coerced via
	/// coerceForAssignment(buildExpr(arg), map(paramType_i)) then encoded (single →
	/// bare value bytes; multiple → ARC4 tuple). Used where the encoding must match
	/// a known callee signature — abi.encodeCall and custom-error revert payloads —
	/// so e.g. a literal `7` to a uint256 param rides at arc4.uint256 (32B), not
	/// the value's arc4.uint64. `_paramTypes` is matched by index; args past its
	/// end fall back to their value type.
	static std::shared_ptr<awst::Expression> arc4EncodeArgsAtParamTypes(
		ContractContext& _ctx,
		std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression const>> const& _args,
		std::vector<solidity::frontend::Type const*> const& _paramTypes,
		awst::SourceLocation const& _loc);

private:
	// ── Encoding helpers ──

	/// Convert expression to bytes, respecting packed byte width from Solidity type.
	/// For encodePacked: uint8 → 1 byte, uint256 → 32 bytes, etc.
	/// For encode: always 32-byte ABI words.
	static std::shared_ptr<awst::Expression> toPackedBytes(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _expr,
		solidity::frontend::Type const* _solType,
		bool _isPacked,
		awst::SourceLocation const& _loc);

	/// Left-pad bytes to exactly N bytes.
	static std::shared_ptr<awst::Expression> leftPadBytes(
		std::shared_ptr<awst::Expression> _expr, int _n,
		awst::SourceLocation const& _loc);

	/// Sign-extend a <=32-byte big-endian two's-complement value to a 32-byte
	/// ABI word: the high bytes are filled with the sign (0xff when byte 0's
	/// top bit is set, else 0x00) and the low `len` bytes keep the value, via
	/// `replace3` so it is robust to the value's runtime width AND idempotent
	/// on an already-32-byte input. Used by abi.encode for signed integers
	/// whose canonical form is two's-complement but whose plain leftpad would
	/// zero-fill the high bytes (0x00…00fffd instead of 0xff…fffd).
	static std::shared_ptr<awst::Expression> signExtendBytesTo32(
		std::shared_ptr<awst::Expression> _bytes,
		awst::SourceLocation const& _loc);

	// ── Individual handlers ──

	static std::unique_ptr<InstanceBuilder> handleEncodePacked(
		ContractContext& _ctx,
		solidity::frontend::FunctionCall const& _callNode,
		bool _isPacked,
		awst::SourceLocation const& _loc);

	/// abi.encode → ARC4 (delegates to encodeArgsAsArc4; no EVM head/tail).
	static std::unique_ptr<InstanceBuilder> handleEncode(
		ContractContext& _ctx,
		solidity::frontend::FunctionCall const& _callNode,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> handleDecode(
		ContractContext& _ctx,
		solidity::frontend::FunctionCall const& _callNode,
		awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::eb
