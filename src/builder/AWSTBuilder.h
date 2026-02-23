#pragma once

#include "awst/Node.h"
#include "builder/ContractTranslator.h"
#include "builder/StorageMapper.h"
#include "builder/TypeMapper.h"

#include <libsolidity/interface/CompilerStack.h>

#include <memory>
#include <string>
#include <vector>

namespace puyasol::builder
{

/// Top-level builder that drives the Solidity AST → AWST transformation.
/// Uses CompilerStack for parsing and type-checking, then visits all contracts.
class AWSTBuilder
{
public:
	/// Build AWST from a Solidity source file.
	/// Returns root nodes (contracts, subroutines) for JSON serialization.
	std::vector<std::shared_ptr<awst::RootNode>> build(
		solidity::frontend::CompilerStack& _compiler,
		std::string const& _sourceFile
	);

private:
	TypeMapper m_typeMapper;
	std::unique_ptr<StorageMapper> m_storageMapper;

	/// Registry of library function IDs, populated during the library translation pass.
	LibraryFunctionIdMap m_libraryFunctionIds;
};

} // namespace puyasol::builder
