#include "builder/sol-types/Arc4Defaults.h"

namespace puyasol::builder
{

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
	// Delegate to puya's ConvertArray lowering — it already knows how to add
	// the uint16 length header when going from ARC4StaticArray to
	// ARC4DynamicArray (and how to strip it in the reverse direction),
	// so we don't need to synthesise concat+reinterpret by hand.
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
		auto elemDefault = arc4DefaultEncoding(elemT);
		if (!elemDefault)
			return std::nullopt;

		std::vector<uint8_t> result;
		if (arc4IsDynamic(elemT))
		{
			int64_t headSize = N * 2;
			int64_t tailSize = static_cast<int64_t>(elemDefault->size());
			// ARC4 dynamic-element offsets are uint16 (2 bytes). If the final
			// element's offset (headSize + (N-1)*tailSize) would exceed 0xFFFF it
			// can't be encoded — bail out so the caller falls back, rather than
			// emit a silently wrapped/corrupt offset header. Checking headSize
			// first bounds N, keeping the product below overflow-safe.
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
		// Pre-compute each field's default and total head size so dynamic
		// field offsets can be embedded.
		struct FieldEnc { bool dynamic; std::vector<uint8_t> bytes; };
		std::vector<FieldEnc> encs;
		encs.reserve(st->fields().size());
		int64_t headSize = 0;
		for (auto const& [name, ft]: st->fields())
		{
			auto fd = arc4DefaultEncoding(ft);
			if (!fd)
				return std::nullopt;
			bool dyn = arc4IsDynamic(ft);
			headSize += dyn ? 2 : static_cast<int64_t>(fd->size());
			encs.push_back({dyn, std::move(*fd)});
		}

		std::vector<uint8_t> head;
		std::vector<uint8_t> tail;
		head.reserve(static_cast<size_t>(headSize));
		int64_t tailOff = headSize;
		for (auto const& fe: encs)
		{
			if (fe.dynamic)
			{
				head.push_back(static_cast<uint8_t>((tailOff >> 8) & 0xFF));
				head.push_back(static_cast<uint8_t>(tailOff & 0xFF));
				tail.insert(tail.end(), fe.bytes.begin(), fe.bytes.end());
				tailOff += static_cast<int64_t>(fe.bytes.size());
			}
			else
			{
				head.insert(head.end(), fe.bytes.begin(), fe.bytes.end());
			}
		}
		head.insert(head.end(), tail.begin(), tail.end());
		return head;
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
		// ARC4 packs consecutive `arc4.bool` fields into shared bytes
		// (8 bools per byte). All other fields are byte-aligned. Walk
		// the field list, accumulating bool runs and flushing them as
		// `ceil(run_length / 8)` bytes whenever a non-bool field
		// interrupts the run (or at the end). If any non-bool field
		// is dynamic (size 0), the whole struct is dynamic.
		auto const* structType = static_cast<awst::ARC4Struct const*>(_type);
		int total = 0;
		int boolRun = 0;
		auto flushBoolRun = [&]() {
			if (boolRun > 0)
			{
				total += (boolRun + 7) / 8;
				boolRun = 0;
			}
		};
		for (auto const& [name, fieldType]: structType->fields())
		{
			if (fieldType == awst::WType::arc4BoolType())
			{
				boolRun++;
				continue;
			}
			flushBoolRun();
			int fieldSize = computeEncodedElementSize(fieldType);
			if (fieldSize == 0)
				return 0;
			total += fieldSize;
		}
		flushBoolRun();
		return total;
	}
	case awst::WTypeKind::ARC4StaticArray:
	{
		auto const* arr = static_cast<awst::ARC4StaticArray const*>(_type);
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

} // namespace puyasol::builder
