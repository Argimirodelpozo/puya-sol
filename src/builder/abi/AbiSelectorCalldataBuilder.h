#pragma once

/// @file AbiSelectorCalldataBuilder.h
/// Three abi.* handlers that produce `[selector][ABI-encoded args]`
/// calldata, split out from AbiEncoderBuilder:
///
/// - `abi.encodeCall(Target.fn, (...))` — compile-time selector from
///   FunctionDefinition (or runtime extract from an external fn-ptr)
///   plus EVM-ABI head/tail of the typed args.
/// - `abi.encodeWithSelector(bytes4 sel, ...)` — runtime selector
///   (using solc's implicit bytes4 conversion) plus canonical
///   EVM ABI encoding of the remaining args.
/// - `abi.encodeWithSignature(string sig, ...)` — keccak256 of the signature,
///   first 4 bytes, plus canonical EVM ABI encoding of the remaining args.
///
/// All three share the same shape (build a selector, append the recursive EVM
/// encoder). The dispatcher in
/// `AbiEncoderBuilder::build` calls these free functions directly.

#include "awst/Node.h"
#include "builder/sol-types/SolcFwd.h"

#include <libsolidity/ast/ASTForward.h>

#include <memory>

namespace puyasol::builder::eb
{
class ContractContext;

std::shared_ptr<awst::Expression> handleEncodeCall(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc);

std::shared_ptr<awst::Expression> handleEncodeWithSelector(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc);

std::shared_ptr<awst::Expression> handleEncodeWithSignature(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc);

} // namespace puyasol::builder::eb
