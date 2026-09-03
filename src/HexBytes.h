#pragma once
/// @file HexBytes.h
/// One strict hex decoder for every CLI address/template input.
///
/// The previous per-site decoders were neither strict nor exception-safe:
/// `std::stoul(pair, nullptr, 16)` silently read "0g" as 0 (a partially parsed
/// byte still produces an address, so routed funds land somewhere the operator
/// never named) and threw on "gg", which nothing caught — the process aborted
/// with 134. A separate nibble decoder indexed 40 characters without checking
/// the length at all, reading out of bounds on short input.
///
/// hexToBytes accepts ONE optional "0x"/"0X" prefix and nothing else: no sign,
/// no whitespace, no partial parse. It never throws.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace puyasol
{

/// Decoded bytes, or nullopt when the input is not exactly an even-length run
/// of hex digits — or, when _expectedBytes is nonzero, when it does not decode
/// to precisely that many bytes. Empty input is rejected: a caller asking for
/// bytes never means "none".
inline std::optional<std::vector<uint8_t>> hexToBytes(
	std::string_view _input, size_t _expectedBytes = 0)
{
	auto nibble = [](char _c) -> int {
		if (_c >= '0' && _c <= '9') return _c - '0';
		if (_c >= 'a' && _c <= 'f') return _c - 'a' + 10;
		if (_c >= 'A' && _c <= 'F') return _c - 'A' + 10;
		return -1;
	};
	std::string_view h = _input;
	if (h.size() >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X'))
		h.remove_prefix(2);
	if (h.empty() || h.size() % 2 != 0)
		return std::nullopt;
	if (_expectedBytes != 0 && h.size() != _expectedBytes * 2)
		return std::nullopt;
	std::vector<uint8_t> out;
	out.reserve(h.size() / 2);
	for (size_t i = 0; i < h.size(); i += 2)
	{
		int const hi = nibble(h[i]);
		int const lo = nibble(h[i + 1]);
		if (hi < 0 || lo < 0)
			return std::nullopt;
		out.push_back(static_cast<uint8_t>((hi << 4) | lo));
	}
	return out;
}

} // namespace puyasol
