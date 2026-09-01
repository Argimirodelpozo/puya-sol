/// @file AwstPostPasses.h
/// Option-driven post-AWST passes (extracted from main.cpp): inline overrides
/// and the new-C() deploy-template artifact. The splitter passes (--fn-split,
/// --deploy-pure-helpers, --split-config) live on branch experimental/splitter.
#pragma once

#include "awst/Node.h"
#include "cli/CliOptions.h"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace puyasol::cli
{

using AwstRoots = std::vector<std::shared_ptr<awst::RootNode>>;

/// Flip inlineOpt on matching nodes.
void applyInlineOverrides(AwstRoots& _roots, Options const& _opts);

/// Bundle new-C() child binaries into deploy.tmpl.json.
void writeChildDeployTemplates(
	std::string const& _outputDir,
	std::set<std::string> const& _childContracts);

} // namespace puyasol::cli
