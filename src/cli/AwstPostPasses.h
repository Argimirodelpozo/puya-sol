/// @file AwstPostPasses.h
/// Option-driven post-AWST work extracted from main.cpp: inline overrides and
/// the new-C() deploy-template artifact.
#pragma once

#include "ArtifactIO.h"
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

/// Invalidate compiler-owned child deployment outputs before invoking the
/// backend, so a missing output can never be satisfied by a previous run.
bool prepareChildDeployArtifacts(
	std::string const& _outputDir,
	std::set<std::string> const& _childContracts,
	std::string& _error);

/// Remove every compiler-owned output for the current deployable target names
/// before the backend runs.
bool prepareBackendTargetArtifacts(
	std::string const& _outputDir,
	AwstRoots const& _roots,
	std::string& _error);

/// Validate and bundle new-C() child binaries into deploy.tmpl.json. Appends
/// hashes for the exact binary inputs and JSON output to `_records`.
bool writeChildDeployTemplates(
	std::string const& _outputDir,
	std::set<std::string> const& _childContracts,
	std::vector<artifact::Record>& _records,
	std::string& _error);

/// Require primary backend outputs and hash every fresh file emitted for the
/// current deployable target names.
bool collectBackendTargetArtifacts(
	std::string const& _outputDir,
	AwstRoots const& _roots,
	std::vector<artifact::Record>& _records,
	std::string& _error);

} // namespace puyasol::cli
