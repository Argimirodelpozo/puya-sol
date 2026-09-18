#pragma once

/// @file SourceLocConvert.h
/// Central solc→AWST source-location conversion. Solc locations are BYTE
/// OFFSETS into a CharStream; puya's SourceLocation is one-based line data
/// (it slices source lines for diagnostics). Every makeLoc() used to copy
/// offsets into the line fields, so errors and source maps pointed at
/// byte-offset-as-line positions. main registers each compiled unit's
/// CharStream after analysis; conversion resolves the node's own source
/// unit (imports included) and translates offsets to one-based lines.

#include "awst/SourceLocation.h"

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
		solidity::langutil::CharStream const* _charStream,
		std::string const& _readableFile = {});

	void clear() { m_streams.clear(); }

	/// Convert a solc byte-offset location to a one-based AWST line range.
	awst::SourceLocation toAwstLoc(
		std::string const& _fallbackFile,
		solidity::langutil::SourceLocation const& _sourceLocation) const;

	/// Offset-pair variant for call sites without a full solc location.
	awst::SourceLocation toAwstLoc(
		std::string const& _fallbackFile, int _start, int _end) const;

private:
	struct Source
	{
		std::string file;
		solidity::langutil::CharStream const* stream;
		mutable std::map<std::pair<int, int>, awst::SourceLocation> locations;
	};
	std::map<std::string, Source> m_streams;
};

} // namespace puyasol::builder
