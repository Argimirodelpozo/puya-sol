#pragma once

/// @file Termination.hpp
/// AWST control-flow analysis helpers used by the builder pipeline:
///   - blockAlwaysTerminates: does this block always terminate (return / revert)?
///   - removeDeadCode: strip statements after a guaranteed terminator
///
/// `removeDeadCode` is required for puya backend acceptance (puya 5.8.0+
/// rejects unreachable code with a compile error).

#include "awst/Node.h"

#include <memory>
#include <vector>

namespace puyasol::awst
{

namespace termination_detail
{
// True if this statement always terminates control flow on every path
// (a `return` or an `assert(false)` produced by `revert`/`require(false)`).
inline bool statementAlwaysTerminates(Statement const& _stmt)
{
	if (dynamic_cast<ReturnStatement const*>(&_stmt))
		return true;
	// assert(false) from revert/require
	if (auto const* exprStmt = dynamic_cast<ExpressionStatement const*>(&_stmt))
	{
		if (auto const* assertExpr = dynamic_cast<AssertExpression const*>(exprStmt->expr.get()))
			if (auto const* boolConst = dynamic_cast<BoolConstant const*>(assertExpr->condition.get()))
				if (!boolConst->value)
					return true;
		// The raw AVM program-exit intrinsic (`return 1`) emitted by the EVM
		// `return(offset, size)` halt lowering — terminates the whole program,
		// so the enclosing function needs no trailing implicit return.
		if (auto const* ic = dynamic_cast<IntrinsicCall const*>(exprStmt->expr.get()))
			if (ic->opCode == "return")
				return true;
	}
	return false;
}
} // namespace termination_detail

/// True if every path through this block reaches a terminator.
/// Recurses through nested blocks and `if/else` where both arms terminate.
inline bool blockAlwaysTerminates(Block const& _block)
{
	if (_block.body.empty())
		return false;
	auto const& last = _block.body.back();
	if (termination_detail::statementAlwaysTerminates(*last))
		return true;
	// Last statement is an if/else with both branches terminating
	if (auto const* ifElse = dynamic_cast<IfElse const*>(last.get()))
	{
		if (!ifElse->elseBranch)
			return false;
		return blockAlwaysTerminates(*ifElse->ifBranch)
			&& blockAlwaysTerminates(*ifElse->elseBranch);
	}
	// Brace-less branches wrap their single stmt in a Block — recurse.
	if (auto const* inner = dynamic_cast<Block const*>(last.get()))
		return blockAlwaysTerminates(*inner);
	return false;
}

/// Remove unreachable statements that follow a guaranteed terminator inside
/// a block body — recursively into nested blocks, ifs, while/for loops.
inline void removeDeadCode(std::vector<std::shared_ptr<Statement>>& _body)
{
	using termination_detail::statementAlwaysTerminates;
	for (size_t i = 0; i < _body.size(); ++i)
	{
		// Recurse into nested blocks first.
		if (auto* ifElse = dynamic_cast<IfElse*>(_body[i].get()))
		{
			if (ifElse->ifBranch) removeDeadCode(ifElse->ifBranch->body);
			if (ifElse->elseBranch) removeDeadCode(ifElse->elseBranch->body);
		}
		else if (auto* block = dynamic_cast<Block*>(_body[i].get()))
			removeDeadCode(block->body);
		else if (auto* whileLoop = dynamic_cast<WhileLoop*>(_body[i].get()))
		{
			if (whileLoop->loopBody) removeDeadCode(whileLoop->loopBody->body);
		}
		else if (auto* forLoop = dynamic_cast<ForInLoop*>(_body[i].get()))
		{
			if (forLoop->loopBody) removeDeadCode(forLoop->loopBody->body);
		}

		// If this statement always terminates, drop everything after it.
		if (statementAlwaysTerminates(*_body[i]) && i + 1 < _body.size())
		{
			_body.erase(_body.begin() + i + 1, _body.end());
			break;
		}
		// IfElse where both branches terminate → drop following statements.
		if (auto const* ifElse = dynamic_cast<IfElse const*>(_body[i].get()))
		{
			if (ifElse->ifBranch && ifElse->elseBranch
				&& blockAlwaysTerminates(*ifElse->ifBranch)
				&& blockAlwaysTerminates(*ifElse->elseBranch)
				&& i + 1 < _body.size())
			{
				_body.erase(_body.begin() + i + 1, _body.end());
				break;
			}
		}
		// Nested block that terminates → drop following statements.
		if (auto const* inner = dynamic_cast<Block const*>(_body[i].get()))
		{
			if (blockAlwaysTerminates(*inner) && i + 1 < _body.size())
			{
				_body.erase(_body.begin() + i + 1, _body.end());
				break;
			}
		}
	}
}

} // namespace puyasol::awst
