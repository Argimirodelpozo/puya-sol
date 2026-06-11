#include "cli/AwstPostPasses.h"
#include "Logger.h"
#include "builder/sol-ast/calls/SolNewExpression.h"
#include "splitter/FunctionSplitter.h"
#include "splitter/SimpleSplitterRunner.h"

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

	// ─── --force-inline-sub: flip inlineOpt=true on matching nodes ──────
	// We mutate inlineOpt on Subroutine root nodes AND on each Contract's
	// methods (ContractMethod) — both have the field; puya treats them
	// the same way (inline at every call site).
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

	// ─── --force-no-inline-sub: flip inlineOpt=false on matching nodes ──────
	// Inverse of --force-inline-sub: keep a single-call subroutine/method a real
	// callsub (NOT inlined) so --fn-split can use the call as a slice boundary
	// and the body lives in exactly one piece's chunk.
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

	// Pieces are appended to roots as additional Subroutine nodes; the
	// original subroutine is left in place (callers can still callsub it
	// normally if they're not going through the orch dance).
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

	// chain_groups.json: small artifact that records which split
	// targets are cross-chunk chains. Only crossChunk specs make it
	// in — non-cross pieces are in-program callsubs and don't need
	// orch-side registration. The deploy harness reads this together
	// with deploy.uros.json: for each group it finds the chunk_idx
	// hosting each piece (by name), pulls the piece's ARC4 selector
	// from that chunk's arc56.json, packs (chunk_app_id, selector)
	// entries, and calls orch.register_chunk_method_chain at deploy
	// time so user calls to orch.dispatch_chain(primary_selector,...)
	// can fan out across the chain.
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
	// pure_helpers.json: small artifact the deploy harness reads to
	// (a) enumerate the synthesized helper Contracts, (b) deploy each
	// as a standalone app, (c) substitute the corresponding TMPL_*
	// variables into main + chunk TEAL at deploy time. Emitted under
	// the contract output dir so it sits beside deploy.uros.json.
	if (pureHelperResult.didExtract)
	{
		fs::create_directories(_opts.outputDir);
		njson helpersDoc;
		njson arr = njson::array();
		for (auto const& h : pureHelperResult.extracted)
		{
			// Helper Contract's emitted file prefix is its bare name
			// (last dotted segment); recover from the full id the same
			// way buildHelperContract does.
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

void writeChildDeployTemplates(std::string const& _outputDir)
{
	auto const& children = puyasol::builder::sol_ast::SolNewExpression::childContracts();
	if (children.empty())
		return;

	auto& logger = puyasol::Logger::instance();

	std::string tmplPath = (fs::path(_outputDir) / "deploy.tmpl.json").string();
	njson tmpl = njson::object();
	for (auto const& childName : children)
	{
		// Read the child's compiled binaries from the output dir
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
	puyasol::builder::sol_ast::SolNewExpression::resetChildContracts();
}

} // namespace puyasol::cli
