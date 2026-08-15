#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>

namespace puyasol::builder
{

/// Owns opaque AWST identities for Solidity function declarations.
/// Resolution is exclusively by solc's globally unique declaration ID;
/// readable source names never participate in identity or collision handling.
class FunctionSymbolTable
{
public:
	using Entries = std::unordered_map<int64_t, std::string>;

	void clear()
	{
		m_entries.clear();
		m_rootSubroutines.clear();
	}

	std::string const& registerDeclaration(
		int64_t _declarationId,
		bool _rootSubroutine)
	{
		auto [it, _] = m_entries.emplace(
			_declarationId,
			"__solfn_" + std::to_string(_declarationId));
		if (_rootSubroutine)
			m_rootSubroutines.insert(_declarationId);
		return it->second;
	}

	std::string const* resolve(int64_t _declarationId) const
	{
		auto const found = m_entries.find(_declarationId);
		return found == m_entries.end() ? nullptr : &found->second;
	}

	bool isRootSubroutine(int64_t _declarationId) const
	{
		return m_rootSubroutines.count(_declarationId) != 0;
	}

private:
	Entries m_entries;
	std::set<int64_t> m_rootSubroutines;
};

} // namespace puyasol::builder
