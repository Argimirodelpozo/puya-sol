#pragma once

#include "awst/Node.h"

#include <functional>

namespace puyasol::awst
{

/// Invoke `_fn` on every DIRECT child Block of `_s`: a bare nested Block,
/// if/else branches, while / for-in loop bodies (`_isLoopBody` = true), and
/// switch cases + default. THE single enumeration of AWST statement
/// containers — hand-rolled copies of this list drift (the T5 walker-gap
/// class: Switch was missing from the modifier-chain walks, ForInLoop from
/// every walker, WhileLoop from the storage-ref return rewrite). Add any new
/// container HERE and every walker inherits it.
inline void forEachChildBlock(
	Statement& _s,
	std::function<void(Block&, bool _isLoopBody)> const& _fn)
{
	if (auto* b = dynamic_cast<Block*>(&_s))
		_fn(*b, false);
	else if (auto* ie = dynamic_cast<IfElse*>(&_s))
	{
		if (ie->ifBranch) _fn(*ie->ifBranch, false);
		if (ie->elseBranch) _fn(*ie->elseBranch, false);
	}
	else if (auto* wl = dynamic_cast<WhileLoop*>(&_s))
	{
		if (wl->loopBody) _fn(*wl->loopBody, true);
	}
	else if (auto* sw = dynamic_cast<Switch*>(&_s))
	{
		for (auto& c: sw->cases)
			if (c.second) _fn(*c.second, false);
		if (sw->defaultCase) _fn(*sw->defaultCase, false);
	}
	else if (auto* fl = dynamic_cast<ForInLoop*>(&_s))
	{
		if (fl->loopBody) _fn(*fl->loopBody, true);
	}
}

/// Apply `_fn` to every return in a statement tree. Container coverage is
/// inherited from `forEachChildBlock`, so new statement kinds have one place
/// to update.
inline void forEachReturnStatement(
	std::vector<std::shared_ptr<Statement>>& _statements,
	std::function<void(ReturnStatement&)> const& _fn)
{
	for (auto& statement: _statements)
		if (auto* ret = dynamic_cast<ReturnStatement*>(statement.get()))
			_fn(*ret);
		else
			forEachChildBlock(*statement, [&](Block& block, bool) {
				forEachReturnStatement(block.body, _fn);
			});
}

} // namespace puyasol::awst
