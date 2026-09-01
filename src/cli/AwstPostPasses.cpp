#include "cli/AwstPostPasses.h"
#include "Logger.h"

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
