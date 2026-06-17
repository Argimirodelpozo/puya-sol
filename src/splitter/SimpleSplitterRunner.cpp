#include "splitter/SimpleSplitterRunner.h"

#include "splitter/SimpleSplitter.h"

#include "json/AWSTSerializer.h"
#include "json/OptionsWriter.h"
#include "runner/PuyaRunner.h"
#include "Logger.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace puyasol::splitter
{

namespace fs = std::filesystem;
using njson = nlohmann::ordered_json;

namespace {

/// Parse --split-config JSON into per-helper name lists.
/// Accepts `{"extract":[...]}` (single) or `{"helpers":[{"extract":[...]},...]}`.
std::vector<std::vector<std::string>> parseSplitConfig(
	std::string const& _path)
{
	std::vector<std::vector<std::string>> result;
	std::ifstream cf(_path);
	if (!cf.is_open())
	{
		Logger::instance().error("Cannot open --split-config file: " + _path);
		return result;
	}
	std::ostringstream ss;
	ss << cf.rdbuf();
	try
	{
		auto cfg = njson::parse(ss.str());
		if (cfg.contains("helpers") && cfg["helpers"].is_array())
		{
			for (auto const& h: cfg["helpers"])
			{
				std::vector<std::string> names;
				if (h.contains("extract") && h["extract"].is_array())
					for (auto const& e: h["extract"])
						if (e.is_string())
							names.push_back(e.get<std::string>());
				if (!names.empty()) result.push_back(std::move(names));
			}
			size_t total = 0;
			for (auto const& v: result) total += v.size();
			Logger::instance().info(
				"Loaded " + std::to_string(total) + " extraction name(s) across "
				+ std::to_string(result.size()) + " helper(s) from " + _path);
		}
		else if (cfg.contains("extract") && cfg["extract"].is_array())
		{
			std::vector<std::string> names;
			for (auto const& e: cfg["extract"])
				if (e.is_string())
					names.push_back(e.get<std::string>());
			if (!names.empty())
			{
				Logger::instance().info(
					"Loaded " + std::to_string(names.size()) +
					" extraction name(s) from " + _path);
				result.push_back(std::move(names));
			}
		}
		else
		{
			Logger::instance().error(
				"--split-config: expected {\"extract\": [...]} or "
				"{\"helpers\": [{\"extract\": [...]}, ...]}");
		}
	}
	catch (std::exception const& e)
	{
		Logger::instance().error(
			"Failed to parse --split-config: " + std::string(e.what()));
	}
	return result;
}

/// All Subroutine.name + ContractMethod.memberName in the roots.
/// Used to validate --force-delegate names and filter each pass's spec.
std::set<std::string> collectAllNames(
	std::vector<std::shared_ptr<awst::RootNode>> const& _roots)
{
	std::set<std::string> all;
	for (auto const& r: _roots)
	{
		if (auto sub = std::dynamic_pointer_cast<awst::Subroutine>(r))
			all.insert(sub->name);
		else if (auto c = std::dynamic_pointer_cast<awst::Contract>(r))
			for (auto const& m: c->methods)
				all.insert(m.memberName);
	}
	return all;
}

} // namespace

SimpleSplitterRunner::Result SimpleSplitterRunner::run(
	Config const& _cfg,
	std::vector<std::shared_ptr<awst::RootNode>> const& _roots)
{
	Result result;
	auto& logger = Logger::instance();

	// 1. Build the helper-spec list from --split-config + --force-delegate.
	std::vector<std::vector<std::string>> helperSpecs;
	if (!_cfg.splitConfigPath.empty())
	{
		helperSpecs = parseSplitConfig(_cfg.splitConfigPath);
		if (helperSpecs.empty() && _cfg.forceDelegate.empty())
			return result;  // parse error already logged
	}

	if (!_cfg.forceDelegate.empty())
	{
		// Each delegate name gets its own one-method helper (not compiled
		// through puya — body has unresolved InstanceMethodTarget refs;
		// artifact is replaced by a hand-crafted lonely chunk later).
		auto presentAll = collectAllNames(_roots);
		int delegateCount = 0;
		for (auto const& name: _cfg.forceDelegate)
		{
			if (!presentAll.count(name))
			{
				logger.warning(
					"--force-delegate: '" + name + "' not found in AWST, "
					"skipping");
				continue;
			}
			helperSpecs.push_back({name});
			delegateCount++;
		}
		if (delegateCount)
			logger.info(
				"--force-delegate: " + std::to_string(delegateCount)
				+ " function(s) routed to dedicated sidecar helpers "
				"(one per F)");
	}

	if (helperSpecs.empty())
		return result;  // nothing to do — caller stays on its single-contract path

	// 2. Run SimpleSplitter per spec, threading the orch's remaining roots forward.
	std::vector<SimpleSplitter::ContractAWST> splitContracts;
	auto currentRoots = _roots;
	int helperIdx = 1;
	for (auto const& names: helperSpecs)
	{
		auto present = collectAllNames(currentRoots);
		std::vector<std::string> toExtract;
		for (auto const& name: names)
			if (present.count(name))
				toExtract.push_back(name);
		if (toExtract.empty()) continue;

		logger.info(
			"SimpleSplitter: extracting " + std::to_string(toExtract.size())
			+ " function(s) into helper #" + std::to_string(helperIdx));
		SimpleSplitter splitter;
		auto parts = splitter.split(
			currentRoots, toExtract, helperIdx, _cfg.ensureBudget);
		if (parts.empty())
		{
			logger.warning(
				"SimpleSplitter pass " + std::to_string(helperIdx)
				+ " returned no result; halting further splits");
			break;
		}
		// parts[0] = helper, parts[1] = orchestrator.
		splitContracts.push_back(parts.front());
		currentRoots = parts.back().roots;
		helperIdx++;
	}

	if (splitContracts.empty())
		return result;  // no helper actually had a match — caller's normal path

	// 3. Stamp the final orchestrator onto the end of splitContracts.
	{
		SimpleSplitter::ContractAWST orch;
		fs::path srcPath(_cfg.sourceFile);
		std::string sourceStem = srcPath.stem().string();
		std::shared_ptr<awst::Contract> stemMatch;
		std::shared_ptr<awst::Contract> lastMatch;
		for (auto const& r: currentRoots)
		{
			if (auto c = std::dynamic_pointer_cast<awst::Contract>(r))
			{
				if (c->name.find("__Helper") != std::string::npos) continue;
				lastMatch = c;
				if (c->name == sourceStem) stemMatch = c;
			}
		}
		auto chosen = stemMatch ? stemMatch : lastMatch;
		if (chosen)
		{
			orch.contractId = chosen->id;
			orch.contractName = chosen->name;
		}
		orch.roots = std::move(currentRoots);
		splitContracts.push_back(std::move(orch));
	}

	// 4. Emit per-contract subdirs (helpers first, orchestrator last).
	puyasol::json::AWSTSerializer serializer;
	fs::create_directories(_cfg.outputDir);

	// Each options.json declares all OTHER helpers as int template vars
	// so puya admits `TMPL_<helperName>_APP_ID` cross-references.
	std::set<std::string> helperNames;
	for (auto const& cawst: splitContracts)
	{
		bool isOrch = false;
		for (auto const& r: cawst.roots)
			if (auto c = std::dynamic_pointer_cast<awst::Contract>(r))
				if (c->id == cawst.contractId && c->name == cawst.contractName
					&& cawst.contractName.find("__Helper") == std::string::npos)
					isOrch = true;
		if (!isOrch) helperNames.insert(cawst.contractName);
	}

	// Delegate helpers are skipped by puya — replaced by hand-crafted lonely-chunk TEAL.
	std::set<std::string> delegateFunctionNames(
		_cfg.forceDelegate.begin(), _cfg.forceDelegate.end());
	std::set<std::string> delegateHelperContractNames;
	for (auto const& cawst: splitContracts)
	{
		if (cawst.contractName.find("__Helper") == std::string::npos) continue;
		for (auto const& r: cawst.roots)
			if (auto c = std::dynamic_pointer_cast<awst::Contract>(r))
				for (auto const& m: c->methods)
					if (delegateFunctionNames.count(m.memberName))
						delegateHelperContractNames.insert(cawst.contractName);
	}

	puyasol::runner::PuyaRunner runner;
	runner.setPuyaPath(_cfg.puyaPath);
	int aggregateExitCode = 0;
	for (auto const& cawst: splitContracts)
	{
		fs::path subdir = fs::path(_cfg.outputDir) / cawst.contractName;
		fs::create_directories(subdir);

		auto subJson = serializer.serialize(cawst.roots);
		std::string subAwstPath = (subdir / "awst.json").string();
		{
			std::ofstream out(subAwstPath);
			out << subJson.dump(2) << std::endl;
			logger.info("Wrote: " + subAwstPath);
		}
		// template_vars_prefix="TMPL_" → var key is `<helperName>_APP_ID`
		// (without the prefix); placeholder 0, deploy-time substitution applies.
		std::map<std::string, int64_t> intVars;
		for (auto const& h: helperNames)
			if (h != cawst.contractName)
				intVars[h + "_APP_ID"] = 0;
		std::string subOptionsPath = (subdir / "options.json").string();
		puyasol::json::OptionsWriter::write(
			subOptionsPath, cawst.contractId, subdir.string(),
			_cfg.optimizationLevel, _cfg.outputIr, {}, intVars);
		logger.info("Wrote: " + subOptionsPath);

		if (!_cfg.noPuya
			&& !delegateHelperContractNames.count(cawst.contractName))
		{
			logger.info("Invoking puya backend for '" + cawst.contractName + "'...");
			int exitCode = runner.run(subAwstPath, subOptionsPath, _cfg.logLevel);
			if (exitCode != 0)
				aggregateExitCode = exitCode;
		}
	}

	result.didSplit = true;
	result.puyaExitCode = aggregateExitCode;
	return result;
}

} // namespace puyasol::splitter
