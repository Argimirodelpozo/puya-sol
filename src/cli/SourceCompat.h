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

/// Transform Solidity source for compatibility with the 0.8.x compiler.
/// Handles pragma relaxation and 0.5.x/0.6.x → 0.8.x syntax differences so
/// that original contracts can be compiled without modification.
std::string transformSource(std::string const& _source);

/// Collect event signatures from a source string.
std::set<std::string> collectEventSignatures(std::string const& _source);

/// Remove event declarations from a contract source that are already defined
/// in its interfaces. In 0.5.x, re-declaring interface events in a contract
/// was allowed; in 0.8.x it's a DeclarationError. This resolves it by removing
/// the duplicate from the contract body.
std::string removeInheritedEvents(
	std::string const& _source, std::set<std::string> const& _interfaceEvents);

/// Pre-scan the main source's relative imports for interface files and collect
/// their event signatures, so removeInheritedEvents can dedup re-declarations.
std::set<std::string> collectInterfaceEventsFromImports(
	std::string const& _mainSource, boost::filesystem::path const& _sourceDir);

} // namespace puyasol::cli
