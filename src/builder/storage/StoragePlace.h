#pragma once

/// @file StoragePlace.h
/// Typed description of an addressable AVM storage root.
///
/// AWST deliberately uses the same expression hierarchy for reads and writable
/// state fields.  Builder code must nevertheless distinguish the *place* (the
/// backend and key) from the value obtained by reading it.  Keeping that
/// distinction here avoids repeating fragile dynamic_cast ladders at every
/// storage-pointer binding site.

#include "awst/Node.h"

#include <memory>
#include <optional>

namespace puyasol::builder
{

enum class StoragePlaceKind
{
	AppGlobal,
	Box,
};

struct StoragePlace
{
	StoragePlaceKind kind;
	std::shared_ptr<awst::Expression> key;
	awst::WType const* valueType;

	/// Recover a root place from a storage read or raw state field.  A single
	/// StateGet and value-only reinterpret casts are transparent.
	static std::optional<StoragePlace> fromRead(
		std::shared_ptr<awst::Expression> _expression);

	/// Storage places are compatible when their backend and structural value
	/// types agree.  WType instances are not globally interned, so pointer
	/// equality is intentionally insufficient here.
	bool hasSameShape(StoragePlace const& _other) const;

	/// Construct the raw AWST state field for this place using `_key`.
	std::shared_ptr<awst::Expression> makeField(
		std::shared_ptr<awst::Expression> _key,
		awst::SourceLocation const& _loc) const;
};

} // namespace puyasol::builder
