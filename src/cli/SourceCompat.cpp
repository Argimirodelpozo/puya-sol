#include "cli/SourceCompat.h"
#include "Logger.h"

#include <liblangutil/CharStream.h>
#include <liblangutil/Scanner.h>
#include <liblangutil/Token.h>
#include <libsolutil/Keccak256.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = boost::filesystem;

namespace puyasol::cli
{

namespace
{

using solidity::langutil::CharStream;
using solidity::langutil::Scanner;
using solidity::langutil::Token;

struct LexToken
{
	Token token;
	std::string literal;
	int start;
	int end;
};

std::vector<LexToken> lex(std::string const& _source)
{
	CharStream stream(_source, "<source-compat>");
	Scanner scanner(stream);
	std::vector<LexToken> result;
	for (;;)
	{
		auto loc = scanner.currentLocation();
		result.push_back({scanner.currentToken(), scanner.currentLiteral(),
			loc.start, loc.end});
		if (scanner.currentToken() == Token::EOS)
			break;
		scanner.next();
	}
	return result;
}

std::string tokenText(std::string const& _source, LexToken const& _token)
{
	if (_token.start < 0 || _token.end < _token.start
		|| static_cast<size_t>(_token.end) > _source.size())
		return {};
	return _source.substr(
		static_cast<size_t>(_token.start),
		static_cast<size_t>(_token.end - _token.start));
}

struct Replacement
{
	int start;
	int end;
	std::string text;
};

std::string applyReplacements(
	std::string _source, std::vector<Replacement> _replacements)
{
	std::sort(_replacements.begin(), _replacements.end(),
		[](Replacement const& a, Replacement const& b) {
			return a.start > b.start;
		});
	int lastStart = static_cast<int>(_source.size()) + 1;
	for (auto const& replacement: _replacements)
	{
		if (replacement.start < 0 || replacement.end < replacement.start
			|| replacement.end > static_cast<int>(_source.size())
			|| replacement.end > lastStart)
			continue;
		_source.replace(
			static_cast<size_t>(replacement.start),
			static_cast<size_t>(replacement.end - replacement.start),
			replacement.text);
		lastStart = replacement.start;
	}
	return _source;
}

bool isIdentifierToken(LexToken const& _token)
{
	return _token.token == Token::Identifier;
}

std::string eventFingerprint(
	std::string const& _source,
	std::vector<LexToken> const& _tokens,
	size_t _event,
	size_t* _end = nullptr)
{
	if (_event >= _tokens.size() || _tokens[_event].token != Token::Event)
		return {};
	std::string result;
	for (size_t i = _event; i < _tokens.size(); ++i)
	{
		if (_tokens[i].token == Token::EOS)
			return {};
		auto text = tokenText(_source, _tokens[i]);
		result += std::to_string(static_cast<int>(_tokens[i].token));
		result += ':';
		result += text;
		result += ';';
		if (_tokens[i].token == Token::Semicolon)
		{
			if (_end) *_end = i;
			return result;
		}
	}
	return {};
}

InterfaceEventMap collectInterfaces(std::string const& _source)
{
	InterfaceEventMap result;
	auto tokens = lex(_source);
	for (size_t i = 0; i + 1 < tokens.size(); ++i)
	{
		if (tokens[i].token != Token::Interface || !isIdentifierToken(tokens[i + 1]))
			continue;
		auto const interfaceName = tokens[i + 1].literal;
		size_t open = i + 2;
		while (open < tokens.size() && tokens[open].token != Token::LBrace
			&& tokens[open].token != Token::Semicolon)
			++open;
		if (open >= tokens.size() || tokens[open].token != Token::LBrace)
			continue;
		int depth = 1;
		for (size_t j = open + 1; j < tokens.size() && depth > 0; ++j)
		{
			if (tokens[j].token == Token::LBrace) ++depth;
			else if (tokens[j].token == Token::RBrace) --depth;
			else if (depth == 1 && tokens[j].token == Token::Event)
			{
				size_t end = j;
				auto fingerprint = eventFingerprint(_source, tokens, j, &end);
				if (!fingerprint.empty())
					result[interfaceName].insert(std::move(fingerprint));
				j = end;
			}
		}
	}
	return result;
}

} // namespace

std::string transformSource(std::string const& _source)
{
	auto tokens = lex(_source);
	std::vector<Replacement> replacements;
	int braceDepth = 0;
	int assemblyDepth = -1;
	bool awaitingAssemblyBrace = false;

	for (size_t i = 0; i < tokens.size(); ++i)
	{
		auto const& token = tokens[i];

		// Relax a version pragma by preserving its major/minor components and
		// setting patch to zero. Token boundaries come from solc, so text in a
		// comment/string can never be rewritten.
		if (token.token == Token::Pragma && i + 1 < tokens.size()
			&& tokenText(_source, tokens[i + 1]) == "solidity")
		{
			size_t end = i + 2;
			while (end < tokens.size() && tokens[end].token != Token::Semicolon)
				++end;
			if (end < tokens.size())
			{
				auto pragmaText = _source.substr(
					static_cast<size_t>(tokens[i + 1].end),
					static_cast<size_t>(tokens[end].start - tokens[i + 1].end));
				auto majorStart = pragmaText.find_first_of("0123456789");
				auto firstDot = majorStart == std::string::npos
					? std::string::npos : pragmaText.find('.', majorStart);
				auto minorEnd = firstDot == std::string::npos
					? std::string::npos
					: pragmaText.find_first_not_of("0123456789", firstDot + 1);
				if (firstDot != std::string::npos)
				{
					if (minorEnd == std::string::npos)
						minorEnd = pragmaText.size();
					auto majorMinor = pragmaText.substr(
						majorStart, minorEnd - majorStart);
					replacements.push_back({token.start, tokens[end].end,
						"pragma solidity >=" + majorMinor + ".0;"});
				}
			}
		}

		// Constructor visibility was legal before 0.7. The lexer lets us
		// remove exactly the keyword rather than matching across comments or a
		// nested parameter type with regular expressions.
		if (token.token == Token::Constructor)
		{
			int parens = 0;
			for (size_t j = i + 1; j < tokens.size(); ++j)
			{
				if (tokens[j].token == Token::LParen) ++parens;
				else if (tokens[j].token == Token::RParen) --parens;
				else if (parens == 0)
				{
					if (tokens[j].token == Token::Public
						|| tokens[j].token == Token::Internal)
						replacements.push_back({tokens[j].start, tokens[j].end, ""});
					break;
				}
			}
		}

		// uintN(-1) was the pre-0.6 max-value idiom.
		if (i + 4 < tokens.size())
		{
			auto typeName = tokenText(_source, token);
			bool uintType = typeName == "uint"
				|| (typeName.rfind("uint", 0) == 0 && typeName.size() > 4
					&& std::all_of(typeName.begin() + 4, typeName.end(),
						[](char c) { return c >= '0' && c <= '9'; }));
			if (uintType && tokens[i + 1].token == Token::LParen
				&& tokens[i + 2].token == Token::Sub
				&& tokens[i + 3].token == Token::Number
				&& tokenText(_source, tokens[i + 3]) == "1"
				&& tokens[i + 4].token == Token::RParen)
				replacements.push_back({token.start, tokens[i + 4].end,
					"type(" + typeName + ").max"});
		}

		if (token.token == Token::Assembly)
			awaitingAssemblyBrace = true;
		if (token.token == Token::LBrace)
		{
			++braceDepth;
			if (awaitingAssemblyBrace)
			{
				assemblyDepth = braceDepth;
				awaitingAssemblyBrace = false;
			}
		}

		// Old Yul exposed chainid as a bare variable. Restrict the shim to an
		// actual assembly block and exact lexer token; Solidity identifiers,
		// comments, strings, and block.chainid remain untouched.
		if (assemblyDepth >= 0 && braceDepth >= assemblyDepth
			&& isIdentifierToken(token) && token.literal == "chainid"
			&& (i == 0 || tokens[i - 1].token != Token::Period)
			&& (i + 1 == tokens.size() || tokens[i + 1].token != Token::LParen))
			replacements.push_back({token.end, token.end, "()"});

		if (token.token == Token::RBrace)
		{
			if (assemblyDepth == braceDepth)
				assemblyDepth = -1;
			--braceDepth;
		}
	}

	return applyReplacements(_source, std::move(replacements));
}

bool writeSourceRewriteManifest(
	fs::path const& _path,
	SourceRewriteMap const& _sources,
	std::string& _error)
{
	using njson = nlohmann::ordered_json;
	njson manifest{
		{"schema", "puya-sol/source-rewrite-manifest/v1"},
		{"mode", "legacy-source-rewrite"},
		{"warning", "These sources were modified before Solidity parsing; "
			"the result is not a compilation of the original source text."},
		{"sources", njson::array()},
	};
	for (auto const& [sourceUnit, record]: _sources)
	{
		manifest["sources"].push_back({
			{"source_unit", sourceUnit},
			{"changed", record.originalSource != record.transformedSource},
			{"original_keccak256",
				solidity::util::keccak256(record.originalSource).hex()},
			{"transformed_keccak256",
				solidity::util::keccak256(record.transformedSource).hex()},
			{"original_source", record.originalSource},
			{"transformed_source", record.transformedSource},
		});
	}

	std::ofstream out(_path.string(), std::ios::binary | std::ios::trunc);
	if (!out)
	{
		_error = "cannot open " + _path.string();
		return false;
	}
	out << manifest.dump(2) << '\n';
	out.close();
	if (!out)
	{
		_error = "failed writing " + _path.string();
		return false;
	}
	return true;
}

std::set<std::string> collectEventSignatures(std::string const& _source)
{
	std::set<std::string> result;
	auto tokens = lex(_source);
	for (size_t i = 0; i < tokens.size(); ++i)
		if (tokens[i].token == Token::Event)
		{
			size_t end = i;
			auto fingerprint = eventFingerprint(_source, tokens, i, &end);
			if (!fingerprint.empty())
				result.insert(std::move(fingerprint));
			i = end;
		}
	return result;
}

std::string removeInheritedEvents(
	std::string const& _source, InterfaceEventMap const& _interfaceEvents)
{
	if (_interfaceEvents.empty())
		return _source;

	auto tokens = lex(_source);
	std::vector<Replacement> replacements;
	for (size_t i = 0; i + 1 < tokens.size(); ++i)
	{
		if (tokens[i].token != Token::Contract && tokens[i].token != Token::Interface)
			continue;
		size_t open = i + 1;
		while (open < tokens.size() && tokens[open].token != Token::LBrace
			&& tokens[open].token != Token::Semicolon)
			++open;
		if (open >= tokens.size() || tokens[open].token != Token::LBrace)
			continue;

		std::set<std::string> inheritedEvents;
		bool inBases = false;
		bool expectBase = false;
		int parenDepth = 0;
		for (size_t j = i + 1; j < open; ++j)
		{
			if (tokens[j].token == Token::Is)
			{
				inBases = true;
				expectBase = true;
				continue;
			}
			if (!inBases)
				continue;
			if (tokens[j].token == Token::LParen)
			{
				++parenDepth;
				continue;
			}
			if (tokens[j].token == Token::RParen)
			{
				--parenDepth;
				continue;
			}
			if (parenDepth == 0 && tokens[j].token == Token::Comma)
			{
				expectBase = true;
				continue;
			}
			if (!expectBase || !isIdentifierToken(tokens[j]))
				continue;
			expectBase = false;
			// Qualified/aliased bases are intentionally unsupported here: leave
			// them for strict solc analysis rather than guessing their identity.
			if (j + 1 < open && tokens[j + 1].token == Token::Period)
				continue;
			auto found = _interfaceEvents.find(tokens[j].literal);
			if (found != _interfaceEvents.end())
				inheritedEvents.insert(found->second.begin(), found->second.end());
		}
		if (inheritedEvents.empty())
			continue;

		int depth = 1;
		for (size_t j = open + 1; j < tokens.size() && depth > 0; ++j)
		{
			if (tokens[j].token == Token::LBrace) ++depth;
			else if (tokens[j].token == Token::RBrace) --depth;
			else if (depth == 1 && tokens[j].token == Token::Event)
			{
				size_t end = j;
				auto fingerprint = eventFingerprint(_source, tokens, j, &end);
				if (inheritedEvents.count(fingerprint))
					replacements.push_back({tokens[j].start, tokens[end].end, ""});
				j = end;
			}
		}
		i = open;
	}

	return applyReplacements(_source, std::move(replacements));
}

InterfaceEventMap collectInterfaceEventsFromImports(
	std::string const& _mainSource, fs::path const& _sourceDir)
{
	InterfaceEventMap interfaceEvents;

	// Scan imports with solc's lexer. This covers alias/from forms and never
	// mistakes commented-out imports for dependencies.
	auto tokens = lex(_mainSource);
	for (size_t i = 0; i < tokens.size(); ++i)
	{
		if (tokens[i].token != Token::Import)
			continue;
		std::string importPath;
		for (size_t j = i + 1; j < tokens.size() && tokens[j].token != Token::Semicolon; ++j)
			if (tokens[j].token == Token::StringLiteral)
				importPath = tokens[j].literal;
		if (importPath.empty() || importPath[0] != '.')
			continue;
		fs::path importAbsPath = _sourceDir / importPath;
		if (fs::exists(importAbsPath))
		{
			std::ifstream impFile(importAbsPath.string());
			if (impFile.is_open())
			{
				std::ostringstream ss;
				ss << impFile.rdbuf();
				std::string impContent = ss.str();
				auto interfaces = collectInterfaces(impContent);
				for (auto& [name, events]: interfaces)
					interfaceEvents[name].insert(events.begin(), events.end());
			}
		}
	}
	if (!interfaceEvents.empty())
		puyasol::Logger::instance().debug(
			"Found events in " + std::to_string(interfaceEvents.size()) +
			" imported interface(s) eligible for exact deduplication");

	return interfaceEvents;
}

} // namespace puyasol::cli
