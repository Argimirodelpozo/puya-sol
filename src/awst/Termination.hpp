#pragma once

/// @file Termination.hpp
/// AWST control-flow analysis helpers used by the builder pipeline:
///   - blockAlwaysTerminates: does this block always terminate (return / revert)?
///   - removeDeadCode: strip statements after a guaranteed terminator
///
/// `removeDeadCode` is required for puya backend acceptance (puya 5.8.0+
/// rejects unreachable code with a compile error).

#include "awst/StatementWalk.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace puyasol::awst
{

inline bool blockAlwaysTerminates(Block const& _block);

namespace termination_detail
{
// No fallthrough in the current block. Loop transfers terminate their body,
// not the enclosing loop: loops themselves remain conservatively fallthrough.
inline bool statementAlwaysTerminates(Statement const& _stmt)
{
	if (dynamic_cast<ReturnStatement const*>(&_stmt)
		|| dynamic_cast<LoopExit const*>(&_stmt)
		|| dynamic_cast<LoopContinue const*>(&_stmt))
		return true;
	if (auto const* block = dynamic_cast<Block const*>(&_stmt))
		return blockAlwaysTerminates(*block);
	if (auto const* branch = dynamic_cast<IfElse const*>(&_stmt))
		return branch->ifBranch && branch->elseBranch
			&& blockAlwaysTerminates(*branch->ifBranch)
			&& blockAlwaysTerminates(*branch->elseBranch);
	if (auto const* branch = dynamic_cast<Switch const*>(&_stmt))
		return branch->defaultCase && blockAlwaysTerminates(*branch->defaultCase)
			&& std::all_of(branch->cases.begin(), branch->cases.end(), [](auto const& item) {
				return item.second && blockAlwaysTerminates(*item.second);
			});
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

/// True if every path exits this block, including before an unpruned tail.
inline bool blockAlwaysTerminates(Block const& _block)
{
	return std::any_of(_block.body.begin(), _block.body.end(), [](auto const& statement) {
		return termination_detail::statementAlwaysTerminates(*statement);
	});
}

/// Remove unreachable statements that follow a guaranteed terminator inside
/// a block body, with container coverage supplied by the shared AWST walker.
inline void removeDeadCode(std::vector<std::shared_ptr<Statement>>& _body)
{
	using termination_detail::statementAlwaysTerminates;
	for (size_t i = 0; i < _body.size(); ++i)
	{
		forEachChildBlock(*_body[i], [](Block& block, bool) {
			removeDeadCode(block.body);
		});

		// If this statement always terminates, drop everything after it.
		if (statementAlwaysTerminates(*_body[i]) && i + 1 < _body.size())
		{
			_body.erase(_body.begin() + i + 1, _body.end());
			break;
		}
	}
}

} // namespace puyasol::awst
