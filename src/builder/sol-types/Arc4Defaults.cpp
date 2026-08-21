#include "builder/sol-types/Arc4Defaults.h"

namespace puyasol::builder
{

namespace
{

std::optional<std::vector<uint8_t>> arc4AggregateDefaultEncoding(
	std::vector<awst::WType const*> const& _fields)
{
	// ARC4 structs and tuples share the same sequence encoding: consecutive
	// bools form packed runs, fixed fields live in the head, and dynamic fields
	// contribute a uint16 head offset plus their default encoding in the tail.
	enum Kind { Bool, Static, Dynamic };
	struct FieldEncoding { Kind kind; std::vector<uint8_t> bytes; };
	std::vector<FieldEncoding> encodings;
	encodings.reserve(_fields.size());
	int64_t headSize = 0;
	int boolRun = 0;
	auto flushBoolRun = [&]() {
		if (boolRun > 0)
		{
			headSize += (boolRun + 7) / 8;
			boolRun = 0;
		}
	};
	for (auto const* fieldType: _fields)
	{
		if (fieldType == awst::WType::arc4BoolType())
		{
			encodings.push_back({Bool, {}});
			boolRun++;
			continue;
		}
		flushBoolRun();
		auto fieldDefault = arc4DefaultEncoding(fieldType);
		if (!fieldDefault)
			return std::nullopt;
		bool const dynamic = arc4IsDynamic(fieldType);
		headSize += dynamic ? 2 : static_cast<int64_t>(fieldDefault->size());
		encodings.push_back({
			dynamic ? Dynamic : Static, std::move(*fieldDefault)});
	}
	flushBoolRun();
	if (headSize > 0xFFFF)
		return std::nullopt;

	std::vector<uint8_t> head;
	std::vector<uint8_t> tail;
	head.reserve(static_cast<size_t>(headSize));
	int64_t tailOffset = headSize;
	int pendingBools = 0;
	auto emitBoolRun = [&]() {
		if (pendingBools > 0)
		{
			head.insert(
				head.end(), static_cast<size_t>((pendingBools + 7) / 8), 0);
			pendingBools = 0;
		}
	};
	for (auto const& encoding: encodings)
	{
		if (encoding.kind == Bool)
		{
			pendingBools++;
			continue;
		}
		emitBoolRun();
		if (encoding.kind == Dynamic)
		{
			if (tailOffset > 0xFFFF)
				return std::nullopt;
			head.push_back(static_cast<uint8_t>((tailOffset >> 8) & 0xFF));
			head.push_back(static_cast<uint8_t>(tailOffset & 0xFF));
			tail.insert(tail.end(), encoding.bytes.begin(), encoding.bytes.end());
			tailOffset += static_cast<int64_t>(encoding.bytes.size());
		}
		else
			head.insert(head.end(), encoding.bytes.begin(), encoding.bytes.end());
	}
	emitBoolRun();
	head.insert(head.end(), tail.begin(), tail.end());
	return head;
}

int arc4AggregateEncodedSize(std::vector<awst::WType const*> const& _fields)
{
	int total = 0;
	int boolRun = 0;
	auto flushBoolRun = [&]() {
		if (boolRun > 0)
		{
			total += (boolRun + 7) / 8;
			boolRun = 0;
		}
	};
	for (auto const* fieldType: _fields)
	{
		if (fieldType == awst::WType::arc4BoolType())
		{
			boolRun++;
			continue;
		}
		flushBoolRun();
		int const fieldSize = computeEncodedElementSize(fieldType);
		if (fieldSize == 0)
			return 0;
		total += fieldSize;
	}
	flushBoolRun();
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

std::shared_ptr<awst::Expression> prependArc4LengthHeader(
	std::shared_ptr<awst::Expression> _expr,
	int64_t /*_length*/,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc)
{
	// puya's ConvertArray adds/strips the uint16 length header.
	return awst::makeConvertArray(std::move(_expr), _targetType, _loc);
}

bool arc4IsDynamic(awst::WType const* _type)
{
	if (!_type)
		return false;
	switch (_type->kind())
	{
	case awst::WTypeKind::ARC4DynamicArray:
		return true;
	case awst::WTypeKind::ARC4StaticArray:
	{
		auto const* arr = static_cast<awst::ARC4StaticArray const*>(_type);
		return arc4IsDynamic(arr->elementType());
	}
	case awst::WTypeKind::ARC4Struct:
	{
		auto const* st = static_cast<awst::ARC4Struct const*>(_type);
		for (auto const& [name, ft]: st->fields())
			if (arc4IsDynamic(ft))
				return true;
		return false;
	}
	case awst::WTypeKind::ARC4Tuple:
	{
		auto const* tu = static_cast<awst::ARC4Tuple const*>(_type);
		for (auto const* ft: tu->types())
			if (arc4IsDynamic(ft))
				return true;
		return false;
	}
	case awst::WTypeKind::Bytes:
	{
		auto const* bw = static_cast<awst::BytesWType const*>(_type);
		return !bw->length().has_value();
	}
	default:
		return false;
	}
}

std::optional<std::vector<uint8_t>> arc4DefaultEncoding(awst::WType const* _type)
{
	if (!_type)
		return std::nullopt;
	switch (_type->kind())
	{
	case awst::WTypeKind::ARC4UIntN:
	{
		auto const* u = static_cast<awst::ARC4UIntN const*>(_type);
		return std::vector<uint8_t>(static_cast<size_t>(u->n() / 8), 0);
	}
	case awst::WTypeKind::ARC4UFixedNxM:
	{
		auto const* u = static_cast<awst::ARC4UFixedNxM const*>(_type);
		return std::vector<uint8_t>(static_cast<size_t>(u->n() / 8), 0);
	}
	case awst::WTypeKind::ARC4DynamicArray:
		return std::vector<uint8_t>{0, 0};
	case awst::WTypeKind::ARC4StaticArray:
	{
		auto const* arr = static_cast<awst::ARC4StaticArray const*>(_type);
		auto const* elemT = arr->elementType();
		auto N = static_cast<int64_t>(arr->arraySize());
		if (N < 0)
			return std::nullopt;
		// ARC4 packs consecutive bool array elements eight per byte. Treat the
		// complete bool run as the array's element encoding boundary here rather
		// than pretending every bool occupies a byte. An outer fixed array then
		// recurses normally and repeats this correctly packed inner encoding, so
		// bool[M][N][...] needs no rank-specific handling.
		if (elemT == awst::WType::arc4BoolType())
			return std::vector<uint8_t>(static_cast<size_t>((N + 7) / 8), 0);
		auto elemDefault = arc4DefaultEncoding(elemT);
		if (!elemDefault)
			return std::nullopt;

		std::vector<uint8_t> result;
		if (arc4IsDynamic(elemT))
		{
			int64_t headSize = N * 2;
			int64_t tailSize = static_cast<int64_t>(elemDefault->size());
			// ARC4 dynamic-element offsets are uint16; bail out if the final
			// element's offset (headSize+(N-1)*tailSize) would exceed 0xFFFF
			// rather than emit a silently wrapped/corrupt offset header.
			if (headSize > 0xFFFF
				|| (tailSize > 0 && N > 0 && (N - 1) > (0xFFFF - headSize) / tailSize))
				return std::nullopt;
			result.reserve(static_cast<size_t>(headSize + N * tailSize));
			for (int64_t i = 0; i < N; ++i)
			{
				int64_t off = headSize + i * tailSize;
				result.push_back(static_cast<uint8_t>((off >> 8) & 0xFF));
				result.push_back(static_cast<uint8_t>(off & 0xFF));
			}
			for (int64_t i = 0; i < N; ++i)
				result.insert(result.end(), elemDefault->begin(), elemDefault->end());
		}
		else
		{
			result.reserve(static_cast<size_t>(N * static_cast<int64_t>(elemDefault->size())));
			for (int64_t i = 0; i < N; ++i)
				result.insert(result.end(), elemDefault->begin(), elemDefault->end());
		}
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
	{
		auto const* bw = static_cast<awst::BytesWType const*>(_type);
		if (bw->length().has_value())
			return std::vector<uint8_t>(static_cast<size_t>(*bw->length()), 0);
		return std::vector<uint8_t>{0, 0};
	}
	case awst::WTypeKind::Basic:
	{
		if (_type == awst::WType::biguintType())
			return std::vector<uint8_t>(32, 0);
		if (_type == awst::WType::uint64Type())
			return std::vector<uint8_t>(8, 0);
		if (_type == awst::WType::boolType())
			return std::vector<uint8_t>{0};
		if (_type == awst::WType::arc4BoolType())
			return std::vector<uint8_t>{0};
		if (_type == awst::WType::accountType())
			return std::vector<uint8_t>(32, 0);
		return std::nullopt;
	}
	default:
		return std::nullopt;
	}
}

int computeEncodedElementSize(awst::WType const* _type)
{
	if (!_type)
		return 0;

	switch (_type->kind())
	{
	case awst::WTypeKind::ARC4UIntN:
		return static_cast<awst::ARC4UIntN const*>(_type)->n() / 8;
	case awst::WTypeKind::ARC4UFixedNxM:
		return static_cast<awst::ARC4UFixedNxM const*>(_type)->n() / 8;
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
		if (arr->elementType() == awst::WType::arc4BoolType())
			return (arr->arraySize() + 7) / 8;
		int elemSize = computeEncodedElementSize(arr->elementType());
		if (elemSize == 0)
			return 0;
		return arr->arraySize() * elemSize;
	}
	case awst::WTypeKind::ReferenceArray:
	{
		auto const* arr = static_cast<awst::ReferenceArray const*>(_type);
		if (!arr->arraySize())
			return 0;
		int elemSize = computeEncodedElementSize(arr->elementType());
		if (elemSize == 0)
			return 0;
		return *arr->arraySize() * elemSize;
	}
	case awst::WTypeKind::ARC4DynamicArray:
		return 0;
	case awst::WTypeKind::Bytes:
	{
		auto const* bytesType = static_cast<awst::BytesWType const*>(_type);
		if (bytesType->length())
			return *bytesType->length();
		return 0;
	}
	case awst::WTypeKind::Basic:
	{
		if (_type == awst::WType::biguintType())
			return 32;
		if (_type == awst::WType::uint64Type())
			return 8;
		if (_type == awst::WType::boolType())
			return 8;
		if (_type == awst::WType::accountType())
			return 32;
		return 0;
	}
	default:
		return 0;
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
	// Flip here once the blob model handles new-init / push / etc. for small arrays. See PLAN.md.
	return computeEncodedElementSize(_type) > 4096;
}

} // namespace puyasol::builder
