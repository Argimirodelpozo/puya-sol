#pragma once

/// @file SolStatement.h
/// Base class for Solidity statement wrappers + free-function entry points.
///
/// Statement wrappers take a `BlockContext&` (the narrowest scope they need
/// — parents, enclosing loop, modifier placeholder body all live there).
/// Children are translated by calling the free `buildStatement` /
/// `buildBlock` entry points, which construct a fresh visitor using the
/// passed (possibly derived) context.
///
/// Expression children go through `m_blk.builderCtx().build(...)` — the
/// expression layer hasn't been moved off BuilderContext yet.

#include "awst/Node.h"
#include "builder/sol-ast/Context.h"

#include <libsolidity/ast/AST.h>

#include <memory>
#include <vector>

namespace puyasol::builder::sol_ast
{

/// Base class for Solidity statement wrappers.
class SolStatement
{
public:
	virtual ~SolStatement() = default;

	/// Translate this statement to AWST. May expand to multiple AWST
	/// statements (e.g., when pre/post-pending statements are flushed).
	virtual std::vector<std::shared_ptr<awst::Statement>> toAwst() = 0;

protected:
	SolStatement(BlockContext& _blk, awst::SourceLocation _loc)
		: m_blk(_blk), m_loc(std::move(_loc)) {}

	BlockContext& m_blk;
	awst::SourceLocation m_loc;
};

/// Build a single Solidity statement, possibly producing multiple AWST
/// statements (e.g., pending statement flushes).
std::vector<std::shared_ptr<awst::Statement>> buildStatementMulti(
	BlockContext& _blk,
	solidity::frontend::Statement const& _stmt);

/// Convenience: build one statement, wrapping multi-result into a Block.
std::shared_ptr<awst::Statement> buildStatement(
	BlockContext& _blk,
	solidity::frontend::Statement const& _stmt);

/// Build a Solidity Block into an AWST Block.
std::shared_ptr<awst::Block> buildBlock(
	BlockContext& _blk,
	solidity::frontend::Block const& _block);

} // namespace puyasol::builder::sol_ast
