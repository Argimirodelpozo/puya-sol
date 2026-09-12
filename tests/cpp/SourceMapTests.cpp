#include "builder/SourceLocConvert.h"
#include <liblangutil/CharStream.h>

#include <iostream>
#include <memory>

int main()
{
	using namespace solidity::langutil;
	puyasol::builder::SourceMap map;
	CharStream main("first\nsecond\n", "main.sol");
	CharStream imported("a\xc3\xa9z\n\nlast\n", "library.sol");
	map.registerCharStream("main.sol", &main, "/project/main.sol");
	map.registerCharStream("library.sol", &imported, "/project/./library.sol");
	map.registerCharStream("/project/library.sol", &imported);
	int failures = 0;
	auto check = [&](bool ok, char const* message) {
		if (!ok) { std::cerr << message << '\n'; ++failures; }
	};
	auto location = [&](int start, int end, std::string name = "library.sol") {
		return map.toAwstLoc("main.sol", SourceLocation{start, end,
			std::make_shared<std::string const>(std::move(name))});
	};
	auto span = location(0, 4);
	check(span.file == "/project/library.sol" && span.line == 1 && span.endLine == 1,
		"import must use its owning source");
	check(span.column == 0 && span.endColumn == 3, "UTF-8 byte offsets to character columns");
	span = location(3, 4);
	check(span.column == 2 && span.endColumn == 3, "column following multi-byte character");
	span = location(0, 5);
	check(span.endLine == 1 && span.endColumn == 4, "exclusive end at newline");
	span = location(5, 5);
	check(span.line == 2 && span.column == 0 && !span.endColumn, "empty line span");
	span = location(6, 10);
	check(span.line == 3 && span.endLine == 3 && span.endColumn == 4, "last line");
	span = map.toAwstLoc("/project/library.sol", 6, 10);
	check(span.file == "/project/library.sol" && span.line == 3, "absolute alias");
	for (auto unknown: {location(-1, -1), location(0, 100), location(0, 4, "missing.sol")})
		check(unknown.file.empty() && unknown.line == 1 && !unknown.column,
			"unknown spans must not borrow another source or fabricate lines");
	map.registerCharStream("virtual.sol", &imported);
	span = location(6, 10, "virtual.sol");
	check(span.file.empty() && span.line == 3, "virtual source retains line facts without a guessed path");
	map.clear();
	check(location(0, 4).file.empty(), "reset forgets prior source identities");
	return failures ? 1 : 0;
}
