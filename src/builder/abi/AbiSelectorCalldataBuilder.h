#pragma once

/// @file AbiSelectorCalldataBuilder.h
/// Three abi.* handlers that produce `[selector][ABI-encoded args]`
/// calldata, split out from AbiEncoderBuilder:
///
/// - `abi.encodeCall(Target.fn, (...))` — compile-time selector from
///   FunctionDefinition (or runtime extract from an external fn-ptr)
///   plus EVM-ABI head/tail of the typed args.
/// - `abi.encodeWithSelector(bytes4 sel, ...)` — runtime selector
///   (any-width integer literal accepted, coerced to 4 bytes) plus the
///   ARC4 encoding of the remaining args.
/// - `abi.encodeWithSignature(string sig, ...)` — sha512_256 of the
///   signature string (AVM convention, NOT EVM keccak256), first 4 bytes,
///   plus the ARC4 encoding of the remaining args.
///
/// All three share the same shape (build a selector, optionally append
/// `encodeArgsAsArc4` / `arc4EncodeArgsAtParamTypes`) — they're cohesive
/// but voluminous, so they live in their own TU. The dispatcher in
/// `AbiEncoderBuilder::tryHandle` calls these free functions directly.

#include "awst/Node.h"
#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/AST.h>

#include <memory>

namespace puyasol::builder::eb
{

std::unique_ptr<InstanceBuilder> handleEncodeCall(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc);

std::unique_ptr<InstanceBuilder> handleEncodeWithSelector(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc);

std::unique_ptr<InstanceBuilder> handleEncodeWithSignature(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc);

} // namespace puyasol::builder::eb
