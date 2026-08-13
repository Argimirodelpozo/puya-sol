#include "cli/AwstPostPasses.h"
#include "Logger.h"
#include "experimental/splitter/FunctionSplitter.h"
#include "experimental/splitter/SimpleSplitterRunner.h"

#include <boost/filesystem.hpp>

#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>

namespace fs = boost::filesystem;
using njson = nlohmann::ordered_json;

namespace puyasol::cli
{

void applyInlineOverrides(AwstRoots& _roots, Options const& _opts)
{
	auto& logger = puyasol::Logger::instance();

	// --force-inline-sub: set inlineOpt=true on matching Subroutine / ContractMethod nodes.
	if (!_opts.forceInlineSubs.empty())
	{
		std::set<std::string> wanted(
			_opts.forceInlineSubs.begin(), _opts.forceInlineSubs.end());
		std::set<std::string> hit;
		for (auto& root : _roots)
		{
			if (auto* sub = dynamic_cast<puyasol::awst::Subroutine*>(root.get()))
			{
				if (wanted.count(sub->name))
				{
					sub->inlineOpt = true;
					hit.insert(sub->name);
				}
			}
			else if (auto* contract = dynamic_cast<puyasol::awst::Contract*>(root.get()))
			{
				for (auto& m : contract->methods)
				{
					if (wanted.count(m.memberName))
					{
						m.inlineOpt = true;
						hit.insert(m.memberName);
					}
				}
			}
		}
		for (auto const& name : wanted)
		{
			if (!hit.count(name))
				logger.warning(
					"--force-inline-sub: '" + name + "' not found "
					"as Subroutine or ContractMethod in any root");
		}
		if (!hit.empty())
			logger.info(
				"--force-inline-sub: marked " + std::to_string(hit.size())
				+ " node(s) for inlining");
	}

	// --force-no-inline-sub: set inlineOpt=false so a single-call sub stays a real
	// callsub — keeps it as a --fn-split boundary and limits its body to one chunk.
	if (!_opts.forceNoInlineSubs.empty())
	{
		std::set<std::string> wanted(
			_opts.forceNoInlineSubs.begin(), _opts.forceNoInlineSubs.end());
		std::set<std::string> hit;
		for (auto& root : _roots)
		{
			if (auto* sub = dynamic_cast<puyasol::awst::Subroutine*>(root.get()))
			{
				if (wanted.count(sub->name))
				{
					sub->inlineOpt = false;
					hit.insert(sub->name);
				}
			}
			else if (auto* contract = dynamic_cast<puyasol::awst::Contract*>(root.get()))
			{
				for (auto& m : contract->methods)
					if (wanted.count(m.memberName))
					{
						m.inlineOpt = false;
						hit.insert(m.memberName);
					}
			}
		}
		for (auto const& name : wanted)
			if (!hit.count(name))
				logger.warning(
					"--force-no-inline-sub: '" + name + "' not found "
					"as Subroutine or ContractMethod in any root");
		if (!hit.empty())
			logger.info(
				"--force-no-inline-sub: marked " + std::to_string(hit.size())
				+ " node(s) non-inline");
	}
}

void applyFnSplits(AwstRoots& _roots, Options const& _opts)
{
	if (_opts.fnSplits.empty())
		return;

	auto& logger = puyasol::Logger::instance();

	// Pieces are appended to roots; the original subroutine is left in place.
	std::vector<puyasol::splitter::FunctionSplitter::PieceSpec> specs;
	for (auto const& fnSpec : _opts.fnSplits)
	{
		puyasol::splitter::FunctionSplitter::PieceSpec ps;
		ps.subroutineName = fnSpec.subroutineName;
		ps.splitPoints = fnSpec.splitPoints;
		ps.groupId = fnSpec.groupId;
		ps.crossChunk = fnSpec.crossChunk;
		specs.push_back(std::move(ps));
	}
	puyasol::splitter::FunctionSplitter functionSplitter;
	auto fsResult = functionSplitter.splitAt(_roots, specs);
	if (fsResult.didSplit)
		logger.info(
			"--fn-split: emitted " +
			std::to_string(
				fsResult.newSubroutines.size() +
				fsResult.newContractMethodPieces) +
			" piece(s) across " +
			std::to_string(fsResult.splitFunctions.size()) +
			" function(s)");

	// chain_groups.json: records cross-chunk chains for the deploy harness.
	// Non-cross pieces are in-program callsubs and don't need registration.
	// Harness reads this with deploy.uros.json to call
	// orch.register_chunk_method_chain for each group at deploy time.
	bool anyCross = false;
	for (auto const& fnSpec : _opts.fnSplits)
		if (fnSpec.crossChunk) { anyCross = true; break; }
	if (anyCross)
	{
		fs::create_directories(_opts.outputDir);
		njson chainsDoc;
		njson groupsArr = njson::array();
		for (auto const& fnSpec : _opts.fnSplits)
		{
			if (!fnSpec.crossChunk) continue;
			njson g;
			g["primary_method"] = fnSpec.subroutineName;
			g["group_id"] = fnSpec.groupId;
			njson piecesArr = njson::array();
			size_t numPieces = fnSpec.splitPoints.size() + 1;
			for (size_t pi = 0; pi < numPieces; ++pi)
			{
				piecesArr.push_back(
					fnSpec.subroutineName + "__piece_"
					+ std::to_string(pi) + "_g"
					+ std::to_string(fnSpec.groupId));
			}
			g["piece_methods"] = std::move(piecesArr);
			groupsArr.push_back(std::move(g));
		}
		chainsDoc["groups"] = std::move(groupsArr);
		std::string chainGroupsPath =
			(fs::path(_opts.outputDir) / "chain_groups.json").string();
		std::ofstream cgout(chainGroupsPath);
		cgout << chainsDoc.dump(2) << std::endl;
		logger.info("Wrote: " + chainGroupsPath);
	}
}

splitter::PureHelperExtractor::Result extractPureHelpers(
	AwstRoots& _roots, Options const& _opts)
{
	auto& logger = puyasol::Logger::instance();

	puyasol::splitter::PureHelperExtractor::Result pureHelperResult;
	if (_opts.deployPureHelpers)
	{
		puyasol::splitter::PureHelperExtractor ex;
		std::vector<puyasol::splitter::PureHelperExtractor::HelperSplitSpec>
			splitSpecs;
		for (auto const& s : _opts.pureHelperSplits)
			splitSpecs.push_back({s.subroutineName, s.splitPoints});
		pureHelperResult = ex.extract(_roots, splitSpecs);
	}
	// pure_helpers.json: lists helper Contracts for the deploy harness to
	// deploy as standalone apps and substitute TMPL_* vars into TEAL.
	if (pureHelperResult.didExtract)
	{
		fs::create_directories(_opts.outputDir);
		njson helpersDoc;
		njson arr = njson::array();
		for (auto const& h : pureHelperResult.extracted)
		{
			// Bare name = last dotted segment of the full contract id.
			auto dot = h.helperContractId.find_last_of('.');
			std::string bareName = (dot == std::string::npos)
				? h.helperContractId
				: h.helperContractId.substr(dot + 1);
			njson e;
			e["template_var"] = h.templateVarName;
			e["contract_name"] = bareName;
			arr.push_back(std::move(e));
		}
		helpersDoc["helpers"] = std::move(arr);
		std::string pureHelpersPath =
			(fs::path(_opts.outputDir) / "pure_helpers.json").string();
		std::ofstream phout(pureHelpersPath);
		phout << helpersDoc.dump(2) << std::endl;
		logger.info("Wrote: " + pureHelpersPath);
	}
	return pureHelperResult;
}

std::optional<int> runSimpleSplitterIfRequested(
	AwstRoots& _roots, Options const& _opts, std::string const& _sourceFile)
{
	if (_opts.splitConfig.empty() && _opts.forceDelegate.empty())
		return std::nullopt;

	auto& logger = puyasol::Logger::instance();

	puyasol::splitter::SimpleSplitterRunner::Config cfg;
	cfg.splitConfigPath = _opts.splitConfig;
	cfg.forceDelegate = _opts.forceDelegate;
	cfg.ensureBudget = _opts.ensureBudget;
	cfg.outputDir = _opts.outputDir;
	cfg.puyaPath = _opts.puyaPath;
	cfg.logLevel = _opts.logLevel;
	cfg.optimizationLevel = _opts.optimizationLevel;
	cfg.outputIr = _opts.outputIr;
	cfg.noPuya = _opts.noPuya;
	cfg.sourceFile = _sourceFile;
	puyasol::splitter::SimpleSplitterRunner runner;
	auto result = runner.run(cfg, _roots);
	if (result.didSplit)
	{
		if (logger.warningCount() > 0)
			logger.info("Completed with " + std::to_string(
				logger.warningCount()) + " warning(s)");
		return result.puyaExitCode;
	}
	return std::nullopt;
}

void writeChildDeployTemplates(
	std::string const& _outputDir,
	std::set<std::string> const& _childContracts)
{
	if (_childContracts.empty())
		return;

	auto& logger = puyasol::Logger::instance();

	std::string tmplPath = (fs::path(_outputDir) / "deploy.tmpl.json").string();
	njson tmpl = njson::object();
	for (auto const& childName : _childContracts)
	{
		// Read child's compiled binaries from the output dir.
		auto approvalBin = fs::path(_outputDir) / (childName + ".approval.bin");
		auto clearBin = fs::path(_outputDir) / (childName + ".clear.bin");
		if (fs::exists(approvalBin))
		{
			std::ifstream af(approvalBin, std::ios::binary);
			std::vector<uint8_t> ab((std::istreambuf_iterator<char>(af)),
				std::istreambuf_iterator<char>());
			std::string hex;
			for (auto b : ab)
			{
				char buf[3];
				snprintf(buf, sizeof(buf), "%02x", b);
				hex += buf;
			}
			tmpl["TMPL_APPROVAL_" + childName] = hex;
		}
		if (fs::exists(clearBin))
		{
			std::ifstream cf(clearBin, std::ios::binary);
			std::vector<uint8_t> cb((std::istreambuf_iterator<char>(cf)),
				std::istreambuf_iterator<char>());
			std::string hex;
			for (auto b : cb)
			{
				char buf[3];
				snprintf(buf, sizeof(buf), "%02x", b);
				hex += buf;
			}
			tmpl["TMPL_CLEAR_" + childName] = hex;
		}
	}
	std::ofstream tf(tmplPath);
	tf << tmpl.dump(2);
	logger.info("Wrote: " + tmplPath);
}

} // namespace puyasol::cli
