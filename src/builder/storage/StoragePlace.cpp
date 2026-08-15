#include "builder/storage/StoragePlace.h"

namespace puyasol::builder
{

std::optional<StoragePlace> StoragePlace::fromRead(
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
		return StoragePlace{StoragePlaceKind::AppGlobal, state->key, state->wtype};

	// bytes/string state roots may be represented by their box key directly.
	if (auto const* key = dynamic_cast<awst::BytesConstant const*>(
		_expression.get()); key && key->wtype == awst::WType::boxKeyType())
		return StoragePlace{
			StoragePlaceKind::Box, std::move(_expression), awst::WType::bytesType()};

	return std::nullopt;
}

bool StoragePlace::hasSameShape(StoragePlace const& _other) const
{
	if (kind != _other.kind || !valueType || !_other.valueType)
		return false;
	return awst::structurallyEquivalent(valueType, _other.valueType);
}

std::shared_ptr<awst::Expression> StoragePlace::makeField(
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

} // namespace puyasol::builder
