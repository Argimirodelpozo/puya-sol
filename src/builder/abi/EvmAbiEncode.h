#pragma once

#include "awst/Node.h"

#include <libsolidity/ast/Types.h>

#include <memory>
#include <vector>

namespace puyasol::builder
{
class TypeMapper;
}

namespace puyasol::builder::abi
{

/// Recursive capability check for the canonical Solidity ABI encoder.
bool canEncodeEvmAbi(
	std::vector<solidity::frontend::Type const*> const& components);

/// Encode a tuple of values using Solidity's canonical ABI head/tail layout.
/// solc owns every layout decision (dynamic predicate and calldata head size);
/// this adapter only materialises those facts as AWST. Runtime loops make array
/// rank and length irrelevant to the implementation.
std::shared_ptr<awst::Expression> encodeEvmAbi(
	TypeMapper& typeMapper,
	std::vector<solidity::frontend::Type const*> const& components,
	std::vector<std::shared_ptr<awst::Expression>> values,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out);

} // namespace puyasol::builder::abi
