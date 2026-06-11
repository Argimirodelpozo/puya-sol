/// @file AwstPostPasses.h
/// The option-driven post-AWST pass chain moved out of main.cpp: inline
/// overrides, --fn-split slicing (+ chain_groups.json), --deploy-pure-helpers
/// extraction (+ pure_helpers.json), the SimpleSplitter pipeline, and the
/// `new C()` child deploy-template artifact. Pure orchestration — all
/// splitter internals stay in src/splitter/. Pass order matters and is
/// documented at each function; main() calls them in declaration order.
#pragma once

#include "awst/Node.h"
#include "cli/CliOptions.h"
#include "splitter/PureHelperExtractor.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace puyasol::cli
{

using AwstRoots = std::vector<std::shared_ptr<awst::RootNode>>;

/// --force-inline-sub / --force-no-inline-sub: flip inlineOpt on matching
/// Subroutine / ContractMethod nodes. Runs BEFORE --fn-split so the
/// (non-)inlined body is visible at split time.
void applyInlineOverrides(AwstRoots& _roots, Options const& _opts);

/// --fn-split: slice subroutine bodies into pieces (appended to roots), and
/// emit chain_groups.json for cross-chunk chains. Runs BEFORE --uros-splitter
/// so the new piece subroutines are visible when uros bin-packs methods.
void applyFnSplits(AwstRoots& _roots, Options const& _opts);

/// --deploy-pure-helpers: lift pure Subroutines into sidecar Contracts and
/// emit pure_helpers.json. Runs AFTER --fn-split and BEFORE --uros-splitter.
/// Returns the extraction result; the caller declares each helper's
/// TMPL_* var in options.json.
splitter::PureHelperExtractor::Result extractPureHelpers(
	AwstRoots& _roots, Options const& _opts);

/// --split-config / --force-delegate: SimpleSplitter pipeline (static
/// "extract-named-subroutines" splitter, alternative to --uros-splitter).
/// When the runner takes ownership of output, returns the process exit code
/// the caller must return immediately; std::nullopt means "not requested or
/// didn't split — continue the normal pipeline".
std::optional<int> runSimpleSplitterIfRequested(
	AwstRoots& _roots, Options const& _opts, std::string const& _sourceFile);

/// After the puya backend ran: if any `new C()` child contracts were
/// referenced, bundle their compiled binaries into deploy.tmpl.json
/// (TMPL_APPROVAL_<Child> / TMPL_CLEAR_<Child> hex blobs) and reset the
/// child-contract registry.
void writeChildDeployTemplates(std::string const& _outputDir);

} // namespace puyasol::cli
