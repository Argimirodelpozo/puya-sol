#pragma once

#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

#include <memory>
#include <string>
#include <vector>

namespace puyasol::builder::eb
{

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
		BuilderContext& _ctx,
		std::string const& _memberName,
		solidity::frontend::FunctionCall const& _callNode,
		awst::SourceLocation const& _loc);

private:
	// ── Encoding helpers ──

	/// Convert expression to bytes, respecting packed byte width from Solidity type.
	/// For encodePacked: uint8 → 1 byte, uint256 → 32 bytes, etc.
	/// For encode: always 32-byte ABI words.
	static std::shared_ptr<awst::Expression> toPackedBytes(
		BuilderContext& _ctx,
		std::shared_ptr<awst::Expression> _expr,
		solidity::frontend::Type const* _solType,
		bool _isPacked,
		awst::SourceLocation const& _loc);

	/// Encode a single expression as ARC4 bytes (32-byte padded for most types).
	static std::shared_ptr<awst::Expression> encodeArgAsARC4Bytes(
		BuilderContext& _ctx,
		std::shared_ptr<awst::Expression> _argExpr,
		awst::SourceLocation const& _loc);

	/// Build ARC4 method selector from a FunctionDefinition.
	static std::string buildARC4MethodSelector(
		BuilderContext& _ctx,
		solidity::frontend::FunctionDefinition const* _funcDef);

	/// Concatenate a list of byte expressions using concat intrinsics.
	static std::shared_ptr<awst::Expression> concatByteExprs(
		std::vector<std::shared_ptr<awst::Expression>> _parts,
		awst::SourceLocation const& _loc);

	/// Left-pad bytes to exactly N bytes.
	static std::shared_ptr<awst::Expression> leftPadBytes(
		std::shared_ptr<awst::Expression> _expr, int _n,
		awst::SourceLocation const& _loc);

	// ── Individual handlers ──

	static std::unique_ptr<InstanceBuilder> handleEncodePacked(
		BuilderContext& _ctx,
		solidity::frontend::FunctionCall const& _callNode,
		bool _isPacked,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> handleEncodeCall(
		BuilderContext& _ctx,
		solidity::frontend::FunctionCall const& _callNode,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> handleEncodeWithSelector(
		BuilderContext& _ctx,
		solidity::frontend::FunctionCall const& _callNode,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> handleEncodeWithSignature(
		BuilderContext& _ctx,
		solidity::frontend::FunctionCall const& _callNode,
		awst::SourceLocation const& _loc);

	static std::unique_ptr<InstanceBuilder> handleDecode(
		BuilderContext& _ctx,
		solidity::frontend::FunctionCall const& _callNode,
		awst::SourceLocation const& _loc);

	static std::shared_ptr<awst::Expression> makeUint64(
		std::string _value, awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::eb
