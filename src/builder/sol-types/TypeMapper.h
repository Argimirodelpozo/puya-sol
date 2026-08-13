#pragma once

#include "awst/WType.h"
#include "builder/TargetProfile.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace puyasol::builder
{

struct ProgramAnalysis;
class SourceMap;
struct BuildArtifacts;

/// Maps Solidity types to AWST WTypes.
class TypeMapper
{
public:
	TypeMapper(
		ProgramAnalysis const& _analysis,
		TargetProfile const& _profile,
		SourceMap const& _sourceMap,
		BuildArtifacts& _artifacts)
		: m_analysis(_analysis), m_profile(_profile), m_sourceMap(_sourceMap),
		  m_artifacts(_artifacts)
	{}

	ProgramAnalysis const& analysis() const { return m_analysis; }
	TargetProfile const& profile() const { return m_profile; }
	SourceMap const& sourceMap() const { return m_sourceMap; }
	BuildArtifacts& artifacts() const { return m_artifacts; }

	/// Drop invocation-local mapped and synthetic types before translating a
	/// new CompilerStack with this AWSTBuilder instance.
	void reset()
	{
		m_inProgressStructs.clear();
		m_cache.clear();
		m_ownedTypes.clear();
	}

	/// Map a Solidity type to an AWST WType.
	/// Returns nullptr for unsupported types.
	awst::WType const* map(solidity::frontend::Type const* _solType);

	/// Get or create an ARC4Struct WType for a Solidity struct.
	awst::WType const* mapStruct(solidity::frontend::StructType const* _structType);

	/// Map a raw WType to its ARC4 equivalent for storage encoding.
	/// Types already in ARC4 form pass through unchanged.
	awst::WType const* mapToARC4Type(awst::WType const* _type);

	/// Map a Solidity type directly to ARC4, preserving signedness.
	/// Use this for method signatures where int256 vs uint256 matters.
	awst::WType const* mapSolTypeToARC4(solidity::frontend::Type const* _solType);

	/// Create and register a new owned type.
	template <typename T, typename... Args>
	T const* createType(Args&&... _args)
	{
		auto ptr = std::make_unique<T>(std::forward<Args>(_args)...);
		auto* raw = ptr.get();
		m_ownedTypes.push_back(std::move(ptr));
		return raw;
	}

private:
	ProgramAnalysis const& m_analysis;
	TargetProfile const& m_profile;
	SourceMap const& m_sourceMap;
	BuildArtifacts& m_artifacts;

	/// Owns all dynamically-created WTypes.
	std::vector<std::unique_ptr<awst::WType>> m_ownedTypes;

	/// Cache: Solidity type string → WType.
	std::map<std::string, awst::WType const*> m_cache;

	/// Recursion guard for mapStruct: holds AST IDs of structs that are
	/// currently being mapped, so a recursive struct field returns a
	/// placeholder instead of stack-overflowing.
	std::set<int64_t> m_inProgressStructs;
};

} // namespace puyasol::builder
