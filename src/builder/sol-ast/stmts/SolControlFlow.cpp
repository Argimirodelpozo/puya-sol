/// @file SolControlFlow.cpp
/// if/while/for control flow wrappers.
/// Uses StatementContext fields for loop state (forLoopPost, doWhileCondBreak).

#include "builder/sol-ast/stmts/SolControlFlow.h"
#include "Logger.h"

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

// ── IfStatement ──

SolIfStatement::SolIfStatement(
	StatementContext& _ctx, IfStatement const& _node, awst::SourceLocation _loc)
	: SolStatement(_ctx, std::move(_loc)), m_node(_node)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolIfStatement::toAwst()
{
	std::vector<std::shared_ptr<awst::Statement>> result;

	auto stmt = std::make_shared<awst::IfElse>();
	stmt->sourceLocation = m_loc;
	stmt->condition = m_ctx.buildExpr(m_node.condition());

	auto prePending = m_ctx.takePrePending();
	auto postPending = m_ctx.takePending();

	auto const& trueBody = m_node.trueStatement();
	if (auto const* block = dynamic_cast<Block const*>(&trueBody))
		stmt->ifBranch = m_ctx.buildBlock(*block);
	else
	{
		auto syntheticBlock = std::make_shared<awst::Block>();
		syntheticBlock->sourceLocation = m_ctx.makeLoc(trueBody.location());
		auto translated = m_ctx.buildStmt(trueBody);
		if (translated) syntheticBlock->body.push_back(std::move(translated));
		stmt->ifBranch = syntheticBlock;
	}

	if (m_node.falseStatement())
	{
		auto const& falseBody = *m_node.falseStatement();
		if (auto const* block = dynamic_cast<Block const*>(&falseBody))
			stmt->elseBranch = m_ctx.buildBlock(*block);
		else
		{
			auto syntheticBlock = std::make_shared<awst::Block>();
			syntheticBlock->sourceLocation = m_ctx.makeLoc(falseBody.location());
			auto translated = m_ctx.buildStmt(falseBody);
			if (translated) syntheticBlock->body.push_back(std::move(translated));
			stmt->elseBranch = syntheticBlock;
		}
	}

	for (auto& p: prePending) result.push_back(std::move(p));
	result.push_back(stmt);
	for (auto& p: postPending) result.push_back(std::move(p));
	return result;
}

// ── WhileStatement ──

SolWhileStatement::SolWhileStatement(
	StatementContext& _ctx, WhileStatement const& _node, awst::SourceLocation _loc)
	: SolStatement(_ctx, std::move(_loc)), m_node(_node)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolWhileStatement::toAwst()
{
	if (m_node.isDoWhile())
	{
		auto loop = std::make_shared<awst::WhileLoop>();
		loop->sourceLocation = m_loc;
		loop->condition = awst::makeBoolConstant(true, m_loc);

		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = m_ctx.makeLoc(m_node.body().location());

		auto cond = m_ctx.buildExpr(m_node.condition());
		auto notCond = std::make_shared<awst::Not>();
		notCond->sourceLocation = m_loc;
		notCond->wtype = awst::WType::boolType();
		notCond->expr = std::move(cond);

		auto breakBlock = std::make_shared<awst::Block>();
		breakBlock->sourceLocation = m_loc;
		breakBlock->body.push_back(std::make_shared<awst::LoopExit>());

		auto ifBreak = std::make_shared<awst::IfElse>();
		ifBreak->sourceLocation = m_loc;
		ifBreak->condition = notCond;
		ifBreak->ifBranch = breakBlock;

		auto savedCondBreak = m_ctx.doWhileCondBreak;
		m_ctx.doWhileCondBreak = ifBreak;

		bool bodyTerminated = false;
		if (auto const* block = dynamic_cast<Block const*>(&m_node.body()))
		{
			for (auto const& stmt: block->statements())
			{
				auto translated = m_ctx.buildStmt(*stmt);
				if (translated)
				{
					body->body.push_back(std::move(translated));
					auto const& last = body->body.back();
					if (dynamic_cast<awst::LoopContinue const*>(last.get())
						|| dynamic_cast<awst::LoopExit const*>(last.get())
						|| dynamic_cast<awst::ReturnStatement const*>(last.get()))
					{ bodyTerminated = true; break; }
					if (auto const* blk = dynamic_cast<awst::Block const*>(last.get()))
						if (!blk->body.empty())
						{
							auto const& lb = blk->body.back();
							if (dynamic_cast<awst::LoopContinue const*>(lb.get())
								|| dynamic_cast<awst::LoopExit const*>(lb.get())
								|| dynamic_cast<awst::ReturnStatement const*>(lb.get()))
							{ bodyTerminated = true; break; }
						}
				}
			}
		}

		m_ctx.doWhileCondBreak = savedCondBreak;
		if (!bodyTerminated) body->body.push_back(ifBreak);
		loop->loopBody = body;
		return {loop};
	}
	else
	{
		auto loop = std::make_shared<awst::WhileLoop>();
		loop->sourceLocation = m_loc;
		loop->condition = m_ctx.buildExpr(m_node.condition());

		if (auto const* block = dynamic_cast<Block const*>(&m_node.body()))
			loop->loopBody = m_ctx.buildBlock(*block);
		else
		{
			auto body = std::make_shared<awst::Block>();
			body->sourceLocation = m_ctx.makeLoc(m_node.body().location());
			auto translated = m_ctx.buildStmt(m_node.body());
			if (translated) body->body.push_back(std::move(translated));
			loop->loopBody = body;
		}
		return {loop};
	}
}

// ── ForStatement ──

SolForStatement::SolForStatement(
	StatementContext& _ctx, ForStatement const& _node, awst::SourceLocation _loc)
	: SolStatement(_ctx, std::move(_loc)), m_node(_node)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolForStatement::toAwst()
{
	auto outerBlock = std::make_shared<awst::Block>();
	outerBlock->sourceLocation = m_loc;

	if (m_node.initializationExpression())
	{
		auto init = m_ctx.buildStmt(*m_node.initializationExpression());
		if (init) outerBlock->body.push_back(std::move(init));
	}

	auto loop = std::make_shared<awst::WhileLoop>();
	loop->sourceLocation = m_loc;

	if (m_node.condition())
		loop->condition = m_ctx.buildExpr(*m_node.condition());
	else
	{
		loop->condition = awst::makeBoolConstant(true, m_loc);
	}

	std::shared_ptr<awst::Statement> postStmt;
	if (m_node.loopExpression())
		postStmt = m_ctx.buildStmt(*m_node.loopExpression());

	auto savedPost = m_ctx.forLoopPost;
	m_ctx.forLoopPost = postStmt;

	auto loopBody = std::make_shared<awst::Block>();
	loopBody->sourceLocation = m_loc;

	if (auto const* block = dynamic_cast<Block const*>(&m_node.body()))
	{
		for (auto const& stmt: block->statements())
		{
			auto translated = m_ctx.buildStmt(*stmt);
			if (translated) loopBody->body.push_back(std::move(translated));
		}
	}
	else
	{
		auto translated = m_ctx.buildStmt(m_node.body());
		if (translated) loopBody->body.push_back(std::move(translated));
	}

	m_ctx.forLoopPost = savedPost;
	if (postStmt) loopBody->body.push_back(postStmt);

	loop->loopBody = loopBody;
	outerBlock->body.push_back(loop);
	return {outerBlock};
}

} // namespace puyasol::builder::sol_ast
