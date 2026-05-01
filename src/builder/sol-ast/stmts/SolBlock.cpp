/// @file SolBlock.cpp
/// Block statement: { stmt1; stmt2; ... }
/// Hosts SolStatementVisitor — the central statement dispatcher — which
/// takes a BlockContext& and constructs visitors with derived contexts
/// when entering nested scopes / loops / modifier-placeholder bodies.

#include "builder/sol-ast/stmts/SolBlock.h"
#include "builder/sol-ast/SolASTVisitor.h"
#include "builder/sol-ast/stmts/SolExpressionStatement.h"
#include "builder/sol-ast/stmts/SolControlFlow.h"
#include "builder/sol-ast/stmts/SolEmitStatement.h"
#include "builder/sol-ast/stmts/SolVariableDeclaration.h"
#include "builder/sol-ast/stmts/SolInlineAssembly.h"
#include "builder/sol-eb/BuilderContext.h"
#include "builder/sol-types/TypeMapper.h"
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

/// Concrete SolASTVisitor that translates Solidity statements into AWST.
/// Holds the BlockContext that scope-relevant state lives on (enclosing
/// loop, modifier placeholder body, parent chain, function ctx).
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
			auto block = std::make_shared<awst::Block>();
			block->sourceLocation = loc;
			block->body.push_back(loop->forLoopPost);
			block->body.push_back(std::make_shared<awst::LoopContinue>());
			return {block};
		}
		if (loop && loop->doWhileCondBreak)
		{
			auto block = std::make_shared<awst::Block>();
			block->sourceLocation = loc;
			block->body.push_back(loop->doWhileCondBreak);
			block->body.push_back(std::make_shared<awst::LoopContinue>());
			return {block};
		}
		auto stmt = std::make_shared<awst::LoopContinue>();
		stmt->sourceLocation = loc;
		return {stmt};
	}

	ResultT visitBreak(Break const& _n) override
	{
		auto stmt = std::make_shared<awst::LoopExit>();
		stmt->sourceLocation = locOf(_n);
		return {stmt};
	}

	ResultT visitPlaceholder(PlaceholderStatement const& _n) override
	{
		// Modifier `_;` — splice in the placeholder body if one is set
		// on the current block context.
		if (m_blk.placeholderBody)
		{
			auto block = std::make_shared<awst::Block>();
			block->sourceLocation = locOf(_n);
			for (auto const& s: m_blk.placeholderBody->body)
				block->body.push_back(s);
			return {block};
		}
		return {};
	}

	ResultT visitTryCatch(TryStatement const& _n) override
	{
		auto loc = locOf(_n);
		Logger::instance().warning(
			"try/catch stubbed as success path: AVM cannot catch runtime errors,"
			" catch clauses are dropped — behavior differs from EVM when the try"
			" call reverts", loc);

		ResultT result;
		auto& bc = m_blk.builderCtx();

		// 1. Evaluate the external call.
		auto callExpr = bc.build(_n.externalCall());

		// 2. Find the success clause and assign return values to its named
		//    parameters (declaring locals first).
		auto const* successClause = _n.successClause();
		if (successClause && successClause->parameters())
		{
			auto const& params = successClause->parameters()->parameters();
			if (!params.empty() && callExpr)
			{
				if (params.size() == 1)
				{
					auto* paramType = m_blk.typeMapper().map(params[0]->type());
					auto target = awst::makeVarExpression(params[0]->name(), paramType, loc);
					auto assign = awst::makeAssignmentStatement(std::move(target), std::move(callExpr), loc);
					result.push_back(std::move(assign));
				}
				else
				{
					// Multiple returns: tuple-unpack into named locals.
					auto tupleTarget = std::make_shared<awst::TupleExpression>();
					tupleTarget->sourceLocation = loc;
					std::vector<awst::WType const*> tupleTypes;
					for (auto const& p : params)
					{
						auto* paramType = m_blk.typeMapper().map(p->type());
						auto v = awst::makeVarExpression(p->name(), paramType, loc);
						tupleTarget->items.push_back(std::move(v));
						tupleTypes.push_back(paramType);
					}
					tupleTarget->wtype = m_blk.typeMapper().createType<awst::WTuple>(
						std::move(tupleTypes), std::nullopt);
					auto assign = awst::makeAssignmentStatement(std::move(tupleTarget), std::move(callExpr), loc);
					result.push_back(std::move(assign));
				}
			}
			else if (callExpr)
			{
				// No return params: call as an expression statement for side effects.
				auto exprStmt = awst::makeExpressionStatement(std::move(callExpr), loc);
				result.push_back(std::move(exprStmt));
			}

			// 3. Emit the success-clause block inline.
			SolBlock successHandler(m_blk, successClause->block(),
				m_blk.makeLoc(successClause->block().location()));
			auto successBlock = successHandler.toAwstBlock();
			for (auto& s : successBlock->body)
				result.push_back(std::move(s));
		}
		else if (callExpr)
		{
			auto exprStmt = awst::makeExpressionStatement(std::move(callExpr), loc);
			result.push_back(std::move(exprStmt));
		}

		return result;
	}

	ResultT visitBlock(Block const& _n) override
	{
		// Nested block: derive a child context to keep the parent chain
		// honest. (Today the chain is informational; tomorrow it can carry
		// scope/local maps.)
		auto childBlk = m_blk.nest();
		SolBlock handler(childBlk, _n, m_blk.makeLoc(_n.location()));
		return handler.toAwst();
	}

	ResultT visitDefault(solidity::frontend::ASTNode const& _node) override
	{
		Logger::instance().warning("unhandled statement type", m_blk.makeLoc(_node.location()));
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
	auto awstBlock = std::make_shared<awst::Block>();
	awstBlock->sourceLocation = m_loc;

	// Every block creates a scope — mutable context state (funcPtrTargets,
	// storageAliases, constantLocals) is snapshotted and restored on exit.
	auto& bc = m_blk.builderCtx();
	auto scope = bc.pushScope();

	bool const wasUnchecked = bc.inUncheckedBlock;
	if (m_block.unchecked())
		bc.inUncheckedBlock = true;

	for (auto const& stmt: m_block.statements())
	{
		if (auto const* innerBlock = dynamic_cast<Block const*>(stmt.get()))
		{
			// Flatten nested blocks — they share the same BlockContext nest
			// so unchecked-arithmetic propagates through.
			auto childBlk = m_blk.nest();
			SolBlock handler(childBlk, *innerBlock,
				m_blk.makeLoc(innerBlock->location()));
			auto translated = handler.toAwstBlock();
			for (auto& s: translated->body)
				awstBlock->body.push_back(std::move(s));
		}
		else
		{
			SolStatementVisitor visitor(m_blk);
			for (auto& s: visitor.visit(*stmt))
				if (s) awstBlock->body.push_back(std::move(s));
		}
	}

	bc.inUncheckedBlock = wasUnchecked;
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
	return visitor.visit(_stmt);
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
	auto block = std::make_shared<awst::Block>();
	block->sourceLocation = _blk.makeLoc(_stmt.location());
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
