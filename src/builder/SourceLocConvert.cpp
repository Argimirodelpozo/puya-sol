#include "builder/SourceLocConvert.h"

#include <liblangutil/CharStream.h>

namespace puyasol::builder
{

namespace
{
/// One-based line for a byte offset; -1 when no stream / bad offset.
int lineAt(solidity::langutil::CharStream const* _cs, int _offset)
{
	if (!_cs || _offset < 0)
		return -1;
	// solc LineColumn is zero-based.
	return _cs->translatePositionToLineColumn(_offset).line + 1;
}

awst::SourceLocation convert(
	std::string const& _file,
	solidity::langutil::CharStream const* _cs,
	int _start,
	int _end)
{
	awst::SourceLocation loc;
	loc.file = _file;
	if (_cs)
	{
		int startLine = lineAt(_cs, _start);
		// `end` is exclusive: a node ending exactly at a newline must not
		// spill onto the next line.
		int endLine = lineAt(_cs, _end > _start ? _end - 1 : _end);
		loc.line = startLine > 0 ? startLine : 0;
		loc.endLine = endLine >= startLine ? endLine : loc.line;
	}
	else
	{
		// No stream registered (synthetic node or pre-registration path):
		// keep the raw offsets — monotonic and non-zero beats fabricated 0s.
		loc.line = _start >= 0 ? _start : 0;
		loc.endLine = _end >= 0 ? _end : 0;
	}
	return loc;
}
} // namespace

void SourceMap::registerCharStream(
	std::string const& _sourceName, solidity::langutil::CharStream const* _cs)
{
	m_streams[_sourceName] = _cs;
}

awst::SourceLocation SourceMap::toAwstLoc(
	std::string const& _fallbackFile,
	solidity::langutil::SourceLocation const& _sl) const
{
	// The node's own source unit picks the STREAM (imports have their own
	// offsets); the FILE stays the caller's path — puya opens it to excerpt
	// source lines, and unit names are not readable paths.
	auto it = _sl.sourceName
		? m_streams.find(*_sl.sourceName) : m_streams.end();
	if (it == m_streams.end())
		it = m_streams.find(_fallbackFile);
	return convert(
		_fallbackFile,
		it != m_streams.end() ? it->second : nullptr,
		_sl.start, _sl.end);
}

awst::SourceLocation SourceMap::toAwstLoc(
	std::string const& _fallbackFile, int _start, int _end) const
{
	auto it = m_streams.find(_fallbackFile);
	return convert(
		_fallbackFile,
		it != m_streams.end() ? it->second : nullptr,
		_start, _end);
}

} // namespace puyasol::builder
