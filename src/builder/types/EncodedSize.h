#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace puyasol::builder
{

/// A target-capacity diagnostic, caught at the AWST compilation boundary.
struct SizeError: std::runtime_error
{
	using std::runtime_error::runtime_error;
};

/// Check BEFORE narrowing either host integers or solc's arbitrary-width facts.
template<class Target, class Source>
Target checkedSize(Source const& _value, std::string_view _description)
{
	static_assert(std::is_integral_v<Target>);
	if (_value < 0 || _value > std::numeric_limits<Target>::max())
		throw SizeError(std::string(_description) + " exceeds the compiler's addressable range");
	return static_cast<Target>(_value);
}

/// Byte-aligned element size, deliberately distinct from EVM storage-slot size.
/// A standalone ARC4 bool has no byte stride within a packed run; bool arrays
/// and aggregate runs compute their complete packed size at the container level.
struct EncodedSize
{
	enum class Kind { Fixed, Dynamic, Packed, Unsupported, Overflow };
	Kind kind;
	uint64_t bytes = 0;

	static EncodedSize fixed(uint64_t _bytes) { return {Kind::Fixed, _bytes}; }

	/// Dynamic/packed elements need a non-stride path. Invalid/overflowed sizes
	/// must never take that fallback, and every requested narrowing is checked.
	template<class Target = uint64_t>
	std::optional<Target> fixedBytes() const
	{
		if (kind == Kind::Unsupported)
			throw SizeError("unsupported encoded element size");
		if (kind == Kind::Overflow)
			throw SizeError("encoded element size exceeds the compiler's addressable range");
		if (kind != Kind::Fixed)
			return std::nullopt;
		return checkedSize<Target>(bytes, "encoded element size");
	}

	EncodedSize times(uint64_t _count) const
	{
		if (kind != Kind::Fixed)
			return *this;
		if (_count && bytes > std::numeric_limits<uint64_t>::max() / _count)
			return {Kind::Overflow};
		return fixed(bytes * _count);
	}

	EncodedSize plus(EncodedSize _other) const
	{
		if (kind == Kind::Unsupported || kind == Kind::Overflow) return *this;
		if (_other.kind == Kind::Unsupported || _other.kind == Kind::Overflow) return _other;
		if (kind != Kind::Fixed) return *this;
		if (_other.kind != Kind::Fixed) return _other;
		if (bytes > std::numeric_limits<uint64_t>::max() - _other.bytes)
			return {Kind::Overflow};
		return fixed(bytes + _other.bytes);
	}
};

} // namespace puyasol::builder
