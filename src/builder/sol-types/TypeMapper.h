#pragma once

#include "awst/WType.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace puyasol::builder
{

/// Maps Solidity types to AWST WTypes.
class TypeMapper
{
public:
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

	// ── solc adapter helpers ──
	// Thin wrappers around solc's already-resolved metadata so the rest
	// of the codebase doesn't reach into solidity::frontend directly.
	// See solc_integration_plan.md for the rationale.

	/// Returns solc's canonical ABI signature ("name(types)") for a
	/// FunctionDefinition. Internally calls
	/// `_fd.functionType(false)->externalSignature()`. Returns empty
	/// string if solc refuses to externalize this function (e.g.
	/// internal functions whose signature can't be ABI-encoded).
	static std::string abiSignatureForFunction(
		solidity::frontend::FunctionDefinition const& _fd);

	/// Same for a FunctionType. Returns empty string on failure.
	static std::string abiSignatureForFunction(
		solidity::frontend::FunctionType const& _ft);

	/// Returns the Solidity ABI canonical type name for any Type
	/// (e.g. "uint256[2]", "(int8,bytes)"). Used wherever we'd want to
	/// build a selector from scratch — solc gives us the canonical form.
	static std::string abiTypeName(solidity::frontend::Type const& _t);

	/// Result of an implicit-conversion check.
	struct ImplicitConvert
	{
		bool ok;
		std::string reason;  // empty when ok==true
	};

	/// Wrapper around `Type::isImplicitlyConvertibleTo` that surfaces
	/// the BoolResult's error message as a plain string.
	static ImplicitConvert canImplicitlyConvert(
		solidity::frontend::Type const& _from,
		solidity::frontend::Type const& _to);

	/// solc-resolved virtual target. Given a base FunctionDefinition
	/// and the most-derived contract, returns the actual function
	/// definition that runs (handles `super` and override chains).
	/// Returns nullptr if the base function isn't found in the chain.
	static solidity::frontend::FunctionDefinition const* resolveVirtual(
		solidity::frontend::ContractDefinition const& _mostDerived,
		solidity::frontend::FunctionDefinition const& _baseFn,
		bool _superLookup = false);

	/// Create and register a new owned type.
	template <typename T, typename... Args>
	awst::WType const* createType(Args&&... _args)
	{
		auto ptr = std::make_unique<T>(std::forward<Args>(_args)...);
		auto* raw = ptr.get();
		m_ownedTypes.push_back(std::move(ptr));
		return raw;
	}

private:
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
