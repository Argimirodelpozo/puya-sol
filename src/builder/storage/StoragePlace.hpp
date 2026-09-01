#pragma once

/// @file StoragePlace.hpp
/// Typed description of an addressable AVM storage root. Header-only (one
/// consumer; folded from the former .h/.cpp pair).
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
		std::shared_ptr<awst::Expression> _expression)
	{
		if (!_expression)
			return std::nullopt;
		_expression = awst::unwrapStateGet(std::move(_expression));
		while (auto const* cast = dynamic_cast<awst::ReinterpretCast const*>(
			_expression.get()))
			_expression = cast->expr;

		if (auto const* box = dynamic_cast<awst::BoxValueExpression const*>(
			_expression.get()); box && box->key)
			return StoragePlace{StoragePlaceKind::Box, box->key, box->wtype};
		if (auto const* state = dynamic_cast<awst::AppStateExpression const*>(
			_expression.get()); state && state->key)
			return StoragePlace{
				StoragePlaceKind::AppGlobal, state->key, state->wtype};

		// bytes/string state roots may be represented by their box key directly.
		if (auto const* key = dynamic_cast<awst::BytesConstant const*>(
			_expression.get()); key && key->wtype == awst::WType::boxKeyType())
			return StoragePlace{
				StoragePlaceKind::Box, std::move(_expression),
				awst::WType::bytesType()};

		return std::nullopt;
	}

	/// Storage places are compatible when their backend and structural value
	/// types agree.  WType instances are not globally interned, so pointer
	/// equality is intentionally insufficient here.
	bool hasSameShape(StoragePlace const& _other) const
	{
		if (kind != _other.kind || !valueType || !_other.valueType)
			return false;
		return awst::structurallyEquivalent(valueType, _other.valueType);
	}

	/// Construct the raw AWST state field for this place using `_key`.
	std::shared_ptr<awst::Expression> makeField(
		std::shared_ptr<awst::Expression> _key,
		awst::SourceLocation const& _loc) const
	{
		if (kind == StoragePlaceKind::Box)
			return awst::makeBoxValueExpression(
				awst::makeReinterpretCast(
					std::move(_key), awst::WType::boxKeyType(), _loc),
				valueType, _loc);
		return awst::makeAppStateExpression(
			awst::makeReinterpretCast(
				std::move(_key), awst::WType::stateKeyType(), _loc),
			valueType, _loc);
	}
};

} // namespace puyasol::builder
