/// @file SolBlock.cpp
/// Block statement and SolStatementVisitor — central statement dispatcher.

#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-ast/SolASTVisitor.h"
#include "builder/sol-ast/stmts/SolExpressionStatement.h"
#include "builder/sol-ast/stmts/SolControlFlow.h"
#include "builder/sol-ast/stmts/SolEmitStatement.h"
#include "builder/sol-ast/stmts/SolVariableDeclaration.h"
#include "builder/sol-ast/stmts/SolInlineAssembly.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/sol-types/TypeMapper.h"
#include "awst/Clone.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

SolBlock::SolBlock(
	BlockContext& _blk,
	Block const& _node,
	awst::SourceLocation _loc)
	: SolStatement(_blk, std::move(_loc)), m_block(_node)
{
}

namespace
{

/// Translates Solidity statements into AWST. Holds the BlockContext
/// (enclosing loop, modifier placeholder body, parent chain).
class SolStatementVisitor: public SolASTVisitor<std::vector<std::shared_ptr<awst::Statement>>>
{
public:
	explicit SolStatementVisitor(BlockContext& _blk): m_blk(_blk) {}

	using ResultT = std::vector<std::shared_ptr<awst::Statement>>;

	ResultT visitExprStatement(ExpressionStatement const& _n) override
	{
		SolExpressionStatement handler(m_blk, _n, locOf(_n));
		return handler.toAwst();
	}

	ResultT visitReturn(Return const& _n) override
	{
		SolReturnStatement handler(m_blk, _n, locOf(_n));
		return handler.toAwst();
	}

	ResultT visitRevert(RevertStatement const& _n) override
	{
		SolRevertStatement handler(m_blk, _n, locOf(_n));
		return handler.toAwst();
	}

	ResultT visitEmit(EmitStatement const& _n) override
	{
		SolEmitStatement handler(m_blk, _n, locOf(_n));
		return handler.toAwst();
	}

	ResultT visitVarDecl(VariableDeclarationStatement const& _n) override
	{
		SolVariableDeclaration handler(m_blk, _n, locOf(_n));
		return handler.toAwst();
	}

	ResultT visitIfStatement(IfStatement const& _n) override
	{
		SolIfStatement handler(m_blk, _n, locOf(_n));
		return handler.toAwst();
	}

	ResultT visitWhile(WhileStatement const& _n) override
	{
		SolWhileStatement handler(m_blk, _n, locOf(_n));
		return handler.toAwst();
	}

	ResultT visitFor(ForStatement const& _n) override
	{
		SolForStatement handler(m_blk, _n, locOf(_n));
		return handler.toAwst();
	}

	ResultT visitInlineAssembly(InlineAssembly const& _n) override
	{
		SolInlineAssembly handler(m_blk, _n, locOf(_n));
		return handler.toAwst();
	}

	ResultT visitContinue(Continue const& _n) override
	{
		auto loc = locOf(_n);
		auto const* loop = m_blk.enclosingLoop;
		if (loop && loop->forLoopPost)
		{
			auto block = awst::makeBlock(loc);
			block->body.push_back(loop->forLoopPost);
			block->body.push_back(awst::makeLoopContinue(loc));
			return {block};
		}
		if (loop && loop->doWhileCondBreak)
		{
			auto block = awst::makeBlock(loc);
			block->body.push_back(loop->doWhileCondBreak);
			block->body.push_back(awst::makeLoopContinue(loc));
			return {block};
		}
		return {awst::makeLoopContinue(loc)};
	}

	ResultT visitBreak(Break const& _n) override
	{
		return {awst::makeLoopExit(locOf(_n))};
	}

	ResultT visitPlaceholder(PlaceholderStatement const& _n) override
	{
		// Modifier `_;` — splice in the placeholder body if one is set on the current block
		// context. DEEP-CLONE it: a modifier may contain several `_;` (the body runs once per
		// placeholder), and splicing the same shared nodes would alias them so a later in-place
		// pass corrupts every copy. Each splice gets an independent tree; cloneBlock preserves
		// any DAG sharing within the body and re-mints SingleEvaluation ids.
		if (m_blk.placeholderBody)
		{
			auto cloned = awst::cloneBlock(m_blk.placeholderBody);
			auto block = awst::makeBlock(locOf(_n));
			for (auto& s: cloned->body)
				block->body.push_back(std::move(s));
			return {block};
		}
		return {};
	}

	ResultT visitTryCatch(TryStatement const& _n) override
	{
		// AVM has no in-transaction revert recovery: a failing inner txn
		// aborts the whole outer txn, so CATCH CLAUSES ARE UNREACHABLE by
		// construction. Lower `try CALL returns (..) { S } catch.. {..}` to
		// CALL + bind returns + S, and DROP the catch arms — a documented
		// divergence that surfaces honestly in replay: any historical txn
		// whose catch path ran on EVM reverts here instead of being
		// swallowed (Aave supplyWithPermit's `try permit {} catch {}`
		// front-run-tolerance idiom is the common shape; its success path
		// is identical on both VMs).
		auto loc = locOf(_n);
		Logger::instance().warning(
			"try/catch: catch clauses are UNREACHABLE on AVM (a failing "
			"inner txn aborts the whole txn). Compiling the try call + "
			"success block; a reached catch path becomes a txn failure.",
			loc);

		ResultT out;
		auto call = m_blk.builderCtx().buildExpr(_n.externalCall());
		if (!call)
			return out;
		// External calls lower to a value plus queued effects that submit the
		// inner transaction and capture its return log.  The success binding
		// consumes that captured value, so preserve the ordinary expression
		// order: pre-effects, value consumption, then post-effects.
		auto preEffects = m_blk.builderCtx().takePreEffects();
		auto postEffects = m_blk.builderCtx().takePostEffects();
		for (auto& effect: preEffects)
			out.push_back(std::move(effect));
		auto const& clauses = _n.clauses();
		TryCatchClause const* success =
			clauses.empty() ? nullptr : clauses[0].get();
		auto const* params = success ? success->parameters() : nullptr;

		if (params && !params->parameters().empty())
		{
			auto const& ps = params->parameters();
			if (ps.size() == 1 && ps[0])
			{
				auto tgt = awst::makeVarExpression(
					m_blk.awstVarName(*ps[0]), call->wtype, loc);
				out.push_back(awst::makeAssignmentStatement(
					std::move(tgt), std::move(call), loc));
			}
			else
			{
				auto const* tup =
					dynamic_cast<awst::WTuple const*>(call->wtype);
				auto targets = awst::makeTupleExpression(call->wtype, loc);
				for (size_t i = 0; i < ps.size(); ++i)
				{
					auto const* w = (tup && i < tup->types().size())
						? tup->types()[i]
						: m_blk.builderCtx().typeMapper.map(
							ps[i] ? ps[i]->type() : nullptr);
					targets->items.push_back(awst::makeVarExpression(
						ps[i] ? m_blk.awstVarName(*ps[i])
							  : ("__try_skip" + std::to_string(i)),
						w, loc));
				}
				out.push_back(awst::makeAssignmentStatement(
					std::move(targets), std::move(call), loc));
			}
		}
		else if (call->wtype && call->wtype != awst::WType::voidType())
			out.push_back(awst::makeExpressionStatement(std::move(call), loc));
		else
			out.push_back(awst::makeExpressionStatement(std::move(call), loc));
		for (auto& effect: postEffects)
			out.push_back(std::move(effect));

		if (success)
		{
			auto childBlk = m_blk.nest();
			auto blkGuard = m_blk.builderCtx().pushScopeRaii(&childBlk);
			SolBlock handler(childBlk, success->block(),
				m_blk.makeLoc(success->block().location()));
			for (auto& st: handler.toAwst())
				out.push_back(std::move(st));
		}
		return out;
	}

	ResultT visitBlock(Block const& _n) override
	{
		// Derive a child context to maintain the parent chain.
		auto childBlk = m_blk.nest();
		auto blkGuard = m_blk.builderCtx().pushScopeRaii(&childBlk);
		SolBlock handler(childBlk, _n, m_blk.makeLoc(_n.location()));
		return handler.toAwst();
	}

	ResultT visitDefault(solidity::frontend::ASTNode const& _node) override
	{
		Logger::instance().error("unhandled statement type", m_blk.makeLoc(_node.location()));
		return {};
	}

private:
	BlockContext& m_blk;

	awst::SourceLocation locOf(solidity::frontend::ASTNode const& _n) const
	{
		return m_blk.makeLoc(_n.location());
	}
};

} // anonymous namespace

std::shared_ptr<awst::Block> SolBlock::toAwstBlock()
{
	auto awstBlock = awst::makeBlock(m_loc);

	bool const wasUnchecked = m_blk.unchecked;
	if (m_block.unchecked())
		m_blk.unchecked = true;

	for (auto const& stmt: m_block.statements())
	{
		// Assembly return/revert makes the rest statically dead; puya rejects
		// unreachable code. EVM sources often have a trailing `return` after one.
		if (m_blk.terminated)
			break;
		if (auto const* innerBlock = dynamic_cast<Block const*>(stmt.get()))
		{
			// Flatten nested blocks; unchecked-arithmetic flag propagates through.
			auto childBlk = m_blk.nest();
			auto blkGuard = m_blk.builderCtx().pushScopeRaii(&childBlk);
			SolBlock handler(childBlk, *innerBlock,
				m_blk.makeLoc(innerBlock->location()));
			auto translated = handler.toAwstBlock();
			for (auto& s: translated->body)
				awstBlock->body.push_back(std::move(s));
			// Propagate halt from the nested block to the parent.
			if (childBlk.terminated)
				m_blk.terminated = true;
		}
		else
		{
			for (auto& s: buildStatementMulti(m_blk, *stmt))
				if (s) awstBlock->body.push_back(std::move(s));
		}
	}

	m_blk.unchecked = wasUnchecked;
	return awstBlock;
}

std::vector<std::shared_ptr<awst::Statement>> SolBlock::toAwst()
{
	return {toAwstBlock()};
}

// ── Free-function entry points ──

std::vector<std::shared_ptr<awst::Statement>> buildStatementMulti(
	BlockContext& _blk,
	solidity::frontend::Statement const& _stmt)
{
	SolStatementVisitor visitor(_blk);
	auto lowered = _blk.builderCtx().lowerOperand(
		[&] { return visitor.visit(_stmt); }, false);
	std::vector<std::shared_ptr<awst::Statement>> result;
	result.reserve(lowered.effects.pre.size() + lowered.value.size()
		+ lowered.effects.post.size());
	for (auto& statement: lowered.effects.pre)
		result.push_back(std::move(statement));
	for (auto& statement: lowered.value)
		result.push_back(std::move(statement));
	for (auto& statement: lowered.effects.post)
		result.push_back(std::move(statement));
	return result;
}

std::shared_ptr<awst::Statement> buildStatement(
	BlockContext& _blk,
	solidity::frontend::Statement const& _stmt)
{
	if (auto const* block = dynamic_cast<Block const*>(&_stmt))
		return buildBlock(_blk, *block);

	auto results = buildStatementMulti(_blk, _stmt);
	if (results.size() == 1) return results[0];
	if (results.empty()) return nullptr;
	auto block = awst::makeBlock(_blk.makeLoc(_stmt.location()));
	for (auto& s: results)
		if (s) block->body.push_back(std::move(s));
	return block;
}

std::shared_ptr<awst::Block> buildBlock(
	BlockContext& _blk,
	solidity::frontend::Block const& _block)
{
	auto loc = _blk.makeLoc(_block.location());
	SolBlock handler(_blk, _block, loc);
	return handler.toAwstBlock();
}

} // namespace puyasol::builder::sol_ast
