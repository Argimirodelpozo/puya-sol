#pragma once

/// @file MappingPrefix.h
/// THE box-key-prefix derivation for a storage mapping (or aggregate holder)
/// reached through struct fields. Direct index access
/// (SolIndexAccessHandlers::resolveCursorContext) and mapping-storage call
/// arguments (SolInternalCall::extractMappingKeyPrefix) each carried their own
/// copy, and the copies disagreed: the call side resolved only depth-1 field
/// chains (f(st.a.m) keyed box utf8("m") while st.a.m[k] keyed
/// utf8(st)++"a"++"m" — split-brain state), and the two alias peels handled
/// different wrapper node types — the direct-access peel even DROPPED the
/// field names it walked (`Inner storage p = st.a; p.m[k]` keyed utf8(st)++"m",
/// losing "a"). One implementation, shared by both.

#include "awst/Node.h"
#include "builder/sol-types/SolcFwd.h"

#include <memory>

namespace puyasol::builder::eb
{
class ContractContext;
}

namespace puyasol::builder::sol_ast
{
class Context;

/// Resolve the HOLDER prefix for a chain ROOT expression:
///   - registered mapping-key param → its runtime bytes value;
///   - storage ALIAS → the aliased holder's box/appstate key ++ the field
///     names the alias itself walked (peeling StateGet / ReinterpretCast /
///     FieldExpression in any interleaving);
///   - plain state variable → utf8(physical binding);
///   - mapping element root (`mm[k]`) → the built element's box key.
/// Returns nullptr when the shape is not resolvable here (callers keep their
/// legacy fallbacks; array-element roots with bounds asserts stay in
/// SolIndexAccessHandlers).
std::shared_ptr<awst::Expression> resolveHolderRoot(
	eb::ContractContext& _ctx,
	Context& _scope,
	solidity::frontend::Expression const& _rootExpr,
	awst::SourceLocation const& _loc);

/// Full derivation for a MemberAccess chain `root.f1...fn`: walk to the root,
/// resolveHolderRoot, then append utf8(f1)..utf8(fn). Returns the raw
/// concatenated prefix (callers wrap it in their own reinterpret); nullptr
/// when the root is unresolvable or `_expr` is not a MemberAccess.
std::shared_ptr<awst::Expression> resolveMappingHolderPrefix(
	eb::ContractContext& _ctx,
	Context& _scope,
	solidity::frontend::Expression const& _expr,
	awst::SourceLocation const& _loc);

} // namespace puyasol::builder::sol_ast
