#include "experimental/splitter/SimpleSplitterRunner.h"

#include "experimental/splitter/SimpleSplitter.h"
#include "experimental/splitter/AwstWalker.h"

#include "json/AWSTSerializer.h"
#include "json/OptionsWriter.h"
#include "runner/PuyaRunner.h"
#include "Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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

/// Parse --split-config JSON into ordinary helper lists plus state-preserving
/// delegate-page method names.
std::vector<std::vector<std::string>> parseSplitConfig(
	std::string const& _path, std::vector<std::string>& _delegates)
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
		bool recognized = false;
		if (cfg.contains("helpers") && cfg["helpers"].is_array())
		{
			recognized = true;
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
			recognized = true;
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
		if (cfg.contains("delegate") && cfg["delegate"].is_array())
		{
			recognized = true;
			for (auto const& e: cfg["delegate"])
				if (e.is_string())
					_delegates.push_back(e.get<std::string>());
			Logger::instance().info(
				"Loaded " + std::to_string(_delegates.size())
				+ " delegate-page method name(s) from " + _path);
		}
		if (!recognized)
		{
			Logger::instance().error(
				"--split-config: expected extract, helpers, and/or delegate arrays");
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

/// Only externally routable ABI methods can be paged: the call boundary can
/// select those before execution. Internal subroutines cannot be intercepted
/// without changing their caller/storage context.
std::set<std::string> collectDelegatableNames(
	std::vector<std::shared_ptr<awst::RootNode>> const& _roots)
{
	std::set<std::string> all;
	for (auto const& r: _roots)
		if (auto c = std::dynamic_pointer_cast<awst::Contract>(r))
		{
			// ARC4-profile public methods carry an ABI config.  EVM-profile
			// public methods deliberately do not (the generated Solidity entry
			// adapter owns routing), so discover those from direct approval-
			// program method edges.  This is structural and remains valid for
			// overloads/new route shapes; it does not special-case method names.
			std::set<std::string> approvalTargets;
			if (c->approvalProgram.body)
			{
				auto view = *c->approvalProgram.body;
				walkBlock(view, [&](awst::Expression const& expr)
					-> std::shared_ptr<awst::Expression>
				{
					auto const* call = dynamic_cast<
						awst::SubroutineCallExpression const*>(&expr);
					if (!call) return nullptr;
					if (auto const* target = std::get_if<
						awst::InstanceMethodTarget>(&call->target))
						approvalTargets.insert(target->memberName);
					return nullptr;
				});
			}
			for (auto const& m: c->methods)
				if ((m.arc4MethodConfig
					&& std::holds_alternative<awst::ARC4ABIMethodConfig>(
						*m.arc4MethodConfig))
					|| approvalTargets.count(m.memberName))
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

	// 1. Ordinary helpers use inner app calls. Delegate pages are kept
	// separate because each must be compiled from the same unsliced roots.
	std::vector<std::vector<std::string>> helperSpecs;
	std::vector<std::string> requestedDelegates = _cfg.forceDelegate;
	if (!_cfg.splitConfigPath.empty())
	{
		helperSpecs = parseSplitConfig(
			_cfg.splitConfigPath, requestedDelegates);
		if (helperSpecs.empty() && requestedDelegates.empty())
			return result;  // parse error already logged
	}

	std::vector<std::string> delegateNames;
	if (!requestedDelegates.empty())
	{
		auto presentAll = collectDelegatableNames(_roots);
		for (auto const& name: requestedDelegates)
		{
			if (!presentAll.count(name))
			{
				logger.warning(
					"--force-delegate: '" + name + "' is not an externally "
					"routable ABI method, skipping");
				continue;
			}
			if (std::find(delegateNames.begin(), delegateNames.end(), name)
				== delegateNames.end())
				delegateNames.push_back(name);
		}
		if (!delegateNames.empty())
			logger.info(
				"--force-delegate: " + std::to_string(delegateNames.size())
				+ " externally routable method(s) compiled as dedicated "
				+ "state-preserving code pages");
	}

	if (helperSpecs.empty() && delegateNames.empty())
		return result;  // nothing to do — caller stays on its single-contract path

	// 2. Run ordinary extraction, threading the orchestrator forward.
	std::vector<SimpleSplitter::ContractAWST> splitContracts;
	std::vector<std::pair<std::string, std::string>> delegateEntries;
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
			currentRoots, toExtract, helperIdx, _cfg.ensureBudget, false);
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

	// 3. Compile every code page from the same post-ordinary-split roots, so
	// paged methods that call one another retain the original callee bodies.
	// Independently apply each matching stub to the progressive main roots.
	auto const delegateSourceRoots = currentRoots;
	for (auto const& name: delegateNames)
	{
		SimpleSplitter splitter;
		auto pageParts = splitter.split(
			delegateSourceRoots, {name}, helperIdx, _cfg.ensureBudget, true);
		if (pageParts.empty())
		{
			logger.warning("--force-delegate: failed to build page for '"
				+ name + "', skipping");
			continue;
		}
		auto mainParts = splitter.split(
			currentRoots, {name}, helperIdx, _cfg.ensureBudget, true);
		if (mainParts.empty())
		{
			logger.warning("--force-delegate: failed to stub main route for '"
				+ name + "', skipping");
			continue;
		}
		splitContracts.push_back(pageParts.front());
		delegateEntries.emplace_back(name, pageParts.front().contractName);
		currentRoots = std::move(mainParts.back().roots);
		helperIdx++;
	}

	if (splitContracts.empty())
		return result;  // no helper actually had a match — caller's normal path

	// 4. Stamp the final orchestrator onto the end of splitContracts.
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

	// 5. Emit per-contract subdirs (helpers first, orchestrator last).
	puyasol::json::AWSTSerializer serializer;
	fs::create_directories(_cfg.outputDir);
	if (!delegateEntries.empty())
	{
		njson doc;
		njson entries = njson::array();
		for (auto const& [method, contract]: delegateEntries)
			entries.push_back({{"method", method}, {"contract_name", contract}});
		doc["delegates"] = std::move(entries);
		std::string path =
			(fs::path(_cfg.outputDir) / "delegate_helpers.json").string();
		std::ofstream out(path);
		out << doc.dump(2) << std::endl;
		logger.info("Wrote: " + path);
	}

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

		if (!_cfg.noPuya)
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
