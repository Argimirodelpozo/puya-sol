#include "cli/SourceCompat.h"
#include "Logger.h"

#include <liblangutil/CharStream.h>
#include <liblangutil/Scanner.h>
#include <liblangutil/Token.h>

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

std::set<std::string> collectEventSignatures(std::string const& _source)
{
	std::set<std::string> result;
	auto tokens = lex(_source);
	for (size_t i = 0; i + 1 < tokens.size(); ++i)
		if (tokens[i].token == Token::Event && isIdentifierToken(tokens[i + 1]))
			result.insert(tokens[i + 1].literal);
	return result;
}

std::string removeInheritedEvents(
	std::string const& _source, std::set<std::string> const& _interfaceEvents)
{
	if (_interfaceEvents.empty())
		return _source;

	auto tokens = lex(_source);
	bool hasInheritance = false;
	for (auto const& token: tokens)
		if (token.token == Token::Is) { hasInheritance = true; break; }
	if (!hasInheritance)
		return _source; // no inheritance

	std::vector<Replacement> replacements;
	for (size_t i = 0; i + 1 < tokens.size(); ++i)
	{
		if (tokens[i].token != Token::Event || !isIdentifierToken(tokens[i + 1])
			|| !_interfaceEvents.count(tokens[i + 1].literal))
			continue;
		size_t end = i + 2;
		while (end < tokens.size() && tokens[end].token != Token::Semicolon)
			++end;
		if (end < tokens.size())
			replacements.push_back({tokens[i].start, tokens[end].end, ""});
	}

	return applyReplacements(_source, std::move(replacements));
}

std::set<std::string> collectInterfaceEventsFromImports(
	std::string const& _mainSource, fs::path const& _sourceDir)
{
	std::set<std::string> interfaceEvents;

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
				// Only collect from actual interface declarations.
				auto importedTokens = lex(impContent);
				bool hasInterface = std::any_of(importedTokens.begin(), importedTokens.end(),
					[](LexToken const& token) { return token.token == Token::Interface; });
				if (hasInterface)
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
