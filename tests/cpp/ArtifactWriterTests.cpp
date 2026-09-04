/// Regression coverage for transactional compiler artifacts (audit M-03).
#include "ArtifactIO.h"
#include "cli/AwstPostPasses.h"
#include "json/OptionsWriter.h"

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = boost::filesystem;
using njson = nlohmann::json;

namespace
{

bool require(bool _condition, std::string const& _message)
{
	if (_condition)
		return true;
	std::cerr << "FAIL: " << _message << '\n';
	return false;
}

void writeBytes(fs::path const& _path, std::vector<std::uint8_t> const& _bytes)
{
	std::ofstream out(_path.string(), std::ios::binary | std::ios::trunc);
	if (!_bytes.empty())
		out.write(reinterpret_cast<char const*>(_bytes.data()),
			static_cast<std::streamsize>(_bytes.size()));
}

void writeText(fs::path const& _path, std::string const& _text)
{
	std::ofstream out(_path.string(), std::ios::binary | std::ios::trunc);
	out << _text;
}

std::string readText(fs::path const& _path)
{
	std::ifstream in(_path.string(), std::ios::binary);
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool noTemporaryArtifacts(fs::path const& _directory)
{
	for (fs::directory_iterator it(_directory), end; it != end; ++it)
		if (it->path().filename().string().find(".tmp-") != std::string::npos)
			return false;
	return true;
}

} // namespace

int main()
{
	bool ok = true;
	auto const tempDir = fs::temp_directory_path()
		/ fs::unique_path("puya-sol-artifacts-%%%%%%%%");
	boost::system::error_code ec;
	fs::create_directories(tempDir, ec);
	if (ec)
	{
		std::cerr << "failed to create test directory: " << ec.message() << '\n';
		return 1;
	}

	std::string error;
	puyasol::artifact::Digest digest;
	ok &= require(puyasol::artifact::removeFileIfPresent(
		tempDir / "never-existed.json", error),
		"invalidating a fresh output directory must accept absent files");
	auto const atomicPath = tempDir / "atomic.json";
	writeText(atomicPath, "{\"old\":true}\n");
	ok &= require(!puyasol::artifact::writeJsonAtomically(
		atomicPath, "{not-json", digest, error),
		"invalid JSON must be rejected");
	ok &= require(readText(atomicPath) == "{\"old\":true}\n",
		"a rejected write must preserve the prior artifact");
	error.clear();
	auto const replacement = std::string("{\"new\":true}\n");
	ok &= require(puyasol::artifact::writeJsonAtomically(
		atomicPath, replacement, digest, error),
		"valid JSON must atomically replace an artifact: " + error);
	ok &= require(readText(atomicPath) == replacement,
		"atomic replacement must preserve exact bytes");
	ok &= require(digest.bytes == replacement.size()
		&& digest.sha256.size() == 64,
		"atomic writes must return a size and SHA-256 digest");

	auto const optionsPath = tempDir / "options.json";
	puyasol::artifact::Digest optionsDigest;
	error.clear();
	ok &= require(puyasol::json::OptionsWriter::write(
		optionsPath, {"a.C", "b.D"}, tempDir.string(), 2, true, {"D"}, {},
		optionsDigest, error),
		"multi-target options must write successfully: " + error);
	auto options = njson::parse(readText(optionsPath));
	ok &= require(options["compilation_set"].size() == 2
		&& options["cli_template_definitions"].size() == 3,
		"options schema must contain every target and child template variable");
	auto const optionsBefore = readText(optionsPath);
	error.clear();
	ok &= require(!puyasol::json::OptionsWriter::write(
		optionsPath, {}, tempDir.string(), 1, false, {}, {}, optionsDigest, error),
		"an empty compilation set must be rejected");
	ok &= require(readText(optionsPath) == optionsBefore,
		"invalid options must not replace the last valid file");

	auto const approvalPath = tempDir / "Child.approval.bin";
	auto const clearPath = tempDir / "Child.clear.bin";
	auto const deployPath = tempDir / "deploy.tmpl.json";
	writeText(deployPath, "stale");
	writeBytes(approvalPath, {1});
	writeBytes(clearPath, {2});
	error.clear();
	ok &= require(puyasol::cli::prepareChildDeployArtifacts(
		tempDir.string(), {"Child"}, error),
		"deployment preparation must succeed: " + error);
	ok &= require(!fs::exists(deployPath) && !fs::exists(approvalPath)
		&& !fs::exists(clearPath),
		"deployment preparation must remove every stale input and output");

	std::vector<puyasol::artifact::Record> records;
	error.clear();
	ok &= require(!puyasol::cli::writeChildDeployTemplates(
		tempDir.string(), {"Child"}, records, error),
		"missing child binaries must fail template generation");
	ok &= require(!fs::exists(deployPath),
		"missing child binaries must not publish a template");

	writeBytes(approvalPath, std::vector<std::uint8_t>(8193, 0xaa));
	writeBytes(clearPath, {1});
	error.clear();
	ok &= require(!puyasol::cli::writeChildDeployTemplates(
		tempDir.string(), {"Child"}, records, error),
		"an approval program larger than two pages must fail");
	ok &= require(!fs::exists(deployPath),
		"an oversized approval program must not publish a template");

	std::vector<std::uint8_t> approval(8192);
	for (std::size_t i = 0; i < approval.size(); ++i)
		approval[i] = static_cast<std::uint8_t>(i);
	writeBytes(approvalPath, approval);
	writeBytes(clearPath, std::vector<std::uint8_t>(4096, 0x5a));
	error.clear();
	ok &= require(puyasol::cli::writeChildDeployTemplates(
		tempDir.string(), {"Child"}, records, error),
		"two full approval pages and one full clear page must pass: " + error);
	auto deploy = njson::parse(readText(deployPath));
	ok &= require(deploy.size() == 3
		&& deploy["TMPL_APPROVAL_Child_P0"].get<std::string>().size() == 8192
		&& deploy["TMPL_APPROVAL_Child_P1"].get<std::string>().size() == 8192
		&& deploy["TMPL_CLEAR_Child"].get<std::string>().size() == 8192,
		"deployment template must enforce and preserve both page boundaries");
	ok &= require(records.size() == 3,
		"deployment generation must hash both inputs and its output");

	auto contract = std::make_shared<puyasol::awst::Contract>();
	contract->id = "fixture.Target";
	contract->name = "Target";
	puyasol::cli::AwstRoots roots{contract};
	writeText(tempDir / "Target.sol", "must survive");
	for (auto const& name: {
		"Target.approval.bin", "Target.clear.bin", "Target.approval.teal",
		"Target.clear.teal", "Target.arc56.json", "Target.000.ssa.ir"})
		writeText(tempDir / name, "stale");
	error.clear();
	ok &= require(puyasol::cli::prepareBackendTargetArtifacts(
		tempDir.string(), roots, error),
		"backend target preparation must succeed: " + error);
	ok &= require(readText(tempDir / "Target.sol") == "must survive",
		"backend preparation must not remove a same-stem source file");
	ok &= require(!fs::exists(tempDir / "Target.approval.bin")
		&& !fs::exists(tempDir / "Target.000.ssa.ir"),
		"backend preparation must remove primary and diagnostic stale outputs");
	writeBytes(tempDir / "Target.approval.bin", {1});
	writeBytes(tempDir / "Target.clear.bin", {2});
	writeText(tempDir / "Target.approval.teal", "#pragma version 12\n");
	writeText(tempDir / "Target.clear.teal", "#pragma version 12\n");
	writeText(tempDir / "Target.arc56.json", "{}\n");
	writeText(tempDir / "Target.000.ssa.ir", "block:\n");
	std::vector<puyasol::artifact::Record> backendRecords;
	error.clear();
	ok &= require(puyasol::cli::collectBackendTargetArtifacts(
		tempDir.string(), roots, backendRecords, error),
		"fresh backend outputs must validate: " + error);
	ok &= require(backendRecords.size() == 6,
		"every fresh backend output for the current target must be hashed");
	writeText(tempDir / "Target.arc56.json", "not-json");
	backendRecords.clear();
	error.clear();
	ok &= require(!puyasol::cli::collectBackendTargetArtifacts(
		tempDir.string(), roots, backendRecords, error),
		"invalid backend JSON must fail artifact validation");

	auto const validDeploy = readText(deployPath);
	writeBytes(clearPath, std::vector<std::uint8_t>(4097, 0x5a));
	std::vector<puyasol::artifact::Record> failedRecords;
	error.clear();
	ok &= require(!puyasol::cli::writeChildDeployTemplates(
		tempDir.string(), {"Child"}, failedRecords, error),
		"a clear program larger than one page must fail");
	ok &= require(readText(deployPath) == validDeploy && failedRecords.empty(),
		"failed validation must preserve the prior template and emit no records");

	auto const manifestPath = tempDir / "artifact-manifest.json";
	error.clear();
	ok &= require(puyasol::artifact::writeManifest(
		manifestPath, "backend-complete", records, error),
		"artifact manifest must write successfully: " + error);
	auto manifest = njson::parse(readText(manifestPath));
	ok &= require(manifest["schema"] == "puya-sol/artifact-manifest/v1"
		&& manifest["phase"] == "backend-complete"
		&& manifest["files"].size() == 3,
		"artifact manifest must identify its schema, phase, and every record");

	auto duplicate = records;
	duplicate.push_back(records.front());
	error.clear();
	ok &= require(!puyasol::artifact::writeManifest(
		manifestPath, "backend-complete", duplicate, error),
		"duplicate manifest paths must be rejected");
	ok &= require(njson::parse(readText(manifestPath))["files"].size() == 3,
		"a rejected manifest must preserve the prior valid manifest");
	ok &= require(noTemporaryArtifacts(tempDir),
		"atomic writers must clean every temporary artifact");

	fs::remove_all(tempDir, ec);
	std::cout << (ok ? "Artifact writers: all cases pass\n"
		: "Artifact writers: FAILURES\n");
	return ok ? 0 : 1;
}
