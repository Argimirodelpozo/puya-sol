/// @file SolControlFlow.cpp
/// if/while/for control flow wrappers.
/// Loop bodies derive a LoopContext + BlockContext-with-loop, so
/// continue/break inside know which post-step / cond-break to splice.

#include "builder/sol-ast/stmts/SolControlFlow.h"
#include "builder/sol-eb/BuilderContext.h"
#include "Logger.h"

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

// ── IfStatement ──

SolIfStatement::SolIfStatement(
	BlockContext& _blk, IfStatement const& _node, awst::SourceLocation _loc)
	: SolStatement(_blk, std::move(_loc)), m_node(_node)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolIfStatement::toAwst()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	auto& bc = m_blk.builderCtx();

	auto stmt = std::make_shared<awst::IfElse>();
	stmt->sourceLocation = m_loc;
	stmt->condition = bc.build(m_node.condition());

	auto prePending = bc.takePrePending();
	auto postPending = bc.takePending();

	auto buildBranch = [&](Statement const& body) -> std::shared_ptr<awst::Block> {
		if (auto const* block = dynamic_cast<Block const*>(&body))
			return buildBlock(m_blk, *block);
		auto syntheticBlock = std::make_shared<awst::Block>();
		syntheticBlock->sourceLocation = m_blk.makeLoc(body.location());
		auto translated = buildStatement(m_blk, body);
		if (translated) syntheticBlock->body.push_back(std::move(translated));
		return syntheticBlock;
	};

	stmt->ifBranch = buildBranch(m_node.trueStatement());
	if (m_node.falseStatement())
		stmt->elseBranch = buildBranch(*m_node.falseStatement());

	for (auto& p: prePending) result.push_back(std::move(p));
	result.push_back(stmt);
	for (auto& p: postPending) result.push_back(std::move(p));
	return result;
}

// ── WhileStatement ──

SolWhileStatement::SolWhileStatement(
	BlockContext& _blk, WhileStatement const& _node, awst::SourceLocation _loc)
	: SolStatement(_blk, std::move(_loc)), m_node(_node)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolWhileStatement::toAwst()
{
	auto& bc = m_blk.builderCtx();

	if (m_node.isDoWhile())
	{
		auto loop = std::make_shared<awst::WhileLoop>();
		loop->sourceLocation = m_loc;
		loop->condition = awst::makeBoolConstant(true, m_loc);

		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = m_blk.makeLoc(m_node.body().location());

		auto cond = bc.build(m_node.condition());
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

		LoopContext loopCtx;
		loopCtx.doWhileCondBreak = ifBreak;
		auto bodyBlk = m_blk.withLoop(loopCtx);

		bool bodyTerminated = false;
		if (auto const* block = dynamic_cast<Block const*>(&m_node.body()))
		{
			for (auto const& stmt: block->statements())
			{
				auto translated = buildStatement(bodyBlk, *stmt);
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

		if (!bodyTerminated) body->body.push_back(ifBreak);
		loop->loopBody = body;
		return {loop};
	}
	else
	{
		auto loop = std::make_shared<awst::WhileLoop>();
		loop->sourceLocation = m_loc;
		loop->condition = bc.build(m_node.condition());

		// while-loop body: no special LoopContext data needed (no for-post,
		// no doWhile cond break) but we still create one so continue/break
		// in nested code knows it's inside a loop.
		LoopContext loopCtx;
		auto bodyBlk = m_blk.withLoop(loopCtx);

		if (auto const* block = dynamic_cast<Block const*>(&m_node.body()))
			loop->loopBody = buildBlock(bodyBlk, *block);
		else
		{
			auto body = std::make_shared<awst::Block>();
			body->sourceLocation = m_blk.makeLoc(m_node.body().location());
			auto translated = buildStatement(bodyBlk, m_node.body());
			if (translated) body->body.push_back(std::move(translated));
			loop->loopBody = body;
		}
		return {loop};
	}
}

// ── ForStatement ──

SolForStatement::SolForStatement(
	BlockContext& _blk, ForStatement const& _node, awst::SourceLocation _loc)
	: SolStatement(_blk, std::move(_loc)), m_node(_node)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolForStatement::toAwst()
{
	auto outerBlock = std::make_shared<awst::Block>();
	outerBlock->sourceLocation = m_loc;

	if (m_node.initializationExpression())
	{
		auto init = buildStatement(m_blk, *m_node.initializationExpression());
		if (init) outerBlock->body.push_back(std::move(init));
	}

	auto loop = std::make_shared<awst::WhileLoop>();
	loop->sourceLocation = m_loc;

	auto& bc = m_blk.builderCtx();
	if (m_node.condition())
		loop->condition = bc.build(*m_node.condition());
	else
		loop->condition = awst::makeBoolConstant(true, m_loc);

	std::shared_ptr<awst::Statement> postStmt;
	if (m_node.loopExpression())
		postStmt = buildStatement(m_blk, *m_node.loopExpression());

	LoopContext loopCtx;
	loopCtx.forLoopPost = postStmt;
	auto bodyBlk = m_blk.withLoop(loopCtx);

	auto loopBody = std::make_shared<awst::Block>();
	loopBody->sourceLocation = m_loc;

	if (auto const* block = dynamic_cast<Block const*>(&m_node.body()))
	{
		for (auto const& stmt: block->statements())
		{
			auto translated = buildStatement(bodyBlk, *stmt);
			if (translated) loopBody->body.push_back(std::move(translated));
		}
	}
	else
	{
		auto translated = buildStatement(bodyBlk, m_node.body());
		if (translated) loopBody->body.push_back(std::move(translated));
	}

	if (postStmt) loopBody->body.push_back(postStmt);

	loop->loopBody = loopBody;
	outerBlock->body.push_back(loop);
	return {outerBlock};
}

} // namespace puyasol::builder::sol_ast
