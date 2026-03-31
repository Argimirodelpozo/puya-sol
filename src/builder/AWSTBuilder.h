#pragma once

#include "awst/Node.h"
#include "builder/ContractBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"

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
	/// _opupBudget: if > 0, inject ensure_budget(_opupBudget) into public methods.
	std::vector<std::shared_ptr<awst::RootNode>> build(
		solidity::frontend::CompilerStack& _compiler,
		std::string const& _sourceFile,
		uint64_t _opupBudget = 0,
		std::map<std::string, uint64_t> const& _ensureBudget = {}
	);

private:
	TypeMapper m_typeMapper;
	std::unique_ptr<StorageMapper> m_storageMapper;

	/// Registry of library function IDs, populated during the library translation pass.
	LibraryFunctionIdMap m_libraryFunctionIds;

	/// Maps free function AST ID → subroutine ID, for operator overload resolution.
	std::unordered_map<int64_t, std::string> m_freeFunctionById;
};

} // namespace puyasol::builder
