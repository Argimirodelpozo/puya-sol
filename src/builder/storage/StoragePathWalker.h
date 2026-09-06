#pragma once

/// @file StoragePathWalker.h
/// One walker for default-layout storage paths. A holder pairs the logical
/// key (StorageKey segments) with the serialized value it addresses; mapping
/// keys, array indices and struct members advance both. The index recipe
/// (pins, uint64 narrowing, bounds) is where the callers historically drifted,
/// so it is an explicit policy rather than a silent choice.

#include "awst/Node.h"
#include "builder/sol-types/SolcFwd.h"

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

/// Per-caller index recipe. Each preset reproduces its site's AWST exactly.
struct StoragePathPolicy
{
	/// BeforeBound: checkedIndexToUint64 (a wide index fails its own assert)
	/// ahead of the bounds check. AfterBound: compare at the index's own width
	/// (the bound is widened to it), then implicitNumericCast.
	enum class Narrow { BeforeBound, AfterBound };
	/// Never: the index node is shared by assert, key and element as built.
	/// Shared: assignment/call-valued indices pin first; a bounds-checked one
	/// pins unless var/constant and the assert reads a copy at the access
	/// location. UnlessConstant: everything but a bytes/integer constant.
	enum class Pin { Never, Shared, UnlessConstant };
	/// Fixed length above 2^64-1: skip the check, truncate (0 falls back to
	/// the runtime length), keep the wide literal, or reject.
	enum class StaticBound { SkipWide, TruncateWide, Plain, CheckedWide };

	Narrow narrow;
	Pin pin;
	char const* pinName;
	char const* pinCounter;
	StaticBound staticBound;
	/// Pin a dynamic array value before its length is read; nullptr reads the
	/// length off the value expression in place.
	char const* valuePinName;
	char const* valuePinCounter;
	/// Keep the element value only while it is itself an array (the next
	/// inline rank's bound); otherwise for every element.
	bool elementValueForArraysOnly;
	char const* boundsMessage;

	static StoragePathPolicy indexAccess();  ///< SolIndexAccess::handleMappingAccess
	static StoragePathPolicy getterKey();    ///< public getter key arguments
	static StoragePathPolicy getterInline(); ///< public getter inline ranks
	static StoragePathPolicy holder();       ///< MappingPrefix element resolution
};

/// Advances a holder through the mapping/array levels of a declared type.
/// Consecutive array ranks stay inline in the parent value; a mapping value
/// that is an array starts a new box. A key-less holder derives no key.
class StoragePathWalker
{
public:
	StoragePathWalker(
		TypeMapper& _typeMapper, StoragePathPolicy _policy,
		solidity::frontend::Type const* _root, awst::SourceLocation const& _loc);

	/// Container type at the cursor; nullptr past the declared chain.
	solidity::frontend::Type const* current() const { return m_type; }

	/// One level. Pins and bounds asserts go to `_pre` in policy order.
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
	std::shared_ptr<awst::Expression> pin(
		std::shared_ptr<awst::Expression> _value, char const* _name,
		char const* _counter, std::vector<std::shared_ptr<awst::Statement>>& _pre) const;

	TypeMapper& m_typeMapper;
	StoragePathPolicy m_policy;
	solidity::frontend::Type const* m_type;
	awst::SourceLocation m_loc;
};

} // namespace puyasol::builder
