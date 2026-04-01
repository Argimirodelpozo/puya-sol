/// @file SolBlock.cpp
/// Block statement — the central statement dispatcher.
/// Replaces StatementBuilder as the primary way to build AWST from Solidity statements.

#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-ast/stmts/SolExpressionStatement.h"
#include "builder/sol-ast/stmts/SolControlFlow.h"
#include "builder/sol-ast/stmts/SolEmitStatement.h"
#include "builder/sol-ast/stmts/SolVariableDeclaration.h"
#include "builder/sol-ast/stmts/SolInlineAssembly.h"
#include "builder/ExpressionBuilder.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

SolBlock::SolBlock(
	StatementContext& _ctx,
	Block const& _node,
	awst::SourceLocation _loc,
	ExpressionBuilder& _exprBuilder)
	: SolStatement(_ctx, std::move(_loc)), m_block(_node), m_exprBuilder(_exprBuilder)
{
}

/// Dispatch a single Solidity statement to the right sol-ast wrapper (free function).
static std::vector<std::shared_ptr<awst::Statement>> dispatchStatementImpl(
	StatementContext& _ctx,
	ExpressionBuilder& _exprBuilder,
	Statement const& _stmt)
{
	auto loc = _ctx.makeLoc(_stmt.location());

	if (auto const* node = dynamic_cast<ExpressionStatement const*>(&_stmt))
	{
		SolExpressionStatement handler(_ctx, *node, loc);
		return handler.toAwst();
	}
	if (auto const* node = dynamic_cast<Return const*>(&_stmt))
	{
		SolReturnStatement handler(_ctx, *node, loc);
		return handler.toAwst();
	}
	if (auto const* node = dynamic_cast<RevertStatement const*>(&_stmt))
	{
		SolRevertStatement handler(_ctx, *node, loc);
		return handler.toAwst();
	}
	if (auto const* node = dynamic_cast<EmitStatement const*>(&_stmt))
	{
		SolEmitStatement handler(_ctx, *node, loc);
		return handler.toAwst();
	}
	if (auto const* node = dynamic_cast<VariableDeclarationStatement const*>(&_stmt))
	{
		SolVariableDeclaration handler(_ctx, *node, loc, _exprBuilder);
		return handler.toAwst();
	}
	if (auto const* node = dynamic_cast<IfStatement const*>(&_stmt))
	{
		SolIfStatement handler(_ctx, *node, loc);
		return handler.toAwst();
	}
	if (auto const* node = dynamic_cast<WhileStatement const*>(&_stmt))
	{
		SolWhileStatement handler(_ctx, *node, loc);
		return handler.toAwst();
	}
	if (auto const* node = dynamic_cast<ForStatement const*>(&_stmt))
	{
		SolForStatement handler(_ctx, *node, loc);
		return handler.toAwst();
	}
	if (auto const* node = dynamic_cast<InlineAssembly const*>(&_stmt))
	{
		SolInlineAssembly handler(_ctx, *node, loc,
			_ctx.functionParams, _ctx.returnType, _ctx.functionParamBitWidths);
		return handler.toAwst();
	}
	if (dynamic_cast<Continue const*>(&_stmt))
	{
		if (_ctx.forLoopPost)
		{
			auto block = std::make_shared<awst::Block>();
			block->sourceLocation = loc;
			block->body.push_back(_ctx.forLoopPost);
			block->body.push_back(std::make_shared<awst::LoopContinue>());
			return {block};
		}
		if (_ctx.doWhileCondBreak)
		{
			auto block = std::make_shared<awst::Block>();
			block->sourceLocation = loc;
			block->body.push_back(_ctx.doWhileCondBreak);
			block->body.push_back(std::make_shared<awst::LoopContinue>());
			return {block};
		}
		auto stmt = std::make_shared<awst::LoopContinue>();
		stmt->sourceLocation = loc;
		return {stmt};
	}
	if (dynamic_cast<Break const*>(&_stmt))
	{
		auto stmt = std::make_shared<awst::LoopExit>();
		stmt->sourceLocation = loc;
		return {stmt};
	}
	if (dynamic_cast<PlaceholderStatement const*>(&_stmt))
	{
		if (_ctx.placeholderBody)
		{
			auto block = std::make_shared<awst::Block>();
			block->sourceLocation = loc;
			for (auto const& s: _ctx.placeholderBody->body)
				block->body.push_back(s);
			return {block};
		}
		return {};
	}
	if (dynamic_cast<TryStatement const*>(&_stmt))
	{
		Logger::instance().error(
			"try/catch is not supported on AVM. "
			"AVM errors are not catchable — they abort the entire transaction group.", loc);
		return {};
	}
	if (auto const* node = dynamic_cast<Block const*>(&_stmt))
	{
		SolBlock handler(_ctx, *node, _ctx.makeLoc(node->location()), _exprBuilder);
		return handler.toAwst();
	}

	Logger::instance().warning("unhandled statement type", loc);
	return {};
}

std::shared_ptr<awst::Block> SolBlock::toAwstBlock()
{
	auto awstBlock = std::make_shared<awst::Block>();
	awstBlock->sourceLocation = m_loc;

	// Every block creates a scope — mutable context state (funcPtrTargets,
	// storageAliases, constantLocals) is snapshotted and restored on exit.
	auto scope = m_exprBuilder.pushScope();

	bool const wasUnchecked = m_exprBuilder.inUncheckedBlock();
	if (m_block.unchecked())
		m_exprBuilder.setInUncheckedBlock(true);

	for (auto const& stmt: m_block.statements())
	{
		if (auto const* innerBlock = dynamic_cast<Block const*>(stmt.get()))
		{
			// Flatten nested blocks (including unchecked blocks)
			SolBlock handler(m_ctx, *innerBlock,
				m_ctx.makeLoc(innerBlock->location()), m_exprBuilder);
			auto translated = handler.toAwstBlock();
			for (auto& s: translated->body)
				awstBlock->body.push_back(std::move(s));
		}
		else
		{
			for (auto& s: dispatchStatementImpl(m_ctx, m_exprBuilder, *stmt))
				if (s) awstBlock->body.push_back(std::move(s));
		}
	}

	m_exprBuilder.setInUncheckedBlock(wasUnchecked);
	return awstBlock;
}

std::vector<std::shared_ptr<awst::Statement>> SolBlock::toAwst()
{
	return {toAwstBlock()};
}

// ── Free functions ──

std::shared_ptr<awst::Statement> buildStatement(
	StatementContext& _ctx,
	ExpressionBuilder& _exprBuilder,
	solidity::frontend::Statement const& _stmt)
{
	if (auto const* block = dynamic_cast<Block const*>(&_stmt))
		return buildBlock(_ctx, _exprBuilder, *block);

	auto results = dispatchStatementImpl(_ctx, _exprBuilder, _stmt);
	if (results.size() == 1) return results[0];
	if (results.empty()) return nullptr;
	auto block = std::make_shared<awst::Block>();
	block->sourceLocation = _ctx.makeLoc(_stmt.location());
	for (auto& s: results)
		if (s) block->body.push_back(std::move(s));
	return block;
}

std::shared_ptr<awst::Block> buildBlock(
	StatementContext& _ctx,
	ExpressionBuilder& _exprBuilder,
	solidity::frontend::Block const& _block)
{
	auto loc = _ctx.makeLoc(_block.location());
	SolBlock handler(_ctx, _block, loc, _exprBuilder);
	return handler.toAwstBlock();
}

} // namespace puyasol::builder::sol_ast
