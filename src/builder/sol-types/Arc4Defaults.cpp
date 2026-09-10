#include "builder/sol-types/Arc4Defaults.h"

#include <utility>

namespace puyasol::builder
{

namespace
{

// Defaults are consumed as one stack value or one box initializer, never an
// unbounded host allocation. Larger zero regions use size-only creation paths.
constexpr uint64_t kMaxMaterializedDefault = 32768;

// ARC4 sequence layout: bool runs occupy ceil(N/8) bytes, other fields
// retain their own layout. Both sizing and default encoding consume these runs.
template<class Visit>
bool visitAggregateRuns(std::vector<awst::WType const*> const& fields, Visit visit)
{
	uint64_t bools = 0;
	auto flush = [&]() {
		auto const count = std::exchange(bools, 0);
		return !count || visit(nullptr, EncodedSize::fixed(count / 8 + (count % 8 != 0)));
	};
	for (auto const* field: fields)
	{
		if (field == awst::WType::arc4BoolType()) ++bools;
		else if (!flush() || !visit(field, computeEncodedElementSize(field))) return false;
	}
	return flush();
}

std::optional<std::vector<uint8_t>> arc4AggregateDefaultEncoding(
	std::vector<awst::WType const*> const& _fields)
{
	struct FieldEncoding { bool dynamic; std::vector<uint8_t> bytes; };
	std::vector<FieldEncoding> encodings;
	uint64_t headSize = 0, totalSize = 0;
	bool const valid = visitAggregateRuns(_fields, [&](awst::WType const* field, EncodedSize size) {
		if (!field && size.bytes > kMaxMaterializedDefault) return false;
		auto bytes = field ? arc4DefaultEncoding(field)
			: std::optional<std::vector<uint8_t>>{std::vector<uint8_t>(size.bytes, 0)};
		if (!bytes) return false;
		bool const dynamic = size.kind == EncodedSize::Kind::Dynamic;
		headSize += dynamic ? 2 : bytes->size();
		totalSize += bytes->size() + (dynamic ? 2 : 0);
		if (totalSize > kMaxMaterializedDefault) return false;
		encodings.push_back({dynamic, std::move(*bytes)});
		return true;
	});
	if (!valid || headSize > 0xFFFF) return std::nullopt;
	std::vector<uint8_t> head, tail;
	head.reserve(totalSize);
	for (auto const& field: encodings)
	{
		if (field.dynamic)
		{
			auto const offset = headSize + tail.size();
			if (offset > 0xFFFF) return std::nullopt;
			head.push_back(static_cast<uint8_t>(offset >> 8));
			head.push_back(static_cast<uint8_t>(offset));
			tail.insert(tail.end(), field.bytes.begin(), field.bytes.end());
		}
		else
			head.insert(head.end(), field.bytes.begin(), field.bytes.end());
	}
	head.insert(head.end(), tail.begin(), tail.end());
	return head;
}

EncodedSize arc4AggregateEncodedSize(std::vector<awst::WType const*> const& _fields)
{
	auto total = EncodedSize::fixed(0);
	visitAggregateRuns(_fields, [&](awst::WType const*, EncodedSize size) {
		total = total.plus(size);
		return true; // Validate every field, even after a dynamic field.
	});
	return total;
}

std::vector<awst::WType const*> arc4StructFieldTypes(
	awst::ARC4Struct const* _type)
{
	std::vector<awst::WType const*> result;
	result.reserve(_type->fields().size());
	for (auto const& [name, fieldType]: _type->fields())
		result.push_back(fieldType);
	return result;
}

} // namespace

bool isArc4EncodedType(awst::WType const* _type)
{
	if (!_type)
		return false;
	if (_type == awst::WType::arc4BoolType())
		return true;
	switch (_type->kind())
	{
	case awst::WTypeKind::ARC4UIntN:
	case awst::WTypeKind::ARC4UFixedNxM:
	case awst::WTypeKind::ARC4Tuple:
	case awst::WTypeKind::ARC4DynamicArray:
	case awst::WTypeKind::ARC4StaticArray:
	case awst::WTypeKind::ARC4Struct:
		return true;
	default:
		return false;
	}
}

std::shared_ptr<awst::Expression> makeZeroBytesRuntime(
	int _n,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc)
{
	auto bzero = awst::makeBzero(_n, _loc);
	if (_targetType == awst::WType::bytesType())
		return bzero;
	return awst::makeReinterpretCast(std::move(bzero), _targetType, _loc);
}

bool arc4IsDynamic(awst::WType const* _type)
{
	if (!_type) return false;
	auto const size = computeEncodedElementSize(_type);
	size.fixedBytes(); // Unsupported/overflow must not masquerade as fixed encoding.
	return size.kind == EncodedSize::Kind::Dynamic;
}

std::optional<std::vector<uint8_t>> arc4DefaultEncoding(awst::WType const* _type)
{
	if (!_type)
		return std::nullopt;
	auto const size = computeEncodedElementSize(_type);
	if (size.kind == EncodedSize::Kind::Overflow
		|| size.kind == EncodedSize::Kind::Unsupported
		|| (size.kind == EncodedSize::Kind::Fixed && size.bytes > kMaxMaterializedDefault))
		return std::nullopt;
	// Every fixed encoding has the same all-zero default. Packed standalone
	// bools use one byte; dynamic containers below supply their offset/length
	// headers. This is the same layout used for sizing and classification.
	if (size.kind == EncodedSize::Kind::Fixed)
		return std::vector<uint8_t>(static_cast<size_t>(size.bytes), 0);
	if (size.kind == EncodedSize::Kind::Packed)
		return std::vector<uint8_t>{0};
	switch (_type->kind())
	{
	case awst::WTypeKind::ARC4DynamicArray:
		return std::vector<uint8_t>{0, 0};
	case awst::WTypeKind::ARC4StaticArray:
	{
		auto const* array = static_cast<awst::ARC4StaticArray const*>(_type);
		auto element = arc4DefaultEncoding(array->elementType());
		if (!element) return std::nullopt;
		auto const count = static_cast<uint64_t>(array->arraySize());
		auto const stride = 2 + element->size();
		if (count > kMaxMaterializedDefault / stride) return std::nullopt;
		uint64_t const headSize = count * 2;
		std::vector<uint8_t> result;
		result.reserve(count * stride);
		for (uint64_t i = 0; i < count; ++i)
		{
			auto const offset = headSize + i * element->size();
			if (offset > 0xFFFF) return std::nullopt;
			result.push_back(static_cast<uint8_t>(offset >> 8));
			result.push_back(static_cast<uint8_t>(offset));
		}
		for (uint64_t i = 0; i < count; ++i)
			result.insert(result.end(), element->begin(), element->end());
		return result;
	}
	case awst::WTypeKind::ARC4Struct:
	{
		auto const* st = static_cast<awst::ARC4Struct const*>(_type);
		return arc4AggregateDefaultEncoding(arc4StructFieldTypes(st));
	}
	case awst::WTypeKind::ARC4Tuple:
	{
		auto const* tuple = static_cast<awst::ARC4Tuple const*>(_type);
		return arc4AggregateDefaultEncoding(tuple->types());
	}
	case awst::WTypeKind::Bytes:
		return std::vector<uint8_t>{0, 0};
	case awst::WTypeKind::Basic:
		if (_type == awst::WType::stringType()) return std::vector<uint8_t>{0, 0};
		return std::nullopt;
	default:
		return std::nullopt;
	}
}

EncodedSize computeEncodedElementSize(awst::WType const* _type)
{
	if (!_type)
		return {EncodedSize::Kind::Unsupported};

	switch (_type->kind())
	{
	case awst::WTypeKind::ARC4UIntN:
		return EncodedSize::fixed(static_cast<awst::ARC4UIntN const*>(_type)->n() / 8);
	case awst::WTypeKind::ARC4UFixedNxM:
		return EncodedSize::fixed(static_cast<awst::ARC4UFixedNxM const*>(_type)->n() / 8);
	case awst::WTypeKind::ARC4Struct:
	{
		auto const* structType = static_cast<awst::ARC4Struct const*>(_type);
		return arc4AggregateEncodedSize(arc4StructFieldTypes(structType));
	}
	case awst::WTypeKind::ARC4Tuple:
	{
		auto const* tuple = static_cast<awst::ARC4Tuple const*>(_type);
		return arc4AggregateEncodedSize(tuple->types());
	}
	case awst::WTypeKind::ARC4StaticArray:
	{
		auto const* arr = static_cast<awst::ARC4StaticArray const*>(_type);
		if (arr->arraySize() < 0)
			return {EncodedSize::Kind::Unsupported};
		auto const count = static_cast<uint64_t>(arr->arraySize());
		if (arr->elementType() == awst::WType::arc4BoolType())
			return EncodedSize::fixed(count / 8 + (count % 8 != 0));
		return computeEncodedElementSize(arr->elementType()).times(count);
	}
	case awst::WTypeKind::ReferenceArray:
	{
		auto const* arr = static_cast<awst::ReferenceArray const*>(_type);
		if (!arr->arraySize())
			return {EncodedSize::Kind::Dynamic};
		if (*arr->arraySize() < 0)
			return {EncodedSize::Kind::Unsupported};
		return computeEncodedElementSize(arr->elementType()).times(*arr->arraySize());
	}
	case awst::WTypeKind::ARC4DynamicArray:
	{
		auto const element = computeEncodedElementSize(
			static_cast<awst::ARC4DynamicArray const*>(_type)->elementType());
		if (element.kind == EncodedSize::Kind::Unsupported || element.kind == EncodedSize::Kind::Overflow)
			return element;
		return {EncodedSize::Kind::Dynamic};
	}
	case awst::WTypeKind::Bytes:
	{
		auto const* bytesType = static_cast<awst::BytesWType const*>(_type);
		if (bytesType->length())
		{
			if (*bytesType->length() < 0)
				return {EncodedSize::Kind::Unsupported};
			return EncodedSize::fixed(*bytesType->length());
		}
		return {EncodedSize::Kind::Dynamic};
	}
	case awst::WTypeKind::Basic:
	{
		if (_type == awst::WType::biguintType())
			return EncodedSize::fixed(32);
		if (_type == awst::WType::uint64Type())
			return EncodedSize::fixed(8);
		if (_type == awst::WType::boolType())
			return EncodedSize::fixed(8);
		if (_type == awst::WType::accountType())
			return EncodedSize::fixed(32);
		if (_type == awst::WType::arc4BoolType())
			return {EncodedSize::Kind::Packed};
		if (_type == awst::WType::stringType())
			return {EncodedSize::Kind::Dynamic};
		return {EncodedSize::Kind::Unsupported};
	}
	default:
		return {EncodedSize::Kind::Unsupported};
	}
}

bool memoryUsesBlob(awst::WType const* _type)
{
	if (!_type)
		return false;
	// Single control point for the "memory aggregate lives in the scratch blob/region model"
	// decision (a uint64 (region,offset) pointer) vs a flat ARC4 value. Currently the original
	// rule: encoded size exceeds one memory slot (AssemblyBuilder::SLOT_SIZE = 4096; literal
	// here to keep this leaf TU free of the AssemblyBuilder include).
	//
	// HANDLE-MODEL STAGE 2 (memory→memory aliasing) will extend this to route 1D scalar arrays
	// through the region model so `b = a` ALIASES (matches EVM). That extension is gated on
	// FIRST hardening the blob model for the common small-array ops — verified blocker: routing
	// `uint[]` here breaks `T memory a = new uint[](N)` inline-init ("assignment target type
	// differs from expression value type": the new-array value vs the uint64 offset binding).
	// Flip here once the blob model handles new-init / push / etc. for small arrays.
	return computeEncodedElementSize(_type).fixedBytes().value_or(0) > 4096;
}

} // namespace puyasol::builder
