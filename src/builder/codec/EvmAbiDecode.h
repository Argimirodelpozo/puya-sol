#pragma once

#include "awst/Node.h"
#include "builder/solc/SolcFwd.h"

namespace puyasol::builder { class TypeMapper; }
namespace puyasol::builder::abi
{

/// Materialize an already-resolved calldata reference, whose offset points
/// at data (past the length word for a dynamic array). Coordinates may have
/// been reassigned in Yul; the declared solc type still controls decoding.
std::shared_ptr<awst::Expression> readCalldataValue(
	TypeMapper&, std::shared_ptr<awst::Expression> blob, solidity::frontend::Type const* type,
	std::shared_ptr<awst::Expression> offset, std::shared_ptr<awst::Expression> length,
	awst::SourceLocation const&, std::vector<std::shared_ptr<awst::Statement>>&);

/// Decode a value blob once, using solc head sizes/dynamic predicates and
/// validator rules. Memory/return/constructor payloads use this entry.
std::shared_ptr<awst::Expression> decodeEvmAbi(
	TypeMapper&, std::shared_ptr<awst::Expression> blob,
	std::vector<solidity::frontend::Type const*> const& components,
	awst::WType const* target, awst::SourceLocation const&,
	std::vector<std::shared_ptr<awst::Statement>>&);

/// Router-only source: ApplicationArgs[1]. Uses the entry router's shared
/// word/offset/address helpers and memoizes aggregate decoders per contract.
/// No arbitrary blob can accidentally be paired with these same-source helpers.
std::shared_ptr<awst::Expression> decodeEvmCalldata(
	TypeMapper&, std::vector<solidity::frontend::Type const*> const& components,
	awst::WType const* target, awst::SourceLocation const&,
	std::vector<std::shared_ptr<awst::Statement>>&);

} // namespace puyasol::builder::abi
