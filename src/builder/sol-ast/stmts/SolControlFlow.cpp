/// @file SolControlFlow.cpp
/// if/while/for lowering with sequenced conditions and fresh continue prefixes.

#include "builder/sol-ast/stmts/SolControlFlow.h"
#include "awst/Termination.hpp"
#include "builder/sol-eb/ContractContext.h"
#include <libsolidity/ast/AST.h>
#include <iterator>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

namespace
{

/// Complete condition write-backs before entering a branch/body, retaining
/// the condition's value if those write-backs can change a referenced local.
std::shared_ptr<awst::Expression> lowerCondition(
	BlockContext& blk, Expression const* source,
	std::vector<std::shared_ptr<awst::Statement>>& effects, awst::SourceLocation const& loc)
{
	auto& bc = blk.builderCtx();
	auto condition = source ? bc.pinIfWriteBacks(bc.lower(*source, false), loc) : awst::makeTrue(loc);
	bc.appendEffectsTo(effects);
	return condition;
}

std::shared_ptr<awst::Block> loopTest(
	std::shared_ptr<awst::Expression> condition,
	std::vector<std::shared_ptr<awst::Statement>> effects, awst::SourceLocation const& loc)
{
	auto test = awst::makeBlock(loc);
	test->body = std::move(effects);
	auto exit = awst::makeBlock(loc);
	exit->body.push_back(awst::makeLoopExit(loc));
	test->body.push_back(awst::makeIfElse(
		awst::makeNot(std::move(condition), loc), std::move(exit), nullptr, loc));
	return test;
}

/// Effectful tests run at the top of EVERY iteration, including after continue.
std::shared_ptr<awst::Statement> lowerLoop(
	std::shared_ptr<awst::Expression> condition,
	std::vector<std::shared_ptr<awst::Statement>> effects,
	std::shared_ptr<awst::Block> body, awst::SourceLocation const& loc)
{
	if (!effects.empty())
	{
		auto test = loopTest(std::move(condition), std::move(effects), loc);
		body->body.insert(body->body.begin(),
			std::make_move_iterator(test->body.begin()), std::make_move_iterator(test->body.end()));
		condition = awst::makeTrue(loc);
	}
	return awst::makeWhileLoop(std::move(condition), std::move(body), loc);
}

} // namespace

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

	auto cond = lowerCondition(m_blk, &m_node.condition(), result, m_loc);

	auto buildBranch = [&](Statement const& body) -> std::shared_ptr<awst::Block> {
		// Conditionally-executed region: compile-time-only rebinds (storage
		// pointer aliases) must fail loud inside it.
		eb::ContractContext::ConditionalRegion region(bc);
		return buildBlock(m_blk, body);
	};

	auto ifBranch = buildBranch(m_node.trueStatement());
	auto elseBranch = m_node.falseStatement()
		? buildBranch(*m_node.falseStatement())
		: nullptr;

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
		LoopContext loopCtx{[&] {
			// A continue may be nested in unchecked; the test belongs to the
			// loop's original scope. Rebuild from solc so each site has fresh IDs.
			auto guard = bc.pushScopeRaii(&m_blk.scope);
			std::vector<std::shared_ptr<awst::Statement>> effects;
			auto cond = lowerCondition(m_blk, &m_node.condition(), effects, m_loc);
			return loopTest(std::move(cond), std::move(effects), m_loc);
		}};
		auto body = buildBlock(m_blk.withLoop(loopCtx), m_node.body());
		if (!awst::blockAlwaysTerminates(*body)) body->body.push_back(loopCtx.continuePrefix());
		return {awst::makeWhileLoop(
			awst::makeTrue(m_loc), std::move(body), m_loc)};
	}
	else
	{
		std::vector<std::shared_ptr<awst::Statement>> condPre;
		auto cond = lowerCondition(m_blk, &m_node.condition(), condPre, m_loc);

		// Empty LoopContext (no for-post / doWhile break); still needed so
		// continue/break inside the body know they're in a loop.
		LoopContext loopCtx;
		auto body = buildBlock(m_blk.withLoop(loopCtx), m_node.body());

		return {lowerLoop(std::move(cond), std::move(condPre), std::move(body), m_loc)};
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
	std::vector<std::shared_ptr<awst::Statement>> condPre;
	auto cond = lowerCondition(m_blk, m_node.condition(), condPre, m_loc);

	LoopContext loopCtx;
	if (auto const* post = m_node.loopExpression())
		loopCtx.continuePrefix = [&, post] {
			auto guard = bc.pushScopeRaii(&m_blk.scope);
			return buildStatement(m_blk, *post);
		};
	auto loopBody = buildBlock(m_blk.withLoop(loopCtx), m_node.body());
	if (loopCtx.continuePrefix && !awst::blockAlwaysTerminates(*loopBody))
		if (auto post = loopCtx.continuePrefix()) loopBody->body.push_back(std::move(post));

	outerBlock->body.push_back(lowerLoop(
		std::move(cond), std::move(condPre), std::move(loopBody), m_loc));
	return {outerBlock};
}

} // namespace puyasol::builder::sol_ast
