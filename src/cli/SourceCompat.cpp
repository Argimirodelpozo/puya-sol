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

	// 1. Relax pragma: "=0.5.16" → ">=0.5.0"
	{
		static std::regex const re(R"(pragma\s+solidity\s+[=^~><]*\s*(\d+\.\d+)\.\d+\s*;)");
		result = std::regex_replace(result, re, "pragma solidity >=$1.0;");
	}

	// 2. Drop constructor visibility (0.5.x had it; 0.8.x parser error).
	{
		static std::regex const re(R"(constructor\s*\(([^)]*)\)\s+(?:public|internal)\s*\{)");
		result = std::regex_replace(result, re, "constructor($1) {");
	}

	// 3. uint(-1) → type(uintN).max (0.5.x max-value idiom).
	{
		static std::regex const re(R"((uint\d*)\s*\(\s*-\s*1\s*\))");
		result = std::regex_replace(result, re, "type($1).max");
	}

	// 4. Bare Yul `chainid` → `chainid()` (was a variable in 0.5.x; must be called in 0.8.x).
	//    Must NOT match block.chainid. std::regex has no lookbehind — manual loop.
	{
		std::string const needle = "chainid";
		size_t pos = 0;
		while ((pos = result.find(needle, pos)) != std::string::npos)
		{
			size_t endPos = pos + needle.size();
			// Skip if preceded by '.' (block.chainid)
			if (pos > 0 && result[pos - 1] == '.')
			{
				pos = endPos;
				continue;
			}
			// Already has '('
			size_t nextNonSpace = endPos;
			while (nextNonSpace < result.size() && result[nextNonSpace] == ' ')
				++nextNonSpace;
			if (nextNonSpace < result.size() && result[nextNonSpace] == '(')
			{
				pos = endPos;
				continue;
			}
			// Word-boundary check — BOTH sides. A trailing identifier char
			// means this is a longer identifier (`chainidentifier` would have
			// become `chainid()entifier`).
			if (pos > 0 && (std::isalnum(static_cast<unsigned char>(result[pos - 1]))
				|| result[pos - 1] == '_'))
			{
				pos = endPos;
				continue;
			}
			if (endPos < result.size()
				&& (std::isalnum(static_cast<unsigned char>(result[endPos]))
					|| result[endPos] == '_'))
			{
				pos = endPos;
				continue;
			}
			// Append "()"
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

	// Remove event declarations that exist in inherited interfaces.
	static std::regex const contractRe(R"(contract\s+\w+\s+is\s+)");
	if (!std::regex_search(result, contractRe))
		return result; // no inheritance

	for (auto const& eventName: _interfaceEvents)
	{
		// Match event decl with optional whitespace/newlines.
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

	// Scan relative imports in main source.
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
				// Only collect from interface files.
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
