/// @file SolBlock.cpp
/// Block statement — the central statement dispatcher.
/// Central statement dispatcher: builds AWST statements from Solidity AST nodes.

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
	StatementContext& _ctx,
	Block const& _node,
	awst::SourceLocation _loc,
	eb::BuilderContext& _exprBuilder)
	: SolStatement(_ctx, std::move(_loc)), m_block(_node), m_exprBuilder(_exprBuilder)
{
}

namespace
{

/// Concrete SolASTVisitor that translates Solidity statements into AWST.
/// Holds the shared StatementContext (control-flow state, modifier
/// placeholder body, function context) plus the expression builder.
class AwstStatementVisitor: public SolASTVisitor<std::vector<std::shared_ptr<awst::Statement>>>
{
public:
	AwstStatementVisitor(StatementContext& _ctx, eb::BuilderContext& _exprBuilder)
		: m_ctx(_ctx), m_exprBuilder(_exprBuilder) {}

	using ResultT = std::vector<std::shared_ptr<awst::Statement>>;

	ResultT visitExprStatement(ExpressionStatement const& _n) override
	{
		SolExpressionStatement handler(m_ctx, _n, locOf(_n));
		return handler.toAwst();
	}

	ResultT visitReturn(Return const& _n) override
	{
		SolReturnStatement handler(m_ctx, _n, locOf(_n));
		return handler.toAwst();
	}

	ResultT visitRevert(RevertStatement const& _n) override
	{
		SolRevertStatement handler(m_ctx, _n, locOf(_n));
		return handler.toAwst();
	}

	ResultT visitEmit(EmitStatement const& _n) override
	{
		SolEmitStatement handler(m_ctx, _n, locOf(_n));
		return handler.toAwst();
	}

	ResultT visitVarDecl(VariableDeclarationStatement const& _n) override
	{
		SolVariableDeclaration handler(m_ctx, _n, locOf(_n), m_exprBuilder);
		return handler.toAwst();
	}

	ResultT visitIfStatement(IfStatement const& _n) override
	{
		SolIfStatement handler(m_ctx, _n, locOf(_n));
		return handler.toAwst();
	}

	ResultT visitWhile(WhileStatement const& _n) override
	{
		SolWhileStatement handler(m_ctx, _n, locOf(_n));
		return handler.toAwst();
	}

	ResultT visitFor(ForStatement const& _n) override
	{
		SolForStatement handler(m_ctx, _n, locOf(_n));
		return handler.toAwst();
	}

	ResultT visitInlineAssembly(InlineAssembly const& _n) override
	{
		SolInlineAssembly handler(m_ctx, _n, locOf(_n),
			m_ctx.functionParams, m_ctx.returnType, m_ctx.functionParamBitWidths);
		return handler.toAwst();
	}

	ResultT visitContinue(Continue const& _n) override
	{
		auto loc = locOf(_n);
		// Inside a for-loop, the post-statement (i++) must run before continuing
		// so the loop converges. Inside a do/while, the condition→break check
		// must happen at the bottom of the body.
		if (m_ctx.forLoopPost)
		{
			auto block = std::make_shared<awst::Block>();
			block->sourceLocation = loc;
			block->body.push_back(m_ctx.forLoopPost);
			block->body.push_back(std::make_shared<awst::LoopContinue>());
			return {block};
		}
		if (m_ctx.doWhileCondBreak)
		{
			auto block = std::make_shared<awst::Block>();
			block->sourceLocation = loc;
			block->body.push_back(m_ctx.doWhileCondBreak);
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
		// Modifier `_;` — splice in the placeholder body if one is set.
		if (m_ctx.placeholderBody)
		{
			auto block = std::make_shared<awst::Block>();
			block->sourceLocation = locOf(_n);
			for (auto const& s: m_ctx.placeholderBody->body)
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

		// 1. Evaluate the external call.
		auto callExpr = m_exprBuilder.build(_n.externalCall());

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
					auto* paramType = m_ctx.typeMapper->map(params[0]->type());
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
						auto* paramType = m_ctx.typeMapper->map(p->type());
						auto v = awst::makeVarExpression(p->name(), paramType, loc);
						tupleTarget->items.push_back(std::move(v));
						tupleTypes.push_back(paramType);
					}
					tupleTarget->wtype = m_ctx.typeMapper->createType<awst::WTuple>(
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
			SolBlock successHandler(
				m_ctx, successClause->block(),
				m_ctx.makeLoc(successClause->block().location()), m_exprBuilder);
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
		SolBlock handler(m_ctx, _n, locOf(_n), m_exprBuilder);
		return handler.toAwst();
	}

	ResultT visitDefault(solidity::frontend::ASTNode const& _node) override
	{
		Logger::instance().warning("unhandled statement type", m_ctx.makeLoc(_node.location()));
		return {};
	}

private:
	StatementContext& m_ctx;
	eb::BuilderContext& m_exprBuilder;

	awst::SourceLocation locOf(solidity::frontend::ASTNode const& _n) const
	{
		return m_ctx.makeLoc(_n.location());
	}
};

} // anonymous namespace

/// Dispatch a single Solidity statement to the right sol-ast wrapper (free function).
static std::vector<std::shared_ptr<awst::Statement>> dispatchStatementImpl(
	StatementContext& _ctx,
	eb::BuilderContext& _exprBuilder,
	Statement const& _stmt)
{
	AwstStatementVisitor visitor(_ctx, _exprBuilder);
	return visitor.visit(_stmt);
}

std::shared_ptr<awst::Block> SolBlock::toAwstBlock()
{
	auto awstBlock = std::make_shared<awst::Block>();
	awstBlock->sourceLocation = m_loc;

	// Every block creates a scope — mutable context state (funcPtrTargets,
	// storageAliases, constantLocals) is snapshotted and restored on exit.
	auto& bc = m_exprBuilder;
	auto scope = bc.pushScope();

	bool const wasUnchecked = bc.inUncheckedBlock;
	if (m_block.unchecked())
		bc.inUncheckedBlock = true;

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

	bc.inUncheckedBlock = wasUnchecked;
	return awstBlock;
}

std::vector<std::shared_ptr<awst::Statement>> SolBlock::toAwst()
{
	return {toAwstBlock()};
}

// ── Free functions ──

std::shared_ptr<awst::Statement> buildStatement(
	StatementContext& _ctx,
	eb::BuilderContext& _exprBuilder,
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
	eb::BuilderContext& _exprBuilder,
	solidity::frontend::Block const& _block)
{
	auto loc = _ctx.makeLoc(_block.location());
	SolBlock handler(_ctx, _block, loc, _exprBuilder);
	return handler.toAwstBlock();
}

} // namespace puyasol::builder::sol_ast
