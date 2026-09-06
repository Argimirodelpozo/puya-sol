#pragma once

#include "awst/Node.h"
#include "builder/sol-types/SolcFwd.h"
#include <libsolutil/Numeric.h>

namespace puyasol::builder::StorageKey
{

/// Default-layout holder format 2. Source names and compilation-local IDs
/// never enter this namespace. Existing deployments require their old artifacts.
std::string root(solidity::u256 const& slot, unsigned offset);

/// Every descendant is sha256(domain ++ tag ++ len(parent) ++ parent ++ payload).
/// Members consume solc offsets; array and mapping steps have distinct tags.
std::shared_ptr<awst::Expression> member(
	std::shared_ptr<awst::Expression> parent,
	solidity::frontend::StructType const& type, std::string const& name,
	awst::SourceLocation const& loc);

/// The caller must check array bounds before deriving its holder identity.
std::shared_ptr<awst::Expression> arrayElement(
	std::shared_ptr<awst::Expression> parent,
	std::shared_ptr<awst::Expression> index, awst::SourceLocation const& loc);

std::shared_ptr<awst::Expression> mappingEntry(
	std::shared_ptr<awst::Expression> parent,
	std::shared_ptr<awst::Expression> key, awst::WType const* keyType,
	awst::SourceLocation const& loc);

} // namespace puyasol::builder::StorageKey
