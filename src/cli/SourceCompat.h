/// @file SourceCompat.h
/// 0.5.x/0.6.x → 0.8.x Solidity source compatibility shims, applied over
/// tokens from solc's lexer before the vendored parser runs: pragma
/// relaxation, constructor-visibility removal, `uint(-1)` → `type(...).max`,
/// bare Yul `chainid` → `chainid()`, and duplicate-interface-event removal.
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
