#pragma once

#include "awst/Node.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace puyasol::splitter
{

/// Recursively inlines all subroutine calls in a target function's body,
/// producing one monolithic function with no internal calls.
/// This enables FunctionSplitter to split the result into sequential chunks
/// forming a linear pipeline (for group transaction execution on AVM).
///
/// Handles both SubroutineID targets (library/free functions) and
/// InstanceMethodTarget targets (other contract methods).
class FunctionInliner
{
public:
	struct InlineResult
	{
		bool didInline = false;
		std::set<std::string> inlinedFunctions;
		size_t totalStatements = 0;
	};

	/// Inline all calls in the named contract methods.
	/// Produces monolithic Subroutine bodies in-place on the contract methods,
	/// then creates matching Subroutine root nodes so FunctionSplitter can split them.
	InlineResult inlineAll(
		std::set<std::string> const& _targetNames,
		std::shared_ptr<awst::Contract> _contract,
		std::vector<std::shared_ptr<awst::RootNode>>& _roots
	);

private:
	/// A unified callable: args + body + return type (from either Subroutine or ContractMethod).
	struct Callable
	{
		std::string name;
		std::vector<awst::SubroutineArgument> args;
		awst::WType const* returnType = nullptr;
		awst::Block const* body = nullptr;
	};

	/// Build maps for callable lookup.
	void buildCallableMaps(
		std::shared_ptr<awst::Contract> _contract,
		std::vector<std::shared_ptr<awst::RootNode>> const& _roots
	);

	/// Resolve a SubroutineCallExpression to a Callable.
	Callable const* resolveCall(awst::SubroutineCallExpression const& _call);

	/// Recursively inline all calls in a block. Returns true if any inlining occurred.
	bool inlineBlock(
		awst::Block& _block,
		std::set<std::string>& _inlineStack,
		int _depth
	);

	/// Inline a single call. Returns the statements to replace the call with.
	std::vector<std::shared_ptr<awst::Statement>> inlineCall(
		awst::SubroutineCallExpression const& _call,
		Callable const& _callee,
		std::shared_ptr<awst::Expression> _assignTarget,
		std::set<std::string>& _inlineStack,
		int _depth
	);

	/// Variable renaming.
	void renameVarsInStmts(
		std::vector<std::shared_ptr<awst::Statement>>& _stmts,
		std::string const& _prefix,
		std::set<std::string> const& _localNames
	);
	void renameVarsInExpr(
		std::shared_ptr<awst::Expression>& _expr,
		std::string const& _prefix,
		std::set<std::string> const& _localNames
	);
	void renameVarsInStmt(
		std::shared_ptr<awst::Statement>& _stmt,
		std::string const& _prefix,
		std::set<std::string> const& _localNames
	);

	/// Flatten nested SubroutineCallExpressions into temp variable assignments.
	/// e.g., `x = add(mul(a,b), c)` → `__tmp0 = mul(a,b); x = add(__tmp0, c)`
	bool flattenBlock(awst::Block& _block);
	void flattenExpr(
		std::shared_ptr<awst::Expression>& _expr,
		std::vector<std::shared_ptr<awst::Statement>>& _hoisted,
		awst::SourceLocation const& _loc
	);

	/// Deep-copy AWST nodes (prevents shared_ptr aliasing during rename).
	std::shared_ptr<awst::Expression> deepCopyExpr(std::shared_ptr<awst::Expression> const& _expr);
	std::shared_ptr<awst::Statement> deepCopyStmt(std::shared_ptr<awst::Statement> const& _stmt);
	std::shared_ptr<awst::Block> deepCopyBlock(std::shared_ptr<awst::Block> const& _block);

	/// Collect local variable names from a block.
	void collectLocalNames(awst::Block const& _block, std::set<std::string>& _names);
	void collectStmtDefs(awst::Statement const& _stmt, std::set<std::string>& _names);

	/// Callable maps.
	std::map<std::string, Callable> m_callableById;      // SubroutineID target → Callable
	std::map<std::string, Callable> m_callableByMember;   // InstanceMethodTarget memberName → Callable

	int m_inlineCounter = 0;
	int m_flattenCounter = 0;
	std::set<std::string> m_inlinedFunctions;
};

} // namespace puyasol::splitter
