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

	auto cond = bc.buildExpr(m_node.condition());

	auto preEffects = bc.takePreEffects();
	auto postPending = bc.takePostEffects();

	auto buildBranch = [&](Statement const& body) -> std::shared_ptr<awst::Block> {
		// Conditionally-executed region: compile-time-only rebinds (storage
		// pointer aliases) must fail loud inside it.
		eb::ContractContext::ConditionalRegion region(bc);
		// A halt inside a branch (assembly return() → BlockContext.terminated)
		// must not leak out — the branch is conditional, so code after the if
		// is still reachable. Save/restore the flag around each branch.
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

	// BOTH pending kinds precede the IfElse: post-pendings carry effects of
	// EVALUATING the condition (internal-call storage/memory write-backs,
	// push/pop box writes) — they must complete before either branch runs.
	// Emitting them after the IfElse read pre-mutation state in the branches
	// and LOST the effect entirely when a branch returned/halted.
	for (auto& p: preEffects) result.push_back(std::move(p));
	for (auto& p: postPending) result.push_back(std::move(p));
	result.push_back(awst::makeIfElse(std::move(cond), std::move(ifBranch), std::move(elseBranch), m_loc));
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
	// Cond and body re-execute per iteration — a conditionally-executed
	// region for compile-time rebinds (storage-pointer aliases).
	eb::ContractContext::ConditionalRegion region(bc);

	if (m_node.isDoWhile())
	{
		auto body = awst::makeBlock(m_blk.makeLoc(m_node.body().location()));

		auto cond = bc.buildExpr(m_node.condition());
		// Capture the condition build's pendings NOW (bounds asserts, index
		// temps, write-backs): un-captured they were drained by the first
		// BODY statement — executing at the TOP of the body while the test
		// runs at the BOTTOM, one iteration apart (and leaking out of the
		// loop entirely for bodies that never drain). They re-run with the
		// test each iteration, bundled in one block so the `continue` splice
		// (doWhileCondBreak) carries them too.
		auto condPre = bc.takePreEffects();
		{ auto cp = bc.takePostEffects(); for (auto& p: cp) condPre.push_back(std::move(p)); }
		auto notCond = awst::makeNot(std::move(cond), m_loc);

		auto breakBlock = awst::makeBlock(m_loc);
		breakBlock->body.push_back(awst::makeLoopExit(m_loc));

		std::shared_ptr<awst::Statement> ifBreak =
			awst::makeIfElse(notCond, breakBlock, nullptr, m_loc);
		if (!condPre.empty())
		{
			auto testBlock = awst::makeBlock(m_loc);
			for (auto& p: condPre) testBlock->body.push_back(std::move(p));
			testBlock->body.push_back(std::move(ifBreak));
			ifBreak = std::move(testBlock);
		}

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
		auto cond = bc.buildExpr(m_node.condition());

		// Drain statements emitted while building the condition (e.g. a nested-array
		// `a[i].length` bounds-check) — same orphaning as the for-loop: a WhileLoop
		// condition is a pure expression, so they must re-run each iteration before the
		// test, else the condition reads an undefined temp and reverts.
		auto condPre = bc.takePreEffects();
		{ auto cp = bc.takePostEffects(); for (auto& p: cp) condPre.push_back(std::move(p)); }

		// Empty LoopContext (no for-post / doWhile break); still needed so
		// continue/break inside the body know they're in a loop.
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

		if (condPre.empty())
			return {awst::makeWhileLoop(std::move(cond), std::move(body), m_loc)};

		// while (true) { <cond-pre>; if (!cond) break; <body> }
		auto newBody = awst::makeBlock(m_loc);
		for (auto& p: condPre) newBody->body.push_back(std::move(p));
		auto breakBlk = awst::makeBlock(m_loc);
		breakBlk->body.push_back(awst::makeLoopExit(m_loc));
		newBody->body.push_back(
			awst::makeIfElse(awst::makeNot(std::move(cond), m_loc), breakBlk, nullptr, m_loc));
		for (auto& s: body->body) newBody->body.push_back(std::move(s));
		return {awst::makeWhileLoop(awst::makeTrue(m_loc), std::move(newBody), m_loc)};
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
	// Everything from the condition on (cond, post, body) re-executes per
	// iteration — a conditionally-executed region for compile-time rebinds.
	// The init above runs once, straight-line, and stays outside it.
	eb::ContractContext::ConditionalRegion region(bc);
	auto cond = m_node.condition()
		? bc.buildExpr(*m_node.condition())
		: std::shared_ptr<awst::Expression>(awst::makeTrue(m_loc));

	// Capture statements emitted while building the condition (e.g. the bounds-check
	// assert + index cache for a nested-array `a[i].length`). A WhileLoop condition is a
	// pure expression, so otherwise these leak into the loop BODY and run AFTER the test
	// that consumes them → the condition reads undefined temps and reverts. Run them each
	// iteration BEFORE the test (mirrors the do-while lowering below):
	//   while (true) { <cond-pre>; if (!cond) break; <body>; <post> }
	auto condPre = bc.takePreEffects();
	{ auto cp = bc.takePostEffects(); for (auto& p: cp) condPre.push_back(std::move(p)); }

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

	if (condPre.empty())
	{
		// No condition pre-statements: keep the direct `while (cond) { body }` form.
		outerBlock->body.push_back(
			awst::makeWhileLoop(std::move(cond), std::move(loopBody), m_loc));
	}
	else
	{
		// Re-evaluate the condition pre-statements + test each iteration before the body.
		auto newBody = awst::makeBlock(m_loc);
		for (auto& p: condPre) newBody->body.push_back(std::move(p));
		auto breakBlk = awst::makeBlock(m_loc);
		breakBlk->body.push_back(awst::makeLoopExit(m_loc));
		newBody->body.push_back(
			awst::makeIfElse(awst::makeNot(std::move(cond), m_loc), breakBlk, nullptr, m_loc));
		for (auto& s: loopBody->body) newBody->body.push_back(std::move(s));
		outerBlock->body.push_back(
			awst::makeWhileLoop(awst::makeTrue(m_loc), std::move(newBody), m_loc));
	}
	return {outerBlock};
}

} // namespace puyasol::builder::sol_ast
