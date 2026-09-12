#pragma once

/// @file StoragePlace.hpp
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
#include <stdexcept>

namespace puyasol::builder
{

enum class StoragePlaceKind
{
	AppGlobal,
	Box,
};

struct StoragePlace
{
	/// Only these wrappers preserve the addressed storage location. Callers
	/// stop at StateGet when they need an unmaterialized raw projection.
	static std::shared_ptr<awst::Expression> projectionBase(
		std::shared_ptr<awst::Expression> const& value)
	{
		if (auto index = std::dynamic_pointer_cast<awst::IndexExpression>(value)) return index->base;
		if (auto field = std::dynamic_pointer_cast<awst::FieldExpression>(value)) return field->base;
		if (auto cast = std::dynamic_pointer_cast<awst::ReinterpretCast>(value)) return cast->expr;
		if (auto decode = std::dynamic_pointer_cast<awst::ARC4Decode>(value)) return decode->value;
		if (auto read = std::dynamic_pointer_cast<awst::StateGet>(value)) return read->field;
		return nullptr;
	}

	/// Rebuild a projection with a resolved base, preserving all node metadata.
	static std::shared_ptr<awst::Expression> withProjectionBase(
		std::shared_ptr<awst::Expression> const& value,
		std::shared_ptr<awst::Expression> base)
	{
		if (auto const* node = dynamic_cast<awst::IndexExpression const*>(value.get()))
		{
			auto copy = std::make_shared<awst::IndexExpression>(*node);
			copy->base = std::move(base); return copy;
		}
		if (auto const* node = dynamic_cast<awst::FieldExpression const*>(value.get()))
		{
			auto copy = std::make_shared<awst::FieldExpression>(*node);
			copy->base = std::move(base); return copy;
		}
		if (auto const* node = dynamic_cast<awst::ReinterpretCast const*>(value.get()))
		{
			auto copy = std::make_shared<awst::ReinterpretCast>(*node);
			copy->expr = std::move(base); return copy;
		}
		if (auto const* node = dynamic_cast<awst::ARC4Decode const*>(value.get()))
		{
			auto copy = std::make_shared<awst::ARC4Decode>(*node);
			copy->value = std::move(base); return copy;
		}
		if (auto const* node = dynamic_cast<awst::StateGet const*>(value.get()))
		{
			auto copy = std::make_shared<awst::StateGet>(*node);
			copy->field = std::move(base); return copy;
		}
		throw std::logic_error("Expected an address-preserving projection");
	}

	StoragePlaceKind kind;
	std::shared_ptr<awst::Expression> key;
	awst::WType const* valueType;
	bool preserveEmptyBox = false;

	/// Recover a root place from a storage read or raw state field.  A single
	/// StateGet and value-only reinterpret casts are transparent, in any order.
	static std::optional<StoragePlace> fromRead(
		std::shared_ptr<awst::Expression> _expression)
	{
		if (!_expression)
			return std::nullopt;
		while (_expression)
		{
			if (dynamic_cast<awst::StateGet const*>(_expression.get())
				|| dynamic_cast<awst::ReinterpretCast const*>(_expression.get()))
				_expression = projectionBase(_expression);
			else
				break;
		}

		if (auto const* box = dynamic_cast<awst::BoxValueExpression const*>(
			_expression.get()); box && box->key)
			return StoragePlace{StoragePlaceKind::Box, box->key, box->wtype, box->preserveEmptyBox};
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
		{
			auto box = awst::makeBoxValueExpression(
				awst::makeReinterpretCast(
					std::move(_key), awst::WType::boxKeyType(), _loc),
				valueType, _loc);
			box->preserveEmptyBox = preserveEmptyBox;
			return box;
		}
		return awst::makeAppStateExpression(
			awst::makeReinterpretCast(
				std::move(_key), awst::WType::stateKeyType(), _loc),
			valueType, _loc);
	}
};

} // namespace puyasol::builder
