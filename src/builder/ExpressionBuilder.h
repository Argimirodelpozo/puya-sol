#pragma once

#include "awst/Node.h"
#include "builder/sol-eb/BuilderContext.h"
#include "builder/sol-eb/BuilderRegistry.h"
#include "builder/sol-eb/BuiltinCallables.h"
#include "builder/sol-eb/TypeConversions.h"
#include "builder/sol-ast/SolExpressionFactory.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-intrinsics/IntrinsicMapper.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace puyasol::builder
{

/// Maps "LibraryName.functionName" → subroutine ID string.
using LibraryFunctionIdMap = std::unordered_map<std::string, std::string>;

/// Maps AST node ID → subroutine ID for free functions (used by operator overloading).
using FreeFunctionIdMap = std::unordered_map<int64_t, std::string>;

/// Set of function names that have overloads (multiple definitions with same name).
using OverloadedNamesSet = std::unordered_set<std::string>;

/// Builds AWST Expression nodes from Solidity AST expressions.
///
/// Owns an `eb::BuilderContext` which holds all per-translation mutable state
/// (scope tables, pending-statement buffers, parameter remaps, etc.). Most of
/// this class is thin forwarders to `m_ctx`; over the course of the migration
/// these forwarders will move to `BuilderContext` directly and this class will
/// be deleted.
class ExpressionBuilder
{
public:
	ExpressionBuilder(
		TypeMapper& _typeMapper,
		StorageMapper& _storageMapper,
		std::string const& _sourceFile,
		std::string const& _contractName,
		LibraryFunctionIdMap const& _libraryFunctionIds,
		OverloadedNamesSet const& _overloadedNames = {},
		FreeFunctionIdMap const& _freeFunctionById = {}
	);

	static LibraryFunctionIdMap const s_emptyLibraryFunctionIds;
	static FreeFunctionIdMap const s_emptyFreeFunctionIds;

	/// Build an AWST expression from a Solidity expression.
	std::shared_ptr<awst::Expression> build(solidity::frontend::Expression const& _expr);

	/// Insert implicit numeric cast if needed (e.g. uint64 → biguint).
	static std::shared_ptr<awst::Expression> implicitNumericCast(
		std::shared_ptr<awst::Expression> _expr,
		awst::WType const* _targetType,
		awst::SourceLocation const& _loc
	);

	/// Get the embedded BuilderContext. Used by external code (e.g. ContractBuilder
	/// dispatcher generation) that needs to pass a BuilderContext to static helpers.
	eb::BuilderContext& builderContext() { return m_ctx; }

	/// Consume any pending statements generated during expression translation.
	std::vector<std::shared_ptr<awst::Statement>> takePendingStatements();

	/// Consume any pre-pending statements (must execute before the expression).
	std::vector<std::shared_ptr<awst::Statement>> takePrePendingStatements();

private:
	/// The owned builder context: holds all mutable per-translation state and
	/// the type-builder registry. EB is now a thin wrapper that exists only to
	/// preserve the legacy construction signature used by AWSTBuilder /
	/// ContractBuilder.
	eb::BuilderContext m_ctx;

	LibraryFunctionIdMap const& m_libraryFunctionIds;
	OverloadedNamesSet const& m_overloadedNames;
	FreeFunctionIdMap const& m_freeFunctionById;
};

} // namespace puyasol::builder
