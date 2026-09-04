/// @file SourceCompat.h
/// Explicit, research-only 0.5.x/0.6.x → 0.8.x Solidity compatibility
/// shims. Production compilation passes original source bytes to solc; callers
/// must opt into these transforms with --legacy-source-rewrite.
#pragma once

#include <boost/filesystem.hpp>

#include <map>
#include <set>
#include <string>

namespace puyasol::cli
{

/// Apply 0.5.x/0.6.x → 0.8.x syntax transforms before the compiler parses.
std::string transformSource(std::string const& _source);

/// Collect exact token-normalized event declarations from a source string.
/// Parameter names are deliberately retained: a false negative is a clean
/// solc diagnostic, while a name-only false positive silently deletes source.
std::set<std::string> collectEventSignatures(std::string const& _source);

using InterfaceEventMap = std::map<std::string, std::set<std::string>>;

struct SourceRewriteRecord
{
	std::string originalSource;
	std::string transformedSource;
};

using SourceRewriteMap = std::map<std::string, SourceRewriteRecord>;

/// Write an auditable record for an explicit legacy rewrite. Each entry holds
/// the exact original/transformed source plus Keccak-256 hashes of both.
/// Returns false and populates `_error` when the manifest cannot be written.
bool writeSourceRewriteManifest(
	boost::filesystem::path const& _path,
	SourceRewriteMap const& _sources,
	std::string& _error);

/// Remove exact event re-declarations only from a contract/interface that
/// directly names the imported interface as a base. Unsupported indirect or
/// aliased legacy shapes remain untouched and therefore fail in solc safely.
std::string removeInheritedEvents(
	std::string const& _source, InterfaceEventMap const& _interfaceEvents);

/// Scan relative imports for interface declarations and collect their exact
/// event fingerprints for use by removeInheritedEvents.
InterfaceEventMap collectInterfaceEventsFromImports(
	std::string const& _mainSource, boost::filesystem::path const& _sourceDir);

} // namespace puyasol::cli
