/// @file SolBlock.cpp
/// Block statement and SolStatementVisitor — central statement dispatcher.

#include "builder/sol-ast/SolStatement.h"
#include "awst/Termination.hpp"
#include "builder/sol-ast/stmts/SolExpressionStatement.h"
#include "builder/sol-ast/stmts/SolControlFlow.h"
#include "builder/sol-ast/stmts/SolEmitStatement.h"
#include "builder/sol-ast/stmts/SolVariableDeclaration.h"
#include "builder/sol-ast/stmts/SolInlineAssembly.h"
#include "builder/EvmFeaturePolicy.h"
#include "builder/CallBoundaryPlan.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>
#include <libsolutil/Assertions.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

namespace
{

/// Translates Solidity statements into AWST. Holds the BlockContext
/// (enclosing loop, modifier placeholder factory, effective unchecked flag).
/// Handlers own child lowering; returning false disables solc's child walk.
class SolStatementVisitor: public ASTConstVisitor
{
public:
	explicit SolStatementVisitor(BlockContext& _blk): m_blk(_blk) {}

	using ResultT = std::vector<std::shared_ptr<awst::Statement>>;

	ResultT build(Statement const& _n)
	{
		_n.accept(*this);
		return std::move(m_result);
	}

	bool visit(ExpressionStatement const& _n) override
	{
		m_result = SolExpressionStatement(m_blk, _n, locOf(_n)).toAwst();
		return false;
	}

	bool visit(Return const& _n) override
	{
		m_result = SolReturnStatement(m_blk, _n, locOf(_n)).toAwst();
		return false;
	}

	bool visit(RevertStatement const& _n) override
	{
		m_result = SolRevertStatement(m_blk, _n, locOf(_n)).toAwst();
		return false;
	}

	bool visit(EmitStatement const& _n) override
	{
		m_result = SolEmitStatement(m_blk, _n, locOf(_n)).toAwst();
		return false;
	}

	bool visit(VariableDeclarationStatement const& _n) override
	{
		m_result = SolVariableDeclaration(m_blk, _n, locOf(_n)).toAwst();
		return false;
	}

	bool visit(IfStatement const& _n) override
	{
		m_result = SolIfStatement(m_blk, _n, locOf(_n)).toAwst();
		return false;
	}

	bool visit(WhileStatement const& _n) override
	{
		m_result = SolWhileStatement(m_blk, _n, locOf(_n)).toAwst();
		return false;
	}

	bool visit(ForStatement const& _n) override
	{
		m_result = SolForStatement(m_blk, _n, locOf(_n)).toAwst();
		return false;
	}

	bool visit(InlineAssembly const& _n) override
	{
		m_result = SolInlineAssembly(m_blk, _n, locOf(_n)).toAwst();
		return false;
	}

	bool visit(Continue const& _n) override
	{
		auto loc = locOf(_n);
		auto const* loop = m_blk.enclosingLoop;
		if (loop && loop->continuePrefix)
			if (auto prefix = loop->continuePrefix()) m_result.push_back(std::move(prefix));
		m_result.push_back(awst::makeLoopContinue(loc));
		return false;
	}

	bool visit(Break const& _n) override
	{
		m_result = {awst::makeLoopExit(locOf(_n))};
		return false;
	}

	bool visit(PlaceholderStatement const& _n) override
	{
		// The factory constructs a new call block for every `_;`. In particular,
		// a modifier containing more than one placeholder must not share mutable
		// AWST nodes or SingleEvaluation identities between expansions.
		if (m_blk.placeholderBody)
		{
			auto placeholder = m_blk.placeholderBody();
			auto block = awst::makeBlock(locOf(_n));
			if (placeholder)
				for (auto& s: placeholder->body)
					block->body.push_back(std::move(s));
			m_result = {std::move(block)};
		}
		return false;
	}

	bool visit(TryStatement const& _n) override
	{
		m_result = buildTry(_n);
		return false;
	}

	bool visit(Block const& _n) override
	{
		// Plain lexical blocks flatten; their scope is owned by buildBlock.
		m_result = std::move(buildBlock(m_blk, _n)->body);
		return false;
	}

private:
	BlockContext& m_blk;
	ResultT m_result;

	bool visitNode(ASTNode const& _node) override
	{
		Logger::instance().error("unhandled statement type", locOf(_node));
		return false;
	}

	ResultT buildTry(TryStatement const& _n)
	{
		// Accepted AVM adaptation: a failed inner transaction aborts its caller.
		// Keep the call and success body; report the dropped catch behavior.
		auto loc = locOf(_n);
		EvmFeaturePolicy::report(
			EvmFeature::TryCatch,
			m_blk.builderCtx().typeMapper.profile(), loc);

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
		auto const& success = *_n.successClause();
		auto const* params = success.parameters();

		if (params && !params->parameters().empty())
		{
			auto const& ps = params->parameters();
			// Solc checked the return declarations against the callee. Our
			// lowering must preserve that arity and adapt its physical carriers.
			auto const* tuple = dynamic_cast<awst::WTuple const*>(call->wtype);
			solAssert(ps.size() == 1 || (tuple && tuple->types().size() == ps.size()),
				"try-success return arity changed during lowering");
			std::vector<awst::WType const*> types;
			std::vector<std::shared_ptr<awst::Expression>> targets;
			for (auto const& declaration: ps)
			{
				types.push_back(m_blk.typeMapper().map(declaration->type()));
				targets.push_back(awst::makeVarExpression(
					m_blk.scope.awstVarName(*declaration), types.back(), locOf(*declaration)));
			}
			std::shared_ptr<awst::Expression> target = targets.front();
			if (targets.size() > 1)
			{
				auto binding = awst::makeTupleExpression(
					m_blk.typeMapper().createType<awst::WTuple>(std::move(types)), loc);
				binding->items = std::move(targets);
				target = std::move(binding);
			}
			call = decodeCallResult(std::move(call), target->wtype, loc);
			solAssert(call && awst::structurallyEquivalent(call->wtype, target->wtype),
				"try-success return type changed during lowering");
			out.push_back(awst::makeAssignmentStatement(std::move(target), std::move(call), loc));
		}
		else
			out.push_back(awst::makeExpressionStatement(std::move(call), loc));
		for (auto& effect: postEffects)
			out.push_back(std::move(effect));

		out.push_back(buildBlock(m_blk, success.block()));
		return out;
	}

	awst::SourceLocation locOf(solidity::frontend::ASTNode const& _n) const
	{
		return m_blk.makeLoc(_n.location());
	}
};

} // anonymous namespace

// ── Free-function entry points ──

std::vector<std::shared_ptr<awst::Statement>> buildStatementMulti(
	BlockContext& _blk,
	solidity::frontend::Statement const& _stmt)
{
	SolStatementVisitor visitor(_blk);
	auto lowered = _blk.builderCtx().lowerOperand(
		[&] { return visitor.build(_stmt); }, false);
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
	auto results = buildStatementMulti(_blk, _stmt);
	if (results.size() == 1) return results[0];
	if (results.empty()) return nullptr;
	auto block = awst::makeBlock(_blk.makeLoc(_stmt.location()));
	for (auto& s: results)
		if (s) block->body.push_back(std::move(s));
	return block;
}

std::shared_ptr<awst::Block> buildBlock(
	BlockContext const& _parent,
	solidity::frontend::Statement const& _body)
{
	auto child = _parent.nest();
	auto const* sourceBlock = dynamic_cast<Block const*>(&_body);
	child.scope.unchecked |= sourceBlock && sourceBlock->unchecked();
	auto guard = child.builderCtx().pushScopeRaii(&child.scope);
	auto block = awst::makeBlock(child.makeLoc(_body.location()));
	auto append = [&](Statement const& source) {
		for (auto& statement: buildStatementMulti(child, source))
			if (statement)
			{
				bool const terminates = awst::statementAlwaysTerminates(*statement);
				block->body.push_back(std::move(statement));
				if (terminates) return false;
			}
		return true;
	};
	if (sourceBlock)
	{
		for (auto const& statement: sourceBlock->statements())
			if (!append(*statement)) break;
	}
	else
		append(_body);
	return block;
}

} // namespace puyasol::builder::sol_ast
