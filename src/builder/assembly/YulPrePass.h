#pragma once

// EXPERIMENTAL (possible_solc item 5, --yul-prepass / PUYA_SOL_YUL_PREPASS spike).
// Runs a subset of solc's Yul OptimiserSuite as a canonicalising pre-pass on an
// inline-assembly block before AWST lowering. Off by default; see YulPrePass.cpp.

#include <libyul/AST.h>
#include <libsolidity/ast/ASTAnnotations.h>

#include <map>
#include <memory>
#include <string_view>

namespace solidity::yul { class Dialect; struct AsmAnalysisInfo; }

namespace puyasol::builder
{

/// Enable/disable the pre-pass (set once at startup from main.cpp via --yul-prepass).
void setYulPrePass(bool _on);
/// True if the pre-pass is enabled — by --yul-prepass OR the PUYA_SOL_YUL_PREPASS env var.
bool yulPrePassEnabled();

/// Product of the Yul optimiser pre-pass on one inline-assembly block. `block`
/// OWNS the new (disambiguated/optimised) tree; `externalRefs` is the original
/// external-reference map REBUILT against the new tree's identifier pointers,
/// matched by name (external names are reserved, so they survive verbatim).
/// A unique_ptr keeps the tree's node addresses stable across the return move,
/// so the rebuilt pointer keys stay valid.
struct YulPrePassResult
{
	std::unique_ptr<solidity::yul::Block> block;
	std::map<
		solidity::yul::Identifier const*,
		solidity::frontend::InlineAssemblyAnnotation::ExternalIdentifierInfo
	> externalRefs;
};

/// Disambiguate + run the "hgfo" prelude + `_stepAbbreviations` (empty = prelude
/// only) on a COPY of `_root`, reserving every external-reference name so it is
/// neither renamed nor reused, then rebuild the pointer-keyed external-reference
/// map against the new tree. The AWST walker resolves external refs by pointer
/// (AssemblyBuilder::resolveVarRef), so this rebuild is what keeps it correct.
YulPrePassResult runYulPrePass(
	solidity::yul::Block const& _root,
	solidity::yul::Dialect const& _dialect,
	solidity::yul::AsmAnalysisInfo const& _analysisInfo,
	std::map<
		solidity::yul::Identifier const*,
		solidity::frontend::InlineAssemblyAnnotation::ExternalIdentifierInfo
	> const& _externalRefs,
	std::string_view _stepAbbreviations
);

} // namespace puyasol::builder
