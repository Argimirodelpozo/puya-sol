#pragma once

#include "awst/Node.h"
#include "builder/sol-eb/ContractContext.h"

#include <libsolidity/ast/ASTForward.h>

#include <memory>
#include <optional>

namespace puyasol::builder::eb
{

/// Compiler-side implementation of the ARC4 facade declared in
/// `libs/AVM.sol`.
///
/// The facade deliberately uses ordinary Solidity types:
///
///     ARC4.encode(abi.encode(a, b))
///     abi.decode(ARC4.decode(data), (A, B))
///
/// The nested abi.* calls are type envelopes. They are inspected before their
/// arguments are lowered, so no EVM ABI bytes are produced or consumed.
class Arc4Stdlib
{
public:
	/// True only for the two stock facade declarations in the canonical
	/// `libs/AVM.sol` source unit. Used by both call lowering and root emission.
	static bool isFacadeFunction(
		solidity::frontend::FunctionDefinition const& _function);

	/// Handle a direct call to a resolved ARC4 stdlib function. `encode` is
	/// lowered here; a direct `decode` is diagnosed because it is only meaningful
	/// as the input envelope of `abi.decode`.
	static std::optional<std::shared_ptr<awst::Expression>> tryHandleCall(
		ContractContext& _ctx,
		solidity::frontend::MemberAccess const& _memberAccess,
		solidity::frontend::FunctionCall const& _call,
		awst::SourceLocation const& _loc);

	/// Recognize and lower `abi.decode(ARC4.decode(data), (T...))`.
	/// Returns nullopt for an ordinary Solidity ABI decode.
	static std::optional<std::shared_ptr<awst::Expression>> tryHandleDecodeEnvelope(
		ContractContext& _ctx,
		solidity::frontend::FunctionCall const& _abiDecodeCall,
		awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::eb
