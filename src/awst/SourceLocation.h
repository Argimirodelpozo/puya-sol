#pragma once

#include <optional>
#include <string>

namespace puyasol::awst
{

struct SourceLocation
{
	std::string file;
	int line = 1;
	int endLine = 1;
	int commentLines = 0;
	std::optional<int> column;
	std::optional<int> endColumn;
};

} // namespace puyasol::awst
