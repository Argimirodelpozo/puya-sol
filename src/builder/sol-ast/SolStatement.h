#pragma once

#include "awst/Node.h"

#include <libsolidity/ast/AST.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace puyasol::builder
{
class TypeMapper;
}

namespace puyasol::builder::eb
{
class BuilderContext;
}

namespace puyasol::builder::sol_ast
{

/// Context passed to statement wrappers — provides access to expression building,
/// type mapping, and recursive statement/block building back through the caller.
struct StatementContext
{
	eb::BuilderContext* exprBuilder;
	TypeMapper* typeMapper;
	std::string sourceFile;

	/// Build a child expression (delegates to BuilderContext).
	std::function<std::shared_ptr<awst::Expression>(
		solidity::frontend::Expression const&)> buildExpr;

	/// Build a child statement (delegates to the same dispatcher).
	std::function<std::shared_ptr<awst::Statement>(
		solidity::frontend::Statement const&)> buildStmt;

	/// Build a child block (delegates to the same dispatcher).
	std::function<std::shared_ptr<awst::Block>(
		solidity::frontend::Block const&)> buildBlock;

	/// Flush pre-pending and post-pending statements from BuilderContext.
	std::function<std::vector<std::shared_ptr<awst::Statement>>()> takePrePending;
	std::function<std::vector<std::shared_ptr<awst::Statement>>()> takePending;

	// ── Function context (for inline assembly) ──
	std::vector<std::pair<std::string, awst::WType const*>> functionParams;
	awst::WType const* returnType = nullptr;
	std::map<std::string, unsigned> functionParamBitWidths;

	// ── Modifier placeholder body ──
	std::shared_ptr<awst::Block> placeholderBody;

	// ── Control flow state (mutable, shared across nested statements) ──
	std::shared_ptr<awst::Statement> forLoopPost;
	std::shared_ptr<awst::Statement> doWhileCondBreak;

	/// Create an AWST SourceLocation.
	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _solLoc) const;
};

/// Base class for Solidity statement wrappers.
class SolStatement
{
public:
	virtual ~SolStatement() = default;

	/// Translate this statement to AWST statements.
	/// May produce multiple statements (e.g., pending statement flushing).
	virtual std::vector<std::shared_ptr<awst::Statement>> toAwst() = 0;

protected:
	SolStatement(StatementContext& _ctx, awst::SourceLocation _loc)
		: m_ctx(_ctx), m_loc(std::move(_loc)) {}

	StatementContext& m_ctx;
	awst::SourceLocation m_loc;
};

} // namespace puyasol::builder::sol_ast
