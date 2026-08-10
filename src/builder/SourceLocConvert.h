#pragma once

/// @file SourceLocConvert.h
/// Central solc→AWST source-location conversion. Solc locations are BYTE
/// OFFSETS into a CharStream; puya's SourceLocation is one-based line data
/// (it slices source lines for diagnostics). Every makeLoc() used to copy
/// offsets into the line fields, so errors and source maps pointed at
/// byte-offset-as-line positions. main registers each compiled unit's
/// CharStream after analysis; conversion resolves the node's own source
/// unit (imports included) and translates offsets to one-based lines.

#include "awst/Node.h"

#include <liblangutil/SourceLocation.h>

#include <string>

namespace solidity::langutil
{
class CharStream;
}

namespace puyasol::builder
{

/// Register a compiled source unit's CharStream (keyed by solc source name).
/// The stream must outlive the AWST build (CompilerStack owns it in main).
void registerCharStream(
	std::string const& _sourceName, solidity::langutil::CharStream const* _cs);

/// Drop all registered streams (start of a new compile).
void clearCharStreams();

/// Convert a solc byte-offset location to an AWST location with one-based
/// lines. Uses the location's own source unit when registered (falling back
/// to `_fallbackFile` for the file name, and to raw offsets when no stream
/// is known — better than fabricating line 0).
awst::SourceLocation toAwstLoc(
	std::string const& _fallbackFile,
	solidity::langutil::SourceLocation const& _sl);

/// Offset-pair variant for call sites without a full solc location. Resolved
/// against `_fallbackFile`'s stream when registered.
awst::SourceLocation toAwstLoc(
	std::string const& _fallbackFile, int _start, int _end);

} // namespace puyasol::builder
