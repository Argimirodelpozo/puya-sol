/// @file SourceCompat.h
/// 0.5.x/0.6.x → 0.8.x Solidity source compatibility shims, applied as text
/// transforms before the vendored 0.8.x compiler parses anything: pragma
/// relaxation, constructor-visibility removal, `uint(-1)` → `type(...).max`,
/// bare Yul `chainid` → `chainid()`, and duplicate-interface-event removal.
#pragma once

#include <boost/filesystem.hpp>

#include <set>
#include <string>

namespace puyasol::cli
{

/// Apply 0.5.x/0.6.x → 0.8.x syntax transforms before the compiler parses.
std::string transformSource(std::string const& _source);

/// Collect event names from a source string.
std::set<std::string> collectEventSignatures(std::string const& _source);

/// Remove event re-declarations from a contract body (0.5.x allowed them;
/// 0.8.x raises DeclarationError).
std::string removeInheritedEvents(
	std::string const& _source, std::set<std::string> const& _interfaceEvents);

/// Scan relative imports for interface files and collect their event names
/// for use by removeInheritedEvents.
std::set<std::string> collectInterfaceEventsFromImports(
	std::string const& _mainSource, boost::filesystem::path const& _sourceDir);

} // namespace puyasol::cli
