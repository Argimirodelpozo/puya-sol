/// @file AwstPostPasses.h
/// Option-driven post-AWST passes (extracted from main.cpp): inline overrides,
/// --fn-split + chain_groups.json, --deploy-pure-helpers + pure_helpers.json,
/// SimpleSplitter pipeline, new-C() deploy-template artifact.
/// Pass order matters; main() calls them in declaration order.
#pragma once

#include "awst/Node.h"
#include "cli/CliOptions.h"
#include "experimental/splitter/PureHelperExtractor.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace puyasol::cli
{

using AwstRoots = std::vector<std::shared_ptr<awst::RootNode>>;

/// Flip inlineOpt on matching nodes. Runs before --fn-split.
void applyInlineOverrides(AwstRoots& _roots, Options const& _opts);

/// Slice subroutine bodies; emit chain_groups.json. Runs before --uros-splitter.
void applyFnSplits(AwstRoots& _roots, Options const& _opts);

/// Lift pure Subroutines into sidecar Contracts; emit pure_helpers.json.
/// Runs after --fn-split, before --uros-splitter.
splitter::PureHelperExtractor::Result extractPureHelpers(
	AwstRoots& _roots, Options const& _opts);

/// SimpleSplitter pipeline (--split-config / --force-delegate). Returns the
/// puya exit code when the runner owns output; std::nullopt = continue normally.
std::optional<int> runSimpleSplitterIfRequested(
	AwstRoots& _roots, Options const& _opts, std::string const& _sourceFile);

/// Bundle new-C() child binaries into deploy.tmpl.json and reset the registry.
void writeChildDeployTemplates(std::string const& _outputDir);

} // namespace puyasol::cli
