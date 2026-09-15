#include "builder/solc/SourceLocConvert.h"

#include <liblangutil/CharStream.h>
#include <filesystem>

namespace puyasol::builder
{

namespace
{
awst::SourceLocation convert(
	std::string const& file, solidity::langutil::CharStream const* stream,
	int start, int end)
{
	// Unknown spans must not become byte-offset-as-line diagnostics.
	if (!stream || start < 0 || end < start
		|| static_cast<size_t>(end) > stream->source().size())
		return awst::SourceLocation({}, 1, 1);
	auto first = stream->translatePositionToLineColumn(start);
	auto last = stream->translatePositionToLineColumn(end > start ? end - 1 : end);
	// solc columns count UTF-8 bytes; Puya indexes Python strings. Count code
	// points within the line, retaining solc's line/byte-offset authority.
	auto column = [&](int offset, int byteColumn) {
		int result = 0;
		for (int i = offset - byteColumn; i < offset; ++i)
			if ((static_cast<unsigned char>(stream->source()[i]) & 0xc0) != 0x80) ++result;
		return result;
	};
	int firstColumn = column(start, first.column);
	int lastOffset = end > start ? end - 1 : end;
	int lastColumn = column(lastOffset, last.column);
	if (end > start && (static_cast<unsigned char>(stream->source()[lastOffset]) & 0xc0) != 0x80)
		++lastColumn;
	return awst::SourceLocation(file, first.line + 1, last.line + 1, 0, firstColumn,
		lastColumn > 0 ? std::optional<int>{lastColumn} : std::nullopt);
}
} // namespace

void SourceMap::registerCharStream(
	std::string const& name, solidity::langutil::CharStream const* stream,
	std::string const& readableFile)
{
	auto file = readableFile.empty() ? name : readableFile;
	// A virtual source has valid line facts, but no readable filesystem path.
	if (!std::filesystem::path(file).is_absolute()) file.clear();
	else file = std::filesystem::path(file).lexically_normal().string();
	m_streams[name] = {std::move(file), stream};
}

awst::SourceLocation SourceMap::toAwstLoc(
	std::string const& fallback, solidity::langutil::SourceLocation const& location) const
{
	// A foreign source's offsets never belong to the fallback source.
	auto it = m_streams.find(location.sourceName ? *location.sourceName : fallback);
	return it == m_streams.end() ? awst::SourceLocation({}, 1, 1)
		: convert(it->second.file, it->second.stream, location.start, location.end);
}

awst::SourceLocation SourceMap::toAwstLoc(std::string const& fallback, int start, int end) const
{
	auto it = m_streams.find(fallback);
	return it == m_streams.end() ? awst::SourceLocation({}, 1, 1)
		: convert(it->second.file, it->second.stream, start, end);
}

} // namespace puyasol::builder
