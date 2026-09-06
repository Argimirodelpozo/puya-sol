#include "builder/storage/StoragePathWalker.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/sol-types/EncodedSize.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/storage/StorageKey.h"
#include "builder/storage/StorageMapper.h"
#include "awst/NameGen.h"

#include <libsolidity/ast/AST.h>

#include <limits>

namespace puyasol::builder
{
using namespace solidity::frontend;

StoragePathPolicy StoragePathPolicy::indexAccess()
{
	return {Narrow::AfterBound, Pin::Shared, "__sol_idx_", "SolIndexAccessHandlers.idxTempCounter",
		StaticBound::SkipWide, nullptr, nullptr, true, "array index out of bounds"};
}

StoragePathPolicy StoragePathPolicy::getterKey()
{
	return {Narrow::BeforeBound, Pin::Never, nullptr, nullptr,
		StaticBound::TruncateWide, nullptr, nullptr, true, "array out-of-bounds"};
}

StoragePathPolicy StoragePathPolicy::getterInline()
{
	return {Narrow::BeforeBound, Pin::Never, nullptr, nullptr, StaticBound::Plain,
		"__idx_bounds_", "PublicGetterBuilder.idxBounds", false, "array out-of-bounds"};
}

StoragePathPolicy StoragePathPolicy::holder()
{
	return {Narrow::BeforeBound, Pin::UnlessConstant, "__holder_", "MappingPrefix.pin",
		StaticBound::CheckedWide, nullptr, nullptr, false, "array index out of bounds"};
}

StoragePathWalker::StoragePathWalker(
	TypeMapper& _typeMapper, StoragePathPolicy _policy, Type const* _root,
	awst::SourceLocation const& _loc)
	: m_typeMapper(_typeMapper), m_policy(_policy), m_type(_root), m_loc(_loc)
{
}

std::shared_ptr<awst::Expression> StoragePathWalker::pin(
	std::shared_ptr<awst::Expression> _value, char const* _name, char const* _counter,
	std::vector<std::shared_ptr<awst::Statement>>& _pre) const
{
	auto variable = awst::makeVarExpression(
		_name + std::to_string(awst::NameGen::next(_counter)), _value->wtype, m_loc);
	_pre.push_back(awst::makeAssignmentStatement(variable, std::move(_value), m_loc));
	return variable;
}

StorageHolder StoragePathWalker::step(
	StorageHolder _holder, std::shared_ptr<awst::Expression> _index,
	std::vector<std::shared_ptr<awst::Statement>>& _pre)
{
	using Narrow = StoragePathPolicy::Narrow;
	using Pin = StoragePathPolicy::Pin;
	using StaticBound = StoragePathPolicy::StaticBound;
	auto const* array = dynamic_cast<ArrayType const*>(m_type);
	auto const* mapping = dynamic_cast<MappingType const*>(m_type);
	awst::WType const* keyWType = mapping
		? m_typeMapper.map(mapping->keyType()) : awst::WType::uint64Type();

	// A side-effecting index is embedded in a key that compound assignments
	// reference twice; evaluate it once, before any coercion can hide it.
	if (m_policy.pin == Pin::Shared
		&& (dynamic_cast<awst::AssignmentExpression const*>(_index.get())
			|| dynamic_cast<awst::SubroutineCallExpression const*>(_index.get())))
		_index = pin(std::move(_index), m_policy.pinName, m_policy.pinCounter, _pre);

	if (array)
	{
		if (m_policy.narrow == Narrow::BeforeBound)
			_index = TypeCoercion::checkedIndexToUint64(_pre, std::move(_index), m_loc);

		std::shared_ptr<awst::Expression> bound;
		bool dynamic = array->isDynamicallySized();
		if (!dynamic)
			switch (m_policy.staticBound)
			{
			case StaticBound::SkipWide:
				if (array->length() <= std::numeric_limits<uint64_t>::max())
					bound = awst::makeIntegerConstant(array->length().str(), m_loc);
				break;
			case StaticBound::TruncateWide:
				if (auto const n = static_cast<uint64_t>(array->length()))
					bound = awst::makeIntegerConstant(
						std::to_string(n), m_loc, awst::WType::uint64Type());
				else
					dynamic = true;
				break;
			case StaticBound::Plain:
				bound = awst::makeIntegerConstant(
					array->length().str(), m_loc, awst::WType::uint64Type());
				break;
			case StaticBound::CheckedWide:
				bound = awst::makeIntegerConstant(
					checkedSize<uint64_t>(array->length(), "holder array bound"), m_loc);
				break;
			}
		if (dynamic)
		{
			// Nested ranks are encoded inside their parent box: the length lives
			// in the tracked value, never in a synthetic descendant box.
			if (!_holder.value)
				throw SizeError("dynamic mapping-holder array has no addressable length");
			if (m_policy.valuePinName)
				_holder.value = pin(std::move(_holder.value),
					m_policy.valuePinName, m_policy.valuePinCounter, _pre);
			bound = awst::makeArrayLength(_holder.value, awst::WType::uint64Type(), m_loc);
		}
		if (bound)
		{
			// The assert and the key segment both reference the index.
			bool materialise = false;
			if (m_policy.pin == Pin::Shared)
				materialise = !dynamic_cast<awst::VarExpression const*>(_index.get())
					&& !dynamic_cast<awst::IntegerConstant const*>(_index.get());
			else if (m_policy.pin == Pin::UnlessConstant)
				materialise = !dynamic_cast<awst::BytesConstant const*>(_index.get())
					&& !dynamic_cast<awst::IntegerConstant const*>(_index.get());
			if (materialise)
				_index = pin(std::move(_index), m_policy.pinName, m_policy.pinCounter, _pre);
			auto ref = _index;
			if (m_policy.pin == Pin::Shared)
			{
				if (auto const* ve = dynamic_cast<awst::VarExpression const*>(_index.get()))
					ref = awst::makeVarExpression(ve->name, ve->wtype, m_loc);
				else if (auto const* ic = dynamic_cast<awst::IntegerConstant const*>(_index.get()))
					ref = awst::makeIntegerConstant(ic->value, m_loc, ic->wtype);
				bound = TypeCoercion::implicitNumericCast(std::move(bound), ref->wtype, m_loc);
			}
			_pre.push_back(awst::makeExpressionStatement(awst::makeAssert(
				awst::makeNumericCompare(
					std::move(ref), awst::NumericComparison::Lt, std::move(bound), m_loc),
				m_loc, m_policy.boundsMessage), m_loc));
		}
		if (m_policy.narrow == Narrow::AfterBound)
			_index = TypeCoercion::implicitNumericCast(std::move(_index), keyWType, m_loc);
	}
	else
		_index = TypeCoercion::implicitNumericCast(std::move(_index), keyWType, m_loc);

	Type const* next = array ? array->baseType() : mapping ? mapping->valueType() : nullptr;
	StorageHolder child;
	if (_holder.value && array
		&& (!m_policy.elementValueForArraysOnly || dynamic_cast<ArrayType const*>(next)))
		child.value = awst::makeIndexExpression(
			_holder.value, _index, m_typeMapper.mapSolTypeToARC4(next), m_loc);
	if (_holder.key)
		child.key = array
			? StorageKey::arrayElement(std::move(_holder.key), _index, m_loc)
			: StorageKey::mappingEntry(std::move(_holder.key), _index, keyWType, m_loc);
	// A mapping value starts a new serialized box; an array there feeds the
	// bounds of the inline ranks that follow.
	if (!array && child.key)
		if (auto const* nextArray = dynamic_cast<ArrayType const*>(next))
			child.value = boxedValue(m_typeMapper, child.key, nextArray, m_loc);
	m_type = next;
	return child;
}

StorageHolder StoragePathWalker::member(
	StorageHolder _base, StructType const& _type, std::string const& _name,
	awst::WType const* _valueType, awst::SourceLocation const& _loc)
{
	if (!_base.key) return {};
	if (transparentMappingWrapper(&_type))
		return _base; // same represented fields and addressable data path
	return {StorageKey::member(std::move(_base.key), _type, _name, _loc),
		awst::makeFieldExpression(std::move(_base.value), _name, _valueType, _loc)};
}

std::shared_ptr<awst::Expression> StoragePathWalker::boxedValue(
	TypeMapper& _typeMapper, std::shared_ptr<awst::Expression> _key, Type const* _type,
	awst::SourceLocation const& _loc)
{
	auto const* wt = _typeMapper.map(_type);
	auto box = awst::makeBoxValueExpression(std::move(_key), wt, _loc);
	return StorageMapper::makeStateGetWithDefault(std::move(box), wt, _loc);
}

} // namespace puyasol::builder
