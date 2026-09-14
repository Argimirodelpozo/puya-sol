#include "builder/storage/named/StoragePathWalker.h"
#include "builder/solc/StorageRefPointer.h"
#include "builder/types/EncodedSize.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/TypeMapper.h"
#include "builder/storage/named/StorageKey.h"
#include "builder/storage/StorageMapper.h"
#include "awst/NameGen.h"

#include <libsolidity/ast/AST.h>

#include <limits>

namespace puyasol::builder
{
using namespace solidity::frontend;

StoragePathWalker::StoragePathWalker(
	TypeMapper& _typeMapper, Type const* _root, awst::SourceLocation const& _loc,
	ValueTracking _tracking)
	: m_typeMapper(_typeMapper), m_tracking(_tracking), m_type(_root), m_loc(_loc)
{
}

StorageHolder StoragePathWalker::step(
	StorageHolder _holder, std::shared_ptr<awst::Expression> _index,
	std::vector<std::shared_ptr<awst::Statement>>& _pre)
{
	auto const* array = dynamic_cast<ArrayType const*>(m_type);
	auto const* mapping = dynamic_cast<MappingType const*>(m_type);
	if (!array && !mapping) throw SizeError("storage path requires an array or mapping");
	auto const* keyWType = mapping
		? m_typeMapper.map(mapping->keyType()) : awst::WType::uint64Type();

	// The bound, holder key and inline value share an index. Freeze it before
	// a later path operand can mutate its source; never freeze a whole box value.
	if (!awst::isConstantExpression(_index.get()))
	{
		auto variable = awst::makeVarExpression("__storage_index_" + std::to_string(
			awst::NameGen::next("StoragePathWalker.index")), _index->wtype, m_loc);
		_pre.push_back(awst::makeAssignmentStatement(variable, std::move(_index), m_loc));
		_index = std::move(variable);
	}
	if (array)
	{
		std::shared_ptr<awst::Expression> bound;
		auto const* compareType = _index->wtype;
		if (array->isDynamicallySized())
		{
			// Nested array lengths belong to the enclosing serialized box,
			// not to the separately derived mapping-holder identity.
			if (!_holder.value) throw SizeError("dynamic storage array has no addressable length");
			bound = awst::makeArrayLength(_holder.value, awst::WType::uint64Type(), m_loc);
		}
		else
		{
			if (array->length() > std::numeric_limits<uint64_t>::max())
				compareType = awst::WType::biguintType();
			bound = awst::makeIntegerConstant(array->length().str(), m_loc, compareType);
		}
		_pre.push_back(awst::makeExpressionStatement(awst::makeAssert(
			awst::makeNumericCompare(
				TypeCoercion::implicitNumericCast(_index, compareType, m_loc),
				awst::NumericComparison::Lt,
				TypeCoercion::implicitNumericCast(std::move(bound), compareType, m_loc), m_loc),
			m_loc, "array index out of bounds"), m_loc));
		// solc owns the logical bound; AVM byte/box addressing owns this capacity.
		_index = TypeCoercion::checkedIndexToUint64(_pre, std::move(_index), m_loc);
	}
	else
		_index = TypeCoercion::implicitNumericCast(std::move(_index), keyWType, m_loc);

	Type const* next = array ? array->baseType() : mapping->valueType();
	StorageHolder child;
	if (_holder.value && array
		&& (m_tracking == ValueTracking::All || dynamic_cast<ArrayType const*>(next)))
		child.value = awst::makeIndexExpression(
			_holder.value, _index, m_typeMapper.mapSolTypeToARC4(next), m_loc);
	if (_holder.key)
		child.key = array
			? StorageKey::arrayElement(std::move(_holder.key), _index, m_loc)
			: StorageKey::mappingEntry(std::move(_holder.key), _index, keyWType, m_loc);
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
