#pragma once

/// @file StoragePathWalker.h
/// One walker for default-layout storage paths. A holder pairs the logical
/// key (StorageKey segments) with the serialized value it addresses; mapping
/// keys, array indices and struct members advance both. All callers share
/// full-width logical bounds, single evaluation, and checked AVM narrowing.

#include "awst/Node.h"
#include "builder/solc/SolcFwd.h"

namespace puyasol::builder
{
class TypeMapper;

/// Logical holder identity and serialized value path are distinct: an inline
/// array/struct has a descendant mapping identity, but its ordinary fields and
/// dynamic length still live inside the enclosing box.
struct StorageHolder
{
	std::shared_ptr<awst::Expression> key;
	std::shared_ptr<awst::Expression> value;
};

/// Advances a holder through the mapping/array levels of a declared type.
/// Consecutive array ranks stay inline in the parent value; a mapping value
/// that is an array starts a new box. A key-less holder derives no key.
class StoragePathWalker
{
public:
	/// Key-only consumers retain inline arrays just for subsequent bounds.
	enum class ValueTracking { All, NestedArrays };
	StoragePathWalker(
		TypeMapper& _typeMapper, solidity::frontend::Type const* _root,
		awst::SourceLocation const& _loc, ValueTracking _tracking = ValueTracking::All);

	/// Container type at the cursor; nullptr past the declared chain.
	solidity::frontend::Type const* current() const { return m_type; }

	/// One level. Evaluate the index, check the solc bound, then narrow for AVM.
	StorageHolder step(
		StorageHolder _holder, std::shared_ptr<awst::Expression> _index,
		std::vector<std::shared_ptr<awst::Statement>>& _pre);

	/// Struct member projection; a transparent single-struct wrapper adds no step.
	static StorageHolder member(
		StorageHolder _base, solidity::frontend::StructType const& _type,
		std::string const& _name, awst::WType const* _valueType,
		awst::SourceLocation const& _loc);

	/// Box-backed value of a non-mapping storage type at `_key`.
	static std::shared_ptr<awst::Expression> boxedValue(
		TypeMapper& _typeMapper, std::shared_ptr<awst::Expression> _key,
		solidity::frontend::Type const* _type, awst::SourceLocation const& _loc);

private:
	TypeMapper& m_typeMapper;
	ValueTracking m_tracking;
	solidity::frontend::Type const* m_type;
	awst::SourceLocation m_loc;
};

} // namespace puyasol::builder
