#include "awst/Termination.h"

namespace puyasol::awst
{

bool statementAlwaysTerminates(Statement const& _stmt)
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
	}
	return false;
}

bool blockAlwaysTerminates(Block const& _block)
{
	if (_block.body.empty())
		return false;
	auto const& last = _block.body.back();
	if (statementAlwaysTerminates(*last))
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

void removeDeadCode(std::vector<std::shared_ptr<Statement>>& _body)
{
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
