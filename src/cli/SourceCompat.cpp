#include "cli/SourceCompat.h"
#include "Logger.h"

#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = boost::filesystem;

namespace puyasol::cli
{

std::string transformSource(std::string const& _source)
{
	std::string result = _source;

	// 1. Relax pragma version: "pragma solidity =0.5.16;" → "pragma solidity >=0.5.0;"
	{
		static std::regex const re(R"(pragma\s+solidity\s+[=^~><]*\s*(\d+\.\d+)\.\d+\s*;)");
		result = std::regex_replace(result, re, "pragma solidity >=$1.0;");
	}

	// 2. Remove visibility from constructors: "constructor(...) public {" → "constructor(...) {"
	//    In 0.5.x constructors had visibility; in 0.8.x this is a parser error.
	{
		static std::regex const re(R"(constructor\s*\(([^)]*)\)\s+(?:public|internal)\s*\{)");
		result = std::regex_replace(result, re, "constructor($1) {");
	}

	// 3. Replace type cast to max: "uint(-1)" → "type(uint256).max", "uint112(-1)" → "type(uint112).max"
	//    In 0.5.x, uint(-1) was the idiom for max value; 0.8.x requires type(...).max
	{
		static std::regex const re(R"((uint\d*)\s*\(\s*-\s*1\s*\))");
		result = std::regex_replace(result, re, "type($1).max");
	}

	// 4. Fix bare Yul builtins in assembly: "chainid" (not followed by "(") → "chainid()"
	//    In 0.5.x Yul, chainid was a variable; in 0.8.x it must be called as a function.
	//    Must NOT match "block.chainid" (0.8.x property access), only bare "chainid" in assembly.
	//    C++ std::regex doesn't support lookbehind, so we use a manual replacement loop.
	{
		std::string const needle = "chainid";
		size_t pos = 0;
		while ((pos = result.find(needle, pos)) != std::string::npos)
		{
			size_t endPos = pos + needle.size();
			// Skip if preceded by '.' (e.g. block.chainid)
			if (pos > 0 && result[pos - 1] == '.')
			{
				pos = endPos;
				continue;
			}
			// Skip if already followed by '('
			size_t nextNonSpace = endPos;
			while (nextNonSpace < result.size() && result[nextNonSpace] == ' ')
				++nextNonSpace;
			if (nextNonSpace < result.size() && result[nextNonSpace] == '(')
			{
				pos = endPos;
				continue;
			}
			// Check word boundary: character before must not be alphanumeric/underscore
			if (pos > 0 && (std::isalnum(result[pos - 1]) || result[pos - 1] == '_'))
			{
				pos = endPos;
				continue;
			}
			// Replace bare "chainid" with "chainid()"
			result.insert(endPos, "()");
			pos = endPos + 2;
		}
	}

	return result;
}

std::set<std::string> collectEventSignatures(std::string const& _source)
{
	std::set<std::string> result;
	static std::regex const eventRe(R"(event\s+(\w+)\s*\([^)]*\)\s*;)");
	auto it = std::sregex_iterator(_source.begin(), _source.end(), eventRe);
	auto end = std::sregex_iterator();
	for (; it != end; ++it)
		result.insert((*it)[1].str());
	return result;
}

std::string removeInheritedEvents(
	std::string const& _source, std::set<std::string> const& _interfaceEvents)
{
	if (_interfaceEvents.empty())
		return _source;

	std::string result = _source;

	// Find "contract X is Y {" sections and remove event declarations for events
	// that exist in the inherited interfaces
	static std::regex const contractRe(R"(contract\s+\w+\s+is\s+)");
	if (!std::regex_search(result, contractRe))
		return result; // No inheritance, nothing to dedup

	// Remove matching event declarations
	for (auto const& eventName: _interfaceEvents)
	{
		// Match "event EventName(...) ;" with possible whitespace/newlines
		std::regex eventDeclRe(
			"\\s*event\\s+" + eventName + "\\s*\\([^)]*\\)\\s*;[\\t ]*\\n?"
		);
		result = std::regex_replace(result, eventDeclRe, "\n");
	}

	return result;
}

std::set<std::string> collectInterfaceEventsFromImports(
	std::string const& _mainSource, fs::path const& _sourceDir)
{
	std::set<std::string> interfaceEvents;

	// Find import paths in the main source
	static std::regex const importRe(R"(import\s+['"](\.\/[^'"]+)['"]\s*;)");
	auto it = std::sregex_iterator(_mainSource.begin(), _mainSource.end(), importRe);
	auto end = std::sregex_iterator();
	for (; it != end; ++it)
	{
		std::string importPath = (*it)[1].str();
		fs::path importAbsPath = _sourceDir / importPath;
		if (fs::exists(importAbsPath))
		{
			std::ifstream impFile(importAbsPath.string());
			if (impFile.is_open())
			{
				std::ostringstream ss;
				ss << impFile.rdbuf();
				std::string impContent = ss.str();
				// Only collect events from interfaces (not concrete contracts)
				if (impContent.find("interface ") != std::string::npos)
				{
					auto events = collectEventSignatures(impContent);
					interfaceEvents.insert(events.begin(), events.end());
				}
			}
		}
	}
	if (!interfaceEvents.empty())
		puyasol::Logger::instance().debug(
			"Found " + std::to_string(interfaceEvents.size()) +
			" event(s) in interfaces to dedup");

	return interfaceEvents;
}

} // namespace puyasol::cli
