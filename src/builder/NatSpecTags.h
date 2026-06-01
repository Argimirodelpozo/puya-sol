#pragma once

#include <string>

namespace puyasol::builder
{

/// Extract the value of a NatSpec custom tag from a raw documentation string.
///
/// puya-sol receives NatSpec as the raw doc text (solc parses doxygen tags
/// "later on"), so we scan it ourselves. Recognises the tag in the forms a
/// user is likely to write it, e.g. for tag == "custom:uros-chunk":
///
///     /// @custom:uros-chunk pricing
///     /** @custom:uros-chunk pricing */
///     // @custom:uros-chunk pricing
///
/// Returns the first whitespace-delimited token after the tag (trimmed), or ""
/// if the tag is absent. Matching is on the literal "@<tag>" substring; the
/// value is everything up to the next newline, trimmed of surrounding
/// whitespace and a single trailing comment terminator ("*/").
inline std::string natSpecTagValue(std::string const& _doc, std::string const& _tag)
{
	if (_doc.empty())
		return "";
	std::string const needle = "@" + _tag;
	std::size_t pos = _doc.find(needle);
	if (pos == std::string::npos)
		return "";
	pos += needle.size();
	// take to end of line
	std::size_t eol = _doc.find('\n', pos);
	std::string value = _doc.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
	// strip a trailing block-comment terminator if present
	if (auto term = value.find("*/"); term != std::string::npos)
		value = value.substr(0, term);
	// trim leading/trailing whitespace
	std::size_t b = value.find_first_not_of(" \t\r\f\v");
	if (b == std::string::npos)
		return "";
	std::size_t e = value.find_last_not_of(" \t\r\f\v");
	value = value.substr(b, e - b + 1);
	// the value is the first whitespace-delimited token (chunk/splitter names
	// are single identifiers); drop anything after the first space.
	if (auto sp = value.find_first_of(" \t"); sp != std::string::npos)
		value = value.substr(0, sp);
	return value;
}

} // namespace puyasol::builder
