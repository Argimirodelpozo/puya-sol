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
/// `wordFetchSub`: optional member-subroutine name (e.g. "__evm_decw") that
/// fetches the bounds-checked 32-byte word at a uint64 offset of the SAME
/// blob. When set, every head/offset word read becomes `callsub` instead of
/// the inline bounds-assert + extract3 — the EVM entry arms share one fetch
/// body across all routes (a 55-method contract repeated the inline form
/// hundreds of times). Only valid when the subroutine reads the same bytes
/// the decoder was given (the arms pass ApplicationArgs[1]).
/// `smallWordSub` / `addressSub`: optional sibling helpers with the same
/// same-blob contract — `smallWordSub(off) -> uint64` replaces the inline
/// offset/length small-word fetch (word + high-24-zero assert + btoi) and
/// `addressSub(off) -> account` the address leaf (word + padding assert).
std::shared_ptr<awst::Expression> decodeEvmAbi(
	TypeMapper& typeMapper,
	std::shared_ptr<awst::Expression> blob,
	std::vector<solidity::frontend::Type const*> const& components,
	awst::WType const* targetType,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out,
	char const* wordFetchSub = nullptr,
	char const* smallWordSub = nullptr,
	char const* addressSub = nullptr);

} // namespace puyasol::builder::abi
