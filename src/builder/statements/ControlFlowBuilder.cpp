/// @file ControlFlowBuilder.cpp
/// Handles control flow: if/else, while, for, continue, break.

#include "builder/statements/StatementBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

namespace puyasol::builder
{

bool StatementBuilder::visit(solidity::frontend::IfStatement const& _node)
{
	auto loc = makeLoc(_node.location());
	auto stmt = std::make_shared<awst::IfElse>();
	stmt->sourceLocation = loc;
	stmt->condition = m_exprBuilder.build(_node.condition());

	// Capture pending statements from condition evaluation BEFORE building
	// branches (buildBlock clears m_stack). These will be pushed AFTER branches.
	auto prePending = m_exprBuilder.takePrePendingStatements();
	auto postPending = m_exprBuilder.takePendingStatements();

	// True branch
	auto const& trueBody = _node.trueStatement();
	if (auto const* block = dynamic_cast<solidity::frontend::Block const*>(&trueBody))
		stmt->ifBranch = buildBlock(*block);
	else
	{
		auto syntheticBlock = std::make_shared<awst::Block>();
		syntheticBlock->sourceLocation = makeLoc(trueBody.location());
		auto translated = build(trueBody);
		if (translated)
			syntheticBlock->body.push_back(std::move(translated));
		stmt->ifBranch = syntheticBlock;
	}

	// False branch (optional)
	if (_node.falseStatement())
	{
		auto const& falseBody = *_node.falseStatement();
		if (auto const* block = dynamic_cast<solidity::frontend::Block const*>(&falseBody))
			stmt->elseBranch = buildBlock(*block);
		else
		{
			auto syntheticBlock = std::make_shared<awst::Block>();
			syntheticBlock->sourceLocation = makeLoc(falseBody.location());
			auto translated = build(falseBody);
			if (translated)
				syntheticBlock->body.push_back(std::move(translated));
			stmt->elseBranch = syntheticBlock;
		}
	}

	// Flush pre-pending statements before the IfElse
	for (auto& p: prePending)
		push(std::move(p));
	push(stmt);
	// Flush post-pending statements (e.g., storage write-back) after the IfElse
	for (auto& p: postPending)
		push(std::move(p));
	return false;
}

bool StatementBuilder::visit(solidity::frontend::WhileStatement const& _node)
{
	auto loc = makeLoc(_node.location());

	if (_node.isDoWhile())
	{
		// do { body } while (cond) → while (true) { body; if (!cond) break; }
		auto loop = std::make_shared<awst::WhileLoop>();
		loop->sourceLocation = loc;

		auto trueLit = std::make_shared<awst::BoolConstant>();
		trueLit->sourceLocation = loc;
		trueLit->wtype = awst::WType::boolType();
		trueLit->value = true;
		loop->condition = trueLit;

		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = makeLoc(_node.body().location());

		// Body statements
		if (auto const* block = dynamic_cast<solidity::frontend::Block const*>(&_node.body()))
		{
			for (auto const& stmt: block->statements())
			{
				auto translated = build(*stmt);
				if (translated)
					body->body.push_back(std::move(translated));
			}
		}

		// Check condition at end
		auto cond = m_exprBuilder.build(_node.condition());
		auto notCond = std::make_shared<awst::Not>();
		notCond->sourceLocation = loc;
		notCond->wtype = awst::WType::boolType();
		notCond->expr = std::move(cond);

		auto breakBlock = std::make_shared<awst::Block>();
		breakBlock->sourceLocation = loc;
		breakBlock->body.push_back(std::make_shared<awst::LoopExit>());

		auto ifBreak = std::make_shared<awst::IfElse>();
		ifBreak->sourceLocation = loc;
		ifBreak->condition = notCond;
		ifBreak->ifBranch = breakBlock;

		body->body.push_back(ifBreak);
		loop->loopBody = body;

		push(loop);
	}
	else
	{
		auto loop = std::make_shared<awst::WhileLoop>();
		loop->sourceLocation = loc;
		loop->condition = m_exprBuilder.build(_node.condition());

		if (auto const* block = dynamic_cast<solidity::frontend::Block const*>(&_node.body()))
			loop->loopBody = buildBlock(*block);
		else
		{
			auto body = std::make_shared<awst::Block>();
			body->sourceLocation = makeLoc(_node.body().location());
			auto translated = build(_node.body());
			if (translated)
				body->body.push_back(std::move(translated));
			loop->loopBody = body;
		}

		push(loop);
	}

	return false;
}

bool StatementBuilder::visit(solidity::frontend::ForStatement const& _node)
{
	// for (init; cond; post) body → { init; while (cond) { body; post; } }
	auto loc = makeLoc(_node.location());
	auto outerBlock = std::make_shared<awst::Block>();
	outerBlock->sourceLocation = loc;

	// Init
	if (_node.initializationExpression())
	{
		auto init = build(*_node.initializationExpression());
		if (init)
			outerBlock->body.push_back(std::move(init));
	}

	// While loop
	auto loop = std::make_shared<awst::WhileLoop>();
	loop->sourceLocation = loc;

	// Condition (default true if absent)
	if (_node.condition())
		loop->condition = m_exprBuilder.build(*_node.condition());
	else
	{
		auto trueLit = std::make_shared<awst::BoolConstant>();
		trueLit->sourceLocation = loc;
		trueLit->wtype = awst::WType::boolType();
		trueLit->value = true;
		loop->condition = trueLit;
	}

	// Build post-expression first so continue statements can reference it
	std::shared_ptr<awst::Statement> postStmt;
	if (_node.loopExpression())
		postStmt = build(*_node.loopExpression());

	// Set post expression so continue statements inject it before jumping
	auto savedPost = m_forLoopPost;
	m_forLoopPost = postStmt;

	// Loop body
	auto loopBody = std::make_shared<awst::Block>();
	loopBody->sourceLocation = loc;

	if (auto const* block = dynamic_cast<solidity::frontend::Block const*>(&_node.body()))
	{
		for (auto const& stmt: block->statements())
		{
			auto translated = build(*stmt);
			if (translated)
				loopBody->body.push_back(std::move(translated));
		}
	}
	else
	{
		auto translated = build(_node.body());
		if (translated)
			loopBody->body.push_back(std::move(translated));
	}

	// Restore previous post (for nested for-loops)
	m_forLoopPost = savedPost;

	// Append post-expression at end of loop body (normal iteration path)
	if (postStmt)
		loopBody->body.push_back(postStmt);

	loop->loopBody = loopBody;
	outerBlock->body.push_back(loop);
	push(outerBlock);
	return false;
}

bool StatementBuilder::visit(solidity::frontend::Continue const& _node)
{
	auto loc = makeLoc(_node.location());

	if (m_forLoopPost)
	{
		// In for-loops, continue must execute the post expression (e.g., i++)
		// before jumping back to the condition. Wrap both in a Block so they're
		// treated as a single statement by the caller.
		auto block = std::make_shared<awst::Block>();
		block->sourceLocation = loc;
		block->body.push_back(m_forLoopPost);
		auto cont = std::make_shared<awst::LoopContinue>();
		cont->sourceLocation = loc;
		block->body.push_back(std::move(cont));
		push(block);
	}
	else
	{
		auto stmt = std::make_shared<awst::LoopContinue>();
		stmt->sourceLocation = loc;
		push(stmt);
	}
	return false;
}

bool StatementBuilder::visit(solidity::frontend::Break const& _node)
{
	auto stmt = std::make_shared<awst::LoopExit>();
	stmt->sourceLocation = makeLoc(_node.location());
	push(stmt);
	return false;
}


} // namespace puyasol::builder
