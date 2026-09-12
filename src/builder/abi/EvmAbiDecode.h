#pragma once

#include "awst/Node.h"
#include "builder/sol-types/SolcFwd.h"

namespace puyasol::builder { class TypeMapper; }
namespace puyasol::builder::abi
{

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
