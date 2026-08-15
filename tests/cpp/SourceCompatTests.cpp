#include "cli/SourceCompat.h"

#include <iostream>
#include <string>

namespace
{

bool require(bool _condition, std::string const& _message)
{
	if (_condition)
		return true;
	std::cerr << _message << '\n';
	return false;
}

} // namespace

int main()
{
	std::string const source = R"(pragma solidity =0.5.16;
// pragma solidity ^0.4.20; uint(-1) chainid
contract Legacy {
    string constant text = "uint(-1) chainid";
    constructor(uint value) public { value; }
    function max() public pure returns (uint) { return uint(-1); }
    function id() public view returns (uint result) {
        assembly { result := chainid }
    }
})";
	auto transformed = puyasol::cli::transformSource(source);
	bool ok = true;
	ok &= require(transformed.find("pragma solidity >=0.5.0;") != std::string::npos,
		"version pragma was not relaxed");
	ok &= require(transformed.find("constructor(uint value)  {") != std::string::npos,
		"constructor visibility was not removed");
	ok &= require(transformed.find("return type(uint).max;") != std::string::npos,
		"uint(-1) was not rewritten");
	ok &= require(transformed.find("result := chainid()") != std::string::npos,
		"bare assembly chainid was not rewritten");
	ok &= require(transformed.find("// pragma solidity ^0.4.20; uint(-1) chainid")
		!= std::string::npos, "comment text was rewritten");
	ok &= require(transformed.find("\"uint(-1) chainid\"") != std::string::npos,
		"string contents were rewritten");
	ok &= require(transformed.find("function max() public") != std::string::npos,
		"non-constructor visibility was removed");

	std::string const interfaceSource = R"(
// event Commented(uint value);
interface I { event Kept(address indexed who); }
)";
	auto names = puyasol::cli::collectEventSignatures(interfaceSource);
	ok &= require(names == std::set<std::string>({"Kept"}),
		"event collection did not follow lexer tokens");
	std::string const contractSource = R"(
// event Commented(uint value);
contract C is I {
    event Kept(address indexed who);
    event Local(uint value);
})";
	auto removed = puyasol::cli::removeInheritedEvents(contractSource, names);
	ok &= require(removed.find("event Local") != std::string::npos,
		"unrelated event was removed");
	ok &= require(removed.find("event Kept") == std::string::npos,
		"inherited event redeclaration was retained");
	ok &= require(removed.find("// event Commented") != std::string::npos,
		"commented event was modified");

	return ok ? 0 : 1;
}
