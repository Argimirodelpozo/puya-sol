#pragma once

#include <string>

namespace puyasol::builder
{

/// Extract the value of a NatSpec custom tag from a raw doc string.
/// Recognises `@custom:uros-chunk pricing` in ///, /**, or // forms.
/// Returns the first token after the tag, or "" if absent.
inline std::string natSpecTagValue(std::string const& _doc, std::string const& _tag)
{
	if (_doc.empty())
		return "";
	std::string const needle = "@" + _tag;
	std::size_t pos = _doc.find(needle);
	if (pos == std::string::npos)
		return "";
	pos += needle.size();
	std::size_t eol = _doc.find('\n', pos);
	std::string value = _doc.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
	if (auto term = value.find("*/"); term != std::string::npos)
		value = value.substr(0, term);
	std::size_t b = value.find_first_not_of(" \t\r\f\v");
	if (b == std::string::npos)
		return "";
	std::size_t e = value.find_last_not_of(" \t\r\f\v");
	value = value.substr(b, e - b + 1);
	// first token only (chunk/splitter names are single identifiers)
	if (auto sp = value.find_first_of(" \t"); sp != std::string::npos)
		value = value.substr(0, sp);
	return value;
}

} // namespace puyasol::builder
