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

	/// Encode a single expression as ARC4 bytes (32-byte padded for most types).
	/// Public because the selector+calldata handler TU calls it directly.
	static std::shared_ptr<awst::Expression> encodeArgAsARC4Bytes(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _argExpr,
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

	/// Encode a range of FunctionCall arguments using EVM ABI head/tail
	/// layout (single bytes blob). Used by abi.encodeWithSelector /
	/// abi.encodeWithSignature after they've consumed the leading
	/// selector/signature arg. The range is [_startIdx, args.size()).
	/// Returns a single concatenated bytes expression; if all args are
	/// static the result is just packed bytes without offsets.
	static std::shared_ptr<awst::Expression> encodeArgsHeadTail(
		ContractContext& _ctx,
		solidity::frontend::FunctionCall const& _callNode,
		size_t _startIdx,
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

	/// Extract a uint64 from a 32-byte ABI word (last 8 bytes → btoi).
	static std::shared_ptr<awst::Expression> uint64FromAbiWord(
		std::shared_ptr<awst::Expression> _word32,
		awst::SourceLocation const& _loc);

	/// Decode a single value from EVM ABI-encoded bytes at a given offset.
	/// Handles static types (uint, bool, address, bytesN) and dynamic types
	/// (bytes, string). Returns the decoded expression with the correct wtype.
	/// @param _data     The full ABI-encoded bytes expression
	/// @param _offset   Byte offset into _data where this value's head slot is
	/// @param _solType  The Solidity type to decode as
	/// @param _loc      Source location for generated nodes
	static std::shared_ptr<awst::Expression> decodeAbiValue(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _data,
		std::shared_ptr<awst::Expression> _offset,
		solidity::frontend::Type const* _solType,
		awst::SourceLocation const& _loc);

	/// Decode a dynamic array whose ELEMENTS are themselves dynamically encoded
	/// (uint256[][], bytes[], string[], …) from EVM ABI bytes, returning the
	/// ARC4 byte layout of the array. `_tailStart` is the absolute byte offset
	/// within `_data` where the array's `[len][offset-table][tails]` encoding
	/// begins. Mirrors encodeDynArrayDynElems (the inverse direction) and
	/// recurses through decodeDynTailToArc4Bytes for each element, so it handles
	/// arbitrary nesting depth. Emits a runtime loop via _ctx.prePendingStatements.
	static std::shared_ptr<awst::Expression> decodeDynArrayDynElemsBytes(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _data,
		std::shared_ptr<awst::Expression> _tailStart,
		solidity::frontend::Type const* _arrSolType,
		awst::WType const* _arc4Type,
		awst::SourceLocation const& _loc);

	/// Decode ONE dynamic value (whose `[len][...]` EVM encoding begins at the
	/// absolute offset `_tailStart` in `_data`) to its ARC4 element byte layout.
	/// Handles bytes/string ([uint16 len][bytes]), dynamic arrays of 32-byte
	/// EVM elements ([uint16 count][count×32]), and nested dynamic-element
	/// arrays (recurses into decodeDynArrayDynElemsBytes). Used as the per-element
	/// step of the nested-array offset-table walk.
	static std::shared_ptr<awst::Expression> decodeDynTailToArc4Bytes(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _data,
		std::shared_ptr<awst::Expression> _tailStart,
		solidity::frontend::Type const* _elemSolType,
		awst::WType const* _arc4Type,
		awst::SourceLocation const& _loc);

	// ── Individual handlers ──

	static std::unique_ptr<InstanceBuilder> handleEncodePacked(
		ContractContext& _ctx,
		solidity::frontend::FunctionCall const& _callNode,
		bool _isPacked,
		awst::SourceLocation const& _loc);

	/// EVM ABI encode with proper head/tail encoding for dynamic types.
	static std::unique_ptr<InstanceBuilder> handleEncode(
		ContractContext& _ctx,
		solidity::frontend::FunctionCall const& _callNode,
		awst::SourceLocation const& _loc);

	/// Right-pad bytes to 32-byte boundary (for dynamic data in tail).
	static std::shared_ptr<awst::Expression> rightPadTo32(
		std::shared_ptr<awst::Expression> _expr,
		awst::SourceLocation const& _loc);

	/// Encode a dynamic type's tail data: [length as 32 bytes][data right-padded to 32].
	static std::shared_ptr<awst::Expression> encodeDynamicTail(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _expr,
		solidity::frontend::Type const* _solType,
		awst::SourceLocation const& _loc);

	/// Convert ARC4-encoded bytes blob to EVM-ABI tail bytes for the given
	/// Solidity type. Recursive entry point used by the nested-dynamic
	/// encoder. Unlike `encodeDynamicTail` (which expects a typed
	/// expression), this takes raw bytes already extracted from a parent
	/// container so it can be called from inside a runtime loop body.
	static std::shared_ptr<awst::Expression> encodeFromArc4Bytes(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _bytesExpr,
		solidity::frontend::Type const* _solType,
		awst::SourceLocation const& _loc);

	/// Encode a dynamic array of small (non-32-byte) static elements as
	/// EVM-ABI bytes via a runtime loop. Handles uint8/uint16/.../uint128,
	/// int8/.../int128 (zero-extended; signed isn't yet sign-extended),
	/// bytes1..bytes31, bool, address. Emits a `while` loop into
	/// `_ctx.prePendingStatements` and returns a fresh local var holding
	/// the encoded bytes.
	static std::shared_ptr<awst::Expression> encodeDynArrayPadSmallElems(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _expr,
		solidity::frontend::Type const* _elemSolType,
		unsigned _elemByteSize,
		bool _isFixedBytes,
		bool _isSigned,
		awst::SourceLocation const& _loc);

	/// Encode a dynamic array of dynamic elements (nested dynamic) as
	/// EVM-ABI bytes via a runtime loop. Walks the ARC4 outer offset
	/// table, recursively encodes each inner via `encodeFromArc4Bytes`,
	/// builds new EVM-ABI head (uint256 offsets) + tail (re-encoded
	/// bodies). Emits a `while` loop into `_ctx.prePendingStatements`.
	static std::shared_ptr<awst::Expression> encodeDynArrayDynElems(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _expr,
		solidity::frontend::Type const* _elemSolType,
		awst::SourceLocation const& _loc);

	/// Encode a static-size array of dynamic elements (e.g. `bytes[3]`,
	/// `uint256[][3]`) as EVM-ABI bytes via a runtime loop. Same shape
	/// as `encodeDynArrayDynElems` but no leading uint256 length word
	/// and `n` is a compile-time constant.
	static std::shared_ptr<awst::Expression> encodeStaticArrayDynElems(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _expr,
		solidity::frontend::Type const* _elemSolType,
		unsigned _n,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> handleDecode(
		ContractContext& _ctx,
		solidity::frontend::FunctionCall const& _callNode,
		awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::eb
