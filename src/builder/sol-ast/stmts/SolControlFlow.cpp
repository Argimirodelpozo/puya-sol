/// @file SolControlFlow.cpp
/// if/while/for control flow wrappers.
/// Loop bodies derive a LoopContext + BlockContext-with-loop, so
/// continue/break inside know which post-step / cond-break to splice.

#include "builder/sol-ast/stmts/SolControlFlow.h"
#include "builder/sol-eb/ContractContext.h"

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

	auto cond = bc.build(m_node.condition());

	auto prePending = bc.takePrePending();
	auto postPending = bc.takePending();

	auto buildBranch = [&](Statement const& body) -> std::shared_ptr<awst::Block> {
		// Branches share the parent BlockContext, so a program halt inside
		// the branch (assembly return() → BlockContext.terminated) must not
		// leak out: the branch is CONDITIONAL — statements after the if are
		// still reachable. The flag does its work within the branch's own
		// buildBlock (skipping trailing branch statements), then the parent
		// value is restored. (Bare nested blocks keep propagating — they
		// execute unconditionally. Loop bodies derive their own context via
		// withLoop and never leak.)
		bool parentTerminated = m_blk.terminated;
		std::shared_ptr<awst::Block> branch;
		if (auto const* block = dynamic_cast<Block const*>(&body))
			branch = buildBlock(m_blk, *block);
		else
		{
			branch = awst::makeBlock(m_blk.makeLoc(body.location()));
			auto translated = buildStatement(m_blk, body);
			if (translated) branch->body.push_back(std::move(translated));
		}
		m_blk.terminated = parentTerminated;
		return branch;
	};

	auto ifBranch = buildBranch(m_node.trueStatement());
	auto elseBranch = m_node.falseStatement()
		? buildBranch(*m_node.falseStatement())
		: nullptr;

	for (auto& p: prePending) result.push_back(std::move(p));
	result.push_back(awst::makeIfElse(std::move(cond), std::move(ifBranch), std::move(elseBranch), m_loc));
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
		auto body = awst::makeBlock(m_blk.makeLoc(m_node.body().location()));

		auto cond = bc.build(m_node.condition());
		auto notCond = awst::makeNot(std::move(cond), m_loc);

		auto breakBlock = awst::makeBlock(m_loc);
		breakBlock->body.push_back(awst::makeLoopExit(m_loc));

		auto ifBreak = awst::makeIfElse(notCond, breakBlock, nullptr, m_loc);

		LoopContext loopCtx;
		loopCtx.doWhileCondBreak = ifBreak;
		auto bodyBlk = m_blk.withLoop(loopCtx);
		auto blkGuard = m_blk.builderCtx().pushScopeRaii(&bodyBlk);

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
		return {awst::makeWhileLoop(
			awst::makeTrue(m_loc), std::move(body), m_loc)};
	}
	else
	{
		auto cond = bc.build(m_node.condition());

		// while-loop body: no special LoopContext data needed (no for-post,
		// no doWhile cond break) but we still create one so continue/break
		// in nested code knows it's inside a loop.
		LoopContext loopCtx;
		auto bodyBlk = m_blk.withLoop(loopCtx);
		auto blkGuard = m_blk.builderCtx().pushScopeRaii(&bodyBlk);

		std::shared_ptr<awst::Block> body;
		if (auto const* block = dynamic_cast<Block const*>(&m_node.body()))
			body = buildBlock(bodyBlk, *block);
		else
		{
			body = awst::makeBlock(m_blk.makeLoc(m_node.body().location()));
			auto translated = buildStatement(bodyBlk, m_node.body());
			if (translated) body->body.push_back(std::move(translated));
		}
		return {awst::makeWhileLoop(std::move(cond), std::move(body), m_loc)};
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
	auto outerBlock = awst::makeBlock(m_loc);

	if (m_node.initializationExpression())
	{
		auto init = buildStatement(m_blk, *m_node.initializationExpression());
		if (init) outerBlock->body.push_back(std::move(init));
	}

	auto& bc = m_blk.builderCtx();
	auto cond = m_node.condition()
		? bc.build(*m_node.condition())
		: std::shared_ptr<awst::Expression>(awst::makeTrue(m_loc));

	std::shared_ptr<awst::Statement> postStmt;
	if (m_node.loopExpression())
		postStmt = buildStatement(m_blk, *m_node.loopExpression());

	LoopContext loopCtx;
	loopCtx.forLoopPost = postStmt;
	auto bodyBlk = m_blk.withLoop(loopCtx);
	auto blkGuard = m_blk.builderCtx().pushScopeRaii(&bodyBlk);

	auto loopBody = awst::makeBlock(m_loc);

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

	outerBlock->body.push_back(
		awst::makeWhileLoop(std::move(cond), std::move(loopBody), m_loc));
	return {outerBlock};
}

} // namespace puyasol::builder::sol_ast
