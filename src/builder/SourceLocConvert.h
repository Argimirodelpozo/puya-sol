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
#include <map>

namespace solidity::langutil
{
class CharStream;
}

namespace puyasol::builder
{

class SourceMap
{
public:
	/// Register a compiled source unit's CharStream. CompilerStack owns it and
	/// must outlive this build session.
	void registerCharStream(
		std::string const& _sourceName,
		solidity::langutil::CharStream const* _charStream);

	void clear() { m_streams.clear(); }

	/// Convert a solc byte-offset location to a one-based AWST line range.
	awst::SourceLocation toAwstLoc(
		std::string const& _fallbackFile,
		solidity::langutil::SourceLocation const& _sourceLocation) const;

	/// Offset-pair variant for call sites without a full solc location.
	awst::SourceLocation toAwstLoc(
		std::string const& _fallbackFile, int _start, int _end) const;

private:
	std::map<std::string, solidity::langutil::CharStream const*> m_streams;
};

} // namespace puyasol::builder
