#include "cli/AwstPostPasses.h"
#include "builder/contract/ChildDeployment.h"
#include "json/OptionsWriter.h"
#include "Logger.h"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>

namespace fs = boost::filesystem;
using njson = nlohmann::json;

namespace puyasol::cli
{

void applyInlineOverrides(AwstRoots& _roots, Options const& _opts)
{
	auto& logger = puyasol::Logger::instance();
	auto applyOverrides = [&](auto const& _names, bool _inline,
		std::string const& _option, std::string const& _action)
	{
		if (_names.empty())
			return;

		std::set<std::string> wanted(_names.begin(), _names.end());
		std::set<std::string> hit;
		for (auto& root : _roots)
		{
			if (auto* sub = dynamic_cast<puyasol::awst::Subroutine*>(root.get()))
			{
				if (wanted.count(sub->name))
				{
					sub->inlineOpt = _inline;
					hit.insert(sub->name);
				}
			}
			else if (auto* contract = dynamic_cast<puyasol::awst::Contract*>(root.get()))
			{
				for (auto& m : contract->methods)
				{
					if (wanted.count(m.memberName))
					{
						m.inlineOpt = _inline;
						hit.insert(m.memberName);
					}
				}
			}
		}
		for (auto const& name : wanted)
		{
			if (!hit.count(name))
				logger.warning(
					_option + ": '" + name + "' not found "
					"as Subroutine or ContractMethod in any root");
		}
		if (!hit.empty())
			logger.info(
				_option + ": marked " + std::to_string(hit.size()) + _action);
	};

	applyOverrides(_opts.forceInlineSubs, true,
		"--force-inline-sub", " node(s) for inlining");
	applyOverrides(_opts.forceNoInlineSubs, false,
		"--force-no-inline-sub", " node(s) non-inline");
}

namespace
{

constexpr std::uintmax_t c_programPageBytes = 4096;
constexpr std::uintmax_t c_approvalBytes = c_programPageBytes * 2;

struct BackendTarget
{
	std::string stem;
	std::string id;
	bool logicSig;
};

std::optional<BackendTarget> backendTarget(awst::RootNode const& _root)
{
	if (auto const* contract = dynamic_cast<awst::Contract const*>(&_root))
		return BackendTarget{contract->name, contract->id, false};
	if (auto const* logicSig = dynamic_cast<awst::LogicSignature const*>(&_root))
		return BackendTarget{logicSig->shortName, logicSig->id, true};
	return std::nullopt;
}

bool validArtifactStem(std::string const& _stem)
{
	if (_stem.empty()
		|| !(std::isalpha(static_cast<unsigned char>(_stem.front()))
			|| _stem.front() == '_'))
		return false;
	return std::all_of(_stem.begin() + 1, _stem.end(), [](unsigned char c) {
		return std::isalnum(c) || c == '_';
	});
}

bool belongsToTarget(std::string const& _fileName, std::string const& _stem)
{
	if (!_fileName.starts_with(_stem + "."))
		return false;
	return _fileName.ends_with(".bin")
		|| _fileName.ends_with(".teal")
		|| _fileName.ends_with(".arc32.json")
		|| _fileName.ends_with(".arc56.json")
		|| _fileName.ends_with(".stats.txt")
		|| _fileName.ends_with(".assembly-report")
		|| _fileName.ends_with(".puya.map")
		|| _fileName.ends_with(".ir")
		|| _fileName.ends_with(".mir");
}

bool addRecord(
	std::vector<artifact::Record>& _records,
	artifact::Record _record,
	std::string& _error)
{
	auto existing = std::find_if(_records.begin(), _records.end(),
		[&](artifact::Record const& record) {
			return record.path == _record.path;
		});
	if (existing == _records.end())
	{
		_records.push_back(std::move(_record));
		return true;
	}
	if (existing->digest.bytes != _record.digest.bytes
		|| existing->digest.sha256 != _record.digest.sha256)
	{
		_error = "artifact changed during validation: " + _record.path;
		return false;
	}
	return true;
}

std::string backendRole(std::string const& _fileName)
{
	if (_fileName.ends_with(".bin")) return "backend-bytecode";
	if (_fileName.ends_with(".teal")) return "backend-teal";
	if (_fileName.ends_with(".arc56.json")) return "backend-arc56";
	if (_fileName.ends_with(".json")) return "backend-json";
	if (_fileName.ends_with(".ir") || _fileName.ends_with(".mir"))
		return "backend-ir";
	return "backend-output";
}

std::string toHex(std::vector<std::uint8_t> const& _bytes)
{
	static constexpr char digits[] = "0123456789abcdef";
	std::string result;
	result.resize(_bytes.size() * 2);
	for (std::size_t i = 0; i < _bytes.size(); ++i)
	{
		result[i * 2] = digits[_bytes[i] >> 4];
		result[i * 2 + 1] = digits[_bytes[i] & 0x0f];
	}
	return result;
}

bool readChildProgram(
	fs::path const& _path,
	std::uintmax_t _maxBytes,
	std::vector<std::uint8_t>& _contents,
	artifact::Digest& _digest,
	std::string& _error)
{
	if (!artifact::readBinary(
		_path, _contents, _digest, _error, _maxBytes))
		return false;
	if (_contents.empty())
	{
		_error = "child program is empty: " + _path.string();
		return false;
	}
	return true;
}

} // namespace

bool prepareChildDeployArtifacts(
	std::string const& _outputDir,
	std::set<std::string> const& _childContracts,
	std::string& _error)
{
	auto const outputDir = fs::path(_outputDir);
	if (!artifact::removeFileIfPresent(
		outputDir / "deploy.tmpl.json", _error))
		return false;
	for (auto const& childName: _childContracts)
		for (auto const* suffix: {".approval.bin", ".clear.bin", ".arc56.json"})
			if (!artifact::removeFileIfPresent(
				outputDir / (childName + suffix), _error))
				return false;
	return true;
}

std::optional<BackendTargets> BackendTargets::collect(
	AwstRoots const& _roots,
	std::string& _error)
{
	BackendTargets result;
	for (auto const& root: _roots)
		if (auto target = backendTarget(*root))
		{
			if (!validArtifactStem(target->stem))
			{
				_error = "invalid backend artifact target name: " + target->stem;
				return std::nullopt;
			}
			if (auto [it, inserted] = result.m_stems.emplace(target->stem, target->id); !inserted)
			{
				_error = "backend artifact name collision: " + target->stem
					+ " (" + it->second + " and " + target->id + ")";
				return std::nullopt;
			}
			result.m_ids.push_back(target->id);
			auto suffixes = target->logicSig
				? std::vector<std::string>{".bin", ".teal"}
				: std::vector<std::string>{".approval.bin", ".clear.bin", ".approval.teal", ".clear.teal"};
			for (auto const& suffix: suffixes) result.m_requiredFiles.insert(target->stem + suffix);
		}
	return result;
}

bool BackendTargets::owns(std::string const& filename) const
{
	return std::any_of(m_stems.begin(), m_stems.end(), [&](auto const& target) {
		return belongsToTarget(filename, target.first);
	});
}

bool prepareBackendTargetArtifacts(
	std::string const& _outputDir,
	BackendTargets const& _targets,
	std::string& _error)
{
	auto const outputDir = fs::path(_outputDir);
	boost::system::error_code ec;
	std::vector<fs::path> stalePaths;
	for (fs::directory_iterator it(outputDir, ec), end; !ec && it != end;
		it.increment(ec))
	{
		auto const fileName = it->path().filename().string();
		if (!_targets.owns(fileName))
			continue;
		stalePaths.push_back(it->path());
	}
	if (ec)
	{
		_error = "cannot enumerate backend artifact directory "
			+ outputDir.string() + ": " + ec.message();
		return false;
	}
	for (auto const& path: stalePaths)
		if (!artifact::removeFileIfPresent(path, _error))
			return false;
	return true;
}

bool writeChildDeployTemplates(
	std::string const& _outputDir,
	std::set<std::string> const& _childContracts,
	std::vector<artifact::Record>& _records,
	std::string& _error)
{
	if (_childContracts.empty())
		return true;

	auto const outputDir = fs::path(_outputDir);
	auto const tmplPath = outputDir / "deploy.tmpl.json";
	njson tmpl = njson::object();
	std::vector<artifact::Record> records;
	for (auto const& childName : _childContracts)
	{
		auto const approvalBin = outputDir / (childName + ".approval.bin");
		auto const clearBin = outputDir / (childName + ".clear.bin");
		std::vector<std::uint8_t> approval;
		std::vector<std::uint8_t> clear;
		artifact::Digest approvalDigest;
		artifact::Digest clearDigest;
		if (!readChildProgram(
			approvalBin, c_approvalBytes, approval, approvalDigest, _error)
			|| !readChildProgram(
				clearBin, c_programPageBytes, clear, clearDigest, _error))
			return false;

		auto approvalHex = toHex(approval);
		auto clearHex = toHex(clear);
		auto const pageHex = static_cast<std::size_t>(c_programPageBytes * 2);
		auto page0 = approvalHex.substr(0, std::min(approvalHex.size(), pageHex));
		auto page1 = approvalHex.size() > pageHex
			? approvalHex.substr(pageHex) : std::string();
		if (page0.size() > pageHex || page1.size() > pageHex)
		{
			_error = "child approval program page exceeds 4096 bytes: "
				+ childName;
			return false;
		}
		tmpl["TMPL_APPROVAL_" + childName + "_P0"] = std::move(page0);
		tmpl["TMPL_APPROVAL_" + childName + "_P1"] = std::move(page1);
		tmpl["TMPL_CLEAR_" + childName] = std::move(clearHex);
		auto const schemaPath = outputDir / (childName + ".arc56.json");
		std::vector<std::uint8_t> schemaBytes;
		artifact::Digest schemaDigest;
		if (!artifact::readBinary(schemaPath, schemaBytes, schemaDigest, _error)) return false;
		auto spec = njson::parse(schemaBytes.begin(), schemaBytes.end(), nullptr, false);
		if (!spec.is_object() || !spec.contains("state") || !spec["state"].is_object()
			|| !spec["state"].contains("schema") || !spec["state"]["schema"].is_object())
		{
			_error = "child ARC56 artifact has no state schema: " + childName;
			return false;
		}
		auto const& schema = spec["state"]["schema"];
		for (auto const& field: builder::childSchemaFields)
		{
			if (!schema.contains(field.scope) || !schema[field.scope].is_object()
				|| !schema[field.scope].contains(field.kind)
				|| !schema[field.scope][field.kind].is_number_unsigned()
				|| schema[field.scope][field.kind].get<uint64_t>() > 64)
			{
				_error = "invalid child ARC56 state schema: " + childName;
				return false;
			}
			tmpl["TMPL_CHILD_" + childName + "_" + field.transactionField] = schema[field.scope][field.kind];
		}
		for (auto const* scope: {"global", "local"})
			if (schema[scope]["ints"].get<uint64_t>() + schema[scope]["bytes"].get<uint64_t>()
				> (std::string_view(scope) == "global" ? 64 : 16))
			{
				_error = "child ARC56 state schema exceeds AVM limits: " + childName;
				return false;
			}
		records.push_back({schemaPath.filename().string(), "child-schema", std::move(schemaDigest)});
		records.push_back({
			approvalBin.filename().string(), "child-approval-bytecode",
			std::move(approvalDigest)});
		records.push_back({
			clearBin.filename().string(), "child-clear-bytecode",
			std::move(clearDigest)});
	}
	if (!tmpl.is_object() || tmpl.size() != _childContracts.size() * 7)
	{
		_error = "deployment template failed schema validation";
		return false;
	}
	artifact::Digest templateDigest;
	if (!artifact::writeJsonAtomically(
		tmplPath, tmpl.dump(2) + '\n', templateDigest, _error))
		return false;
	records.push_back({
		tmplPath.filename().string(), "child-deployment-template",
		std::move(templateDigest)});
	_records.insert(_records.end(),
		std::make_move_iterator(records.begin()),
		std::make_move_iterator(records.end()));
	return true;
}

bool collectBackendTargetArtifacts(
	std::string const& _outputDir,
	BackendTargets const& _targets,
	std::vector<artifact::Record>& _records,
	std::string& _error)
{
	auto const outputDir = fs::path(_outputDir);
	boost::system::error_code ec;
	std::set<std::string> foundFiles;
	for (fs::directory_iterator it(outputDir, ec), end; !ec && it != end;
		it.increment(ec))
	{
		auto const fileName = it->path().filename().string();
		if (!_targets.owns(fileName))
			continue;
		std::vector<std::uint8_t> contents;
		artifact::Digest digest;
		if (!artifact::readBinary(it->path(), contents, digest, _error))
			return false;
		if (contents.empty())
		{
			_error = "backend artifact is empty: " + it->path().string();
			return false;
		}
		if (fileName.ends_with(".json"))
		{
			std::string json(contents.begin(), contents.end());
			if (njson::parse(json, nullptr, false).is_discarded())
			{
				_error = "backend emitted invalid JSON: " + it->path().string();
				return false;
			}
		}
		if (!addRecord(_records,
			{fileName, backendRole(fileName), std::move(digest)}, _error))
			return false;
		foundFiles.insert(fileName);
	}
	if (ec)
	{
		_error = "cannot enumerate backend artifacts in " + outputDir.string()
			+ ": " + ec.message();
		return false;
	}
	for (auto const& fileName: _targets.requiredFiles())
		if (!foundFiles.contains(fileName))
		{
			_error = "missing backend artifact "
				+ (outputDir / fileName).string();
			return false;
		}
	return true;
}

bool ArtifactPublisher::begin()
{
	boost::system::error_code error;
	fs::create_directories(m_output, error);
	if (error)
	{
		m_error = "Cannot create artifact output directory: " + error.message();
		return false;
	}
	// Failed compilation must never leave a prior run's commit marker valid.
	if (!artifact::removeFileIfPresent(m_output / "artifact-manifest.json", m_error)
		|| !prepareChildDeployArtifacts(m_options.outputDir, {}, m_error)
		|| (!m_options.legacySourceRewrite && !artifact::removeFileIfPresent(
			m_output / "source-rewrite-manifest.json", m_error))) return false;
	if (m_options.outputLogs && !Logger::instance().setOutputLogFile((m_output / "puya-sol.log").string()))
	{
		m_error = "Cannot open compilation log: " + (m_output / "puya-sol.log").string();
		return false;
	}
	return true;
}

bool ArtifactPublisher::writeFrontend(std::string const& json, AwstRoots const& roots,
	std::set<std::string> const& children)
{
	m_targets = BackendTargets::collect(roots, m_error);
	if (!m_targets || !prepareChildDeployArtifacts(m_options.outputDir, children, m_error)
		|| !prepareBackendTargetArtifacts(m_options.outputDir, *m_targets, m_error)) return false;
	artifact::Digest digest;
	if (!artifact::writeJsonAtomically(awstPath(), json, digest, m_error)) return false;
	m_records.push_back({"awst.json", "frontend-awst", std::move(digest)});
	if (!puyasol::json::OptionsWriter::write(optionsPath(), m_targets->ids(), m_options.outputDir,
		m_options.optimizationLevel, m_options.outputIr, children, digest, m_error)) return false;
	m_records.push_back({"options.json", "backend-options", std::move(digest)});
	if (m_options.legacySourceRewrite)
	{
		std::vector<std::uint8_t> contents;
		if (!artifact::readBinary(m_output / "source-rewrite-manifest.json", contents, digest, m_error)) return false;
		m_records.push_back({"source-rewrite-manifest.json", "source-rewrite-manifest", std::move(digest)});
	}
	return publish(m_options.noPuya ? "frontend-only" : "frontend-ready");
}

bool ArtifactPublisher::finishBackend(std::set<std::string> const& children)
{
	if (!writeChildDeployTemplates(m_options.outputDir, children, m_records, m_error)) return false;
	if (collectBackendTargetArtifacts(m_options.outputDir, m_targets.value(), m_records, m_error)
		&& publish("backend-complete")) return true;
	std::string cleanupError;
	if (!artifact::removeFileIfPresent(m_output / "deploy.tmpl.json", cleanupError))
		Logger::instance().error("Cannot invalidate deployment template: " + cleanupError);
	return false;
}

bool ArtifactPublisher::publish(std::string const& phase)
{
	auto path = m_output / "artifact-manifest.json";
	if (!artifact::writeManifest(path, phase, m_records, m_error)) return false;
	Logger::instance().info("Wrote: " + path.string());
	return true;
}

} // namespace puyasol::cli
