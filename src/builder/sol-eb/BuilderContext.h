#pragma once

#include "awst/Node.h"
#include "awst/WType.h"

#include <libsolidity/ast/Types.h>
#include <liblangutil/Token.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace solidity::frontend
{
class Expression;
class FunctionDefinition;
class Declaration;
}

namespace puyasol::builder
{
class TypeMapper;
class StorageMapper;
}

namespace puyasol::builder::eb
{

class InstanceBuilder;

/// Parameter remap entry: redirects an AST declaration to a unique variable name.
struct ParamRemap
{
	std::string name;
	awst::WType const* type;
};

/// Shared context passed to all expression builders by reference.
///
/// This struct provides builders access to the compiler state they need
/// without requiring them to depend on ExpressionBuilder directly.
/// During the migration, this is populated from ExpressionBuilder's members.
struct BuilderContext
{
	// ── Compiler services ──
	TypeMapper& typeMapper;
	StorageMapper& storageMapper;
	std::string const& sourceFile;
	std::string const& contractName;

	// ── Function resolution tables ──
	std::unordered_map<std::string, std::string> const& libraryFunctionIds;
	std::unordered_set<std::string> const& overloadedNames;
	std::unordered_map<int64_t, std::string> const& freeFunctionById;

	// ── Side-effect statement buffers (shared with ExpressionBuilder) ──
	std::vector<std::shared_ptr<awst::Statement>>& pendingStatements;
	std::vector<std::shared_ptr<awst::Statement>>& prePendingStatements;

	// ── Expression builder state (passed by reference) ──
	std::map<int64_t, ParamRemap>& paramRemaps;
	std::unordered_map<int64_t, std::string>& superTargetNames;
	std::map<int64_t, std::shared_ptr<awst::Expression>>& storageAliases;
	std::map<int64_t, std::shared_ptr<awst::Expression>>& slotStorageRefs;
	std::map<int64_t, solidity::frontend::FunctionDefinition const*>& funcPtrTargets;
	std::unordered_map<int64_t, unsigned long long>& constantLocals;
	std::map<std::string, int64_t>& varNameToId;
	bool inConstructor;
	bool inUncheckedBlock;

	// ── Recursive build callback (delegates back to ExpressionBuilder) ──
	/// Build a child Solidity expression into an AWST Expression.
	std::function<std::shared_ptr<awst::Expression>(
		solidity::frontend::Expression const&)> buildExpr;

	// ── Binary/unary operation callbacks (delegates back to ExpressionBuilder) ──
	/// Build a binary operation from already-resolved operands (fallback when sol-eb builders don't handle it).
	std::function<std::shared_ptr<awst::Expression>(
		solidity::frontend::Token, std::shared_ptr<awst::Expression>,
		std::shared_ptr<awst::Expression>, awst::WType const*,
		awst::SourceLocation const&)> buildBinaryOp;

	// ── Builder factory callback ──
	/// Get a type-specific InstanceBuilder for an already-resolved expression.
	/// Returns nullptr if no builder is registered for the Solidity type.
	std::function<std::unique_ptr<InstanceBuilder>(
		solidity::frontend::Type const*, std::shared_ptr<awst::Expression>)> builderForInstance;

	// ── Source location helper ──
	/// Create an AWST SourceLocation from file + offset range.
	awst::SourceLocation makeLoc(int _start, int _end) const
	{
		awst::SourceLocation loc;
		loc.file = sourceFile;
		loc.line = _start >= 0 ? _start : 0;
		loc.endLine = _end >= 0 ? _end : 0;
		return loc;
	}
};

} // namespace puyasol::builder::eb
