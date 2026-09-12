/// @file AwstPostPasses.h
/// Option-driven post-AWST work extracted from main.cpp: inline overrides and
/// the new-C() deploy-template artifact.
#pragma once

#include "ArtifactIO.h"
#include "awst/Node.h"
#include "cli/CliOptions.h"

#include <memory>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace puyasol::cli
{

using AwstRoots = std::vector<std::shared_ptr<awst::RootNode>>;

/// Flip inlineOpt on matching nodes.
void applyInlineOverrides(AwstRoots& _roots, Options const& _opts);

/// Validate target identities once, before deleting or publishing their files.
class BackendTargets
{
public:
	static std::optional<BackendTargets> collect(AwstRoots const& roots, std::string& error);
	std::vector<std::string> const& ids() const { return m_ids; }
	std::set<std::string> const& requiredFiles() const { return m_requiredFiles; }
	bool owns(std::string const& filename) const;
private:
	std::vector<std::string> m_ids;
	std::map<std::string, std::string> m_stems;
	std::set<std::string> m_requiredFiles;
};

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
	BackendTargets const& _targets,
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
	BackendTargets const& _targets,
	std::vector<artifact::Record>& _records,
	std::string& _error);

/// Publication state for a single run. The manifest is its phase commit marker.
class ArtifactPublisher
{
public:
	explicit ArtifactPublisher(Options const& options): m_options(options), m_output(options.outputDir) {}
	bool begin();
	bool writeFrontend(std::string const& json, AwstRoots const& roots,
		std::set<std::string> const& children);
	bool finishBackend(std::set<std::string> const& children);
	boost::filesystem::path awstPath() const { return m_output / "awst.json"; }
	boost::filesystem::path optionsPath() const { return m_output / "options.json"; }
	std::string const& error() const { return m_error; }
private:
	bool publish(std::string const& phase);
	Options const& m_options;
	boost::filesystem::path m_output;
	std::optional<BackendTargets> m_targets;
	std::vector<artifact::Record> m_records;
	std::string m_error;
};

} // namespace puyasol::cli
