#pragma once

#include "awst/WType.h"
#include "builder/target/TargetProfile.h"
#include "builder/types/ReturnWirePlan.h"
#include "builder/types/CallBoundaryPlan.h"

#include "builder/solc/SolcFwd.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
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
		m_solTypeCache.clear();
		m_structTypes.clear();
		m_structProjections.clear();
		m_arc4Cache.clear();
		m_solArc4Cache.clear();
		m_aggregateSources.clear();
		m_returnPlans.clear();
		m_callPlans.clear();
		m_arc4ByteType = nullptr;
		m_ownedTypes = std::make_shared<awst::WTypeArena>();
	}

	/// Map a Solidity type to an AWST WType.
	/// Returns nullptr for unsupported types.
	awst::WType const* map(solidity::frontend::Type const* _solType);

	/// A logical storage declaration/address need not have a materializable
	/// whole-value representation. Return nullptr on target capacity overflow;
	/// value consumers must still use map() and retain its explicit diagnostic.
	awst::WType const* tryMapStorageRepresentation(solidity::frontend::Type const* _solType);

	/// Named-storage references whose type requires a box-key handle. Smaller
	/// fixed values travel by value + write-back unless source facts require boxes.
	bool isBoxKeyedStorageRef(solidity::frontend::Type const* _solType);

	/// Scratch model: pointer representation for a memory aggregate declaration
	/// (shared per MemorySharingFacts, or too large for one AVM value); mixed
	/// model: the size rule only.
	bool memoryDeclarationUsesBlob(solidity::frontend::VariableDeclaration const& _declaration);

	/// Native, internal-call, and ABI return forms from one resolved solc declaration.
	FunctionReturnPlan const& functionReturnPlan(
		solidity::frontend::FunctionDefinition const& _function);
	CallBoundaryPlan const& callBoundaryPlan(
		solidity::frontend::FunctionDefinition const& _function,
		solidity::frontend::ContractDefinition const* _mostDerived = nullptr);

	/// Full struct value, or an explicitly finite recursive-array projection.
	/// Projections never enter the full Solidity/ARC4 type caches.
	awst::WType const* mapStruct(
		solidity::frontend::StructType const* _structType, bool _projection = false);

	/// Solc aggregate facts behind a mapped value/projection. Invocation-local;
	/// used to recover logical member offsets and array bounds through aliases.
	solidity::frontend::Type const* solcAggregateFor(awst::WType const* _type) const
	{
		auto it = m_aggregateSources.find(_type);
		return it == m_aggregateSources.end() ? nullptr : it->second;
	}

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
		m_ownedTypes->push_back(std::move(ptr));
		return raw;
	}

	std::shared_ptr<awst::WTypeArena const> typeArena() const { return m_ownedTypes; }

private:
	awst::WType const* mapArray(solidity::frontend::Type const* _type, bool _projection = false);

	ProgramAnalysis const& m_analysis;
	TargetProfile const& m_profile;
	SourceMap const& m_sourceMap;
	BuildArtifacts& m_artifacts;
	std::map<int64_t, FunctionReturnPlan> m_returnPlans;
	std::map<std::pair<int64_t, int64_t>, CallBoundaryPlan> m_callPlans;

	/// Owns all dynamically-created WTypes.
	std::shared_ptr<awst::WTypeArena> m_ownedTypes = std::make_shared<awst::WTypeArena>();

	/// Solc's canonical identifier after value-representation normalization:
	/// arrays/structs (including tuple components) share across locations and
	/// pointer/ref forms; callable signatures retain their parameter locations.
	/// Nominal identity comes from solc, not source spelling or Type pointers.
	/// These invocation-local keys must never be used as persisted storage keys.
	std::unordered_map<std::string, awst::WType const*>
		m_solTypeCache;
	/// Separate nominal solc IDs for full values and finite projections.
	std::map<int64_t, awst::WType const*> m_structTypes, m_structProjections;
	std::unordered_map<awst::WType const*, solidity::frontend::Type const*> m_aggregateSources;

	/// Session-local interning for the two ARC4 conversion entry points.
	/// Input WType and solc Type objects are stable for the compiler session.
	std::unordered_map<awst::WType const*, awst::WType const*> m_arc4Cache;
	std::unordered_map<solidity::frontend::Type const*, awst::WType const*>
		m_solArc4Cache;
	awst::WType const* m_arc4ByteType = nullptr;

};

} // namespace puyasol::builder
