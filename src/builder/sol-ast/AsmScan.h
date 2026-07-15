/// @file AsmScan.h
/// Recursive "does this subtree use inline assembly" query.

#pragma once

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

/// True iff the subtree contains an InlineAssembly node at ANY depth.
/// A naive `body().statements()` scan misses asm nested in a block
/// (`unchecked { assembly { ... } }` or a plain `{ }`), silently flipping
/// the asm-specific param/decode/storage-ref gates — every "does this
/// function use assembly" check must go through here so the gates agree.
inline bool containsInlineAssembly(solidity::frontend::ASTNode const& _node)
{
	class Scan: public solidity::frontend::ASTConstVisitor
	{
	public:
		bool found = false;
		bool visit(solidity::frontend::InlineAssembly const&) override
		{
			found = true;
			return false;
		}
	};
	Scan s;
	_node.accept(s);
	return s.found;
}

} // namespace puyasol::builder
