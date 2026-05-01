#pragma once

/// @file Context.h
/// Typed nested contexts for Solidity AST traversal.
///
/// Each level of the source tree carries information that doesn't make sense
/// at outer levels:
///
///   TranslationContext  — per-contract: type mapper, source file, the
///                          BuilderContext (low-level translation state).
///   FunctionContext     — per-function: params, return type, param bit
///                          widths (for inline assembly packing).
///   BlockContext        — per-block: enclosing loop (for continue/break),
///                          placeholder body (for modifier inlining), parent
///                          link for nesting.
///   LoopContext         — per-loop: forLoopPost (i++ to run before
///                          continue) or doWhileCondBreak (cond at bottom).
///
/// Visitors are *transient* and take the **narrowest context** they need.
/// When entering a nested scope, we derive a new context with `nest()`,
/// `withLoop()`, or `withPlaceholder()`, then construct a new visitor with
/// it. Stack-allocated; no save-and-restore.

#include "awst/Node.h"

#include <libsolidity/ast/AST.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace puyasol::builder
{
class TypeMapper;
namespace eb { class BuilderContext; }
}

namespace puyasol::builder::sol_ast
{

/// Top-level translation context: per-contract state we share across
/// every function and statement. Wraps BuilderContext (which still holds
/// the lower-level shared state like storage layout, library function IDs,
/// scope mappings); future cleanup can flatten more of that down here.
struct TranslationContext
{
	eb::BuilderContext& exprBuilder;
	TypeMapper& typeMapper;
	std::string sourceFile;

	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _sl) const
	{
		awst::SourceLocation loc;
		loc.file = sourceFile;
		loc.line = _sl.start >= 0 ? _sl.start : 0;
		loc.endLine = _sl.end >= 0 ? _sl.end : 0;
		return loc;
	}

	awst::SourceLocation makeLoc(int _start, int _end) const
	{
		awst::SourceLocation loc;
		loc.file = sourceFile;
		loc.line = _start >= 0 ? _start : 0;
		loc.endLine = _end >= 0 ? _end : 0;
		return loc;
	}
};

/// Function-level context: signature info needed to translate the body.
struct FunctionContext
{
	TranslationContext& tr;
	std::vector<std::pair<std::string, awst::WType const*>> params;
	awst::WType const* returnType = nullptr;
	std::map<std::string, unsigned> paramBitWidths;
};

/// Loop-level context: control-flow targets for continue inside this loop.
/// `forLoopPost` is the post-step (e.g., `i++`) to splice in before
/// `LoopContinue`. `doWhileCondBreak` is the bottom-of-body condition check
/// for do/while. At most one of the two is set.
struct LoopContext
{
	std::shared_ptr<awst::Statement> forLoopPost;
	std::shared_ptr<awst::Statement> doWhileCondBreak;
};

/// Block/scope-level context: nesting chain, enclosing loop (for
/// continue/break), modifier placeholder body (for `_;` inlining).
struct BlockContext
{
	FunctionContext& fn;
	BlockContext const* parent = nullptr;
	LoopContext const* enclosingLoop = nullptr;
	std::shared_ptr<awst::Block> placeholderBody;

	/// Construct the top-level block (function body root).
	static BlockContext top(FunctionContext& _fn)
	{
		return {_fn, nullptr, nullptr, nullptr};
	}

	/// Derive a child block context — same enclosing loop & placeholder.
	BlockContext nest() const
	{
		return {fn, this, enclosingLoop, placeholderBody};
	}

	/// Derive a context whose body is the body of `_loop`.
	BlockContext withLoop(LoopContext const& _loop) const
	{
		BlockContext c = nest();
		c.enclosingLoop = &_loop;
		return c;
	}

	/// Derive a context for the body of a modifier-inlined function:
	/// `_;` placeholders splice in `_body`.
	BlockContext withPlaceholder(std::shared_ptr<awst::Block> _body) const
	{
		BlockContext c = nest();
		c.placeholderBody = std::move(_body);
		return c;
	}

	// ── Convenience accessors (bridge to underlying BuilderContext) ──

	eb::BuilderContext& builderCtx() const { return fn.tr.exprBuilder; }
	TypeMapper& typeMapper() const { return fn.tr.typeMapper; }
	std::string const& sourceFile() const { return fn.tr.sourceFile; }
	awst::SourceLocation makeLoc(solidity::langutil::SourceLocation const& _sl) const
	{
		return fn.tr.makeLoc(_sl);
	}
};

} // namespace puyasol::builder::sol_ast
