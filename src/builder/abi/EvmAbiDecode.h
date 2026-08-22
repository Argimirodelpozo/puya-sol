#pragma once

#include "awst/Node.h"

#include "builder/sol-types/SolcFwd.h"

#include <memory>
#include <vector>

namespace puyasol::builder
{
class TypeMapper;
}

namespace puyasol::builder::abi
{

/// Recursive capability check for the external EVM ABI decoder.
bool canDecodeEvmAbi(
	std::vector<solidity::frontend::Type const*> const& components);

/// Decode one ABI tuple (or a single component) recursively.  Solidity's own
/// isDynamicallyEncoded()/calldataHeadSize()/calldataEncodedTailSize() facts
/// define every head stride and offset base; no rank or element-width cases
/// are encoded here.
std::shared_ptr<awst::Expression> decodeEvmAbi(
	TypeMapper& typeMapper,
	std::shared_ptr<awst::Expression> blob,
	std::vector<solidity::frontend::Type const*> const& components,
	awst::WType const* targetType,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out);

} // namespace puyasol::builder::abi
