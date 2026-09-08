#pragma once

#include <optional>
#include <string>
#include <utility>

namespace puyasol::awst
{

struct SourceLocation
{
	SourceLocation() = default;
	explicit SourceLocation(std::string _file): file(std::move(_file)) {}
	SourceLocation(
		std::string _file,
		int _line,
		int _endLine,
		int _commentLines = 0,
		std::optional<int> _column = std::nullopt,
		std::optional<int> _endColumn = std::nullopt)
		: file(std::move(_file)),
		  line(_line),
		  endLine(_endLine),
		  commentLines(_commentLines),
		  column(_column),
		  endColumn(_endColumn)
	{}

	std::string file;
	int line = 1;
	int endLine = 1;
	int commentLines = 0;
	std::optional<int> column;
	std::optional<int> endColumn;
};

} // namespace puyasol::awst
