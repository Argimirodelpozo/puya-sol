/// Negative tests for the strict CLI hex decoder (audit H-06).
///
/// The decoder it replaces partially parsed "0g" as the byte 0x00 — a template
/// that still derives an ADDRESS, just not the one the operator named — and
/// aborted the process on "gg" because nothing caught std::invalid_argument.
/// A second copy indexed 40 characters with no length check, reading out of
/// bounds on short input. Every case below is one of those two failure modes.
#include "HexBytes.h"

#include <iostream>
#include <string>

namespace
{

bool require(bool _condition, char const* _message)
{
	if (_condition)
		return true;
	std::cerr << "FAIL: " << _message << '\n';
	return false;
}

bool rejects(std::string_view _in, size_t _expected, char const* _message)
{
	return require(!puyasol::hexToBytes(_in, _expected).has_value(), _message);
}

} // namespace

int main()
{
	using puyasol::hexToBytes;
	bool ok = true;

	// ── accepts ──────────────────────────────────────────────────────────
	auto plain = hexToBytes("00ff10");
	ok &= require(plain && *plain == std::vector<uint8_t>{0x00, 0xff, 0x10},
		"plain lowercase hex must decode");
	auto prefixed = hexToBytes("0x00ff10");
	ok &= require(prefixed && *prefixed == *plain,
		"a 0x prefix must decode identically");
	auto upperPrefix = hexToBytes("0X00FF10");
	ok &= require(upperPrefix && *upperPrefix == *plain,
		"a 0X prefix and uppercase digits must decode identically");
	auto sized = hexToBytes("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", 20);
	ok &= require(sized && sized->size() == 20,
		"the 20-byte placeholder must decode");
	// Boundary: one byte is the smallest meaningful input.
	auto one = hexToBytes("0x00");
	ok &= require(one && one->size() == 1, "a single byte must decode");
	// Huge input must decode rather than be rejected for size alone.
	auto huge = hexToBytes(std::string(200000, 'a'));
	ok &= require(huge && huge->size() == 100000,
		"a large well-formed blob must still decode");

	// ── rejects: the partial-parse family ────────────────────────────────
	ok &= rejects("0g", 0, "'0g' must be rejected, not read as 0x00");
	ok &= rejects("gg", 0, "'gg' must be rejected, not abort the process");
	ok &= rejects("g0", 0, "an invalid FIRST nibble must be rejected");
	ok &= rejects("0f0g", 0, "an invalid nibble anywhere must be rejected");
	ok &= rejects(" 00", 0, "leading whitespace must be rejected");
	ok &= rejects("00 ", 0, "trailing whitespace must be rejected");
	ok &= rejects("-1", 0, "a sign must be rejected, not wrap to 0xff");
	ok &= rejects("+0", 0, "a sign must be rejected");
	ok &= rejects("0x0x00", 0, "only ONE prefix may be stripped");

	// ── rejects: shape ──────────────────────────────────────────────────
	ok &= rejects("", 0, "empty input must be rejected");
	ok &= rejects("0x", 0, "a bare prefix must be rejected");
	ok &= rejects("abc", 0, "odd length must be rejected");
	ok &= rejects("0xabc", 0, "odd length behind a prefix must be rejected");

	// ── rejects: size constraint ────────────────────────────────────────
	ok &= rejects("eeee", 20, "a short placeholder must be rejected");
	ok &= rejects(std::string(42, 'e'), 20,
		"an over-long placeholder must be rejected");
	ok &= require(hexToBytes(std::string(40, 'e'), 20).has_value(),
		"exactly 20 bytes must be accepted");
	// A size request is exact, so 19 and 21 bytes both fail.
	ok &= rejects(std::string(38, 'e'), 20, "19 bytes must be rejected");

	std::cout << (ok ? "HexBytes: all cases pass\n" : "HexBytes: FAILURES\n");
	return ok ? 0 : 1;
}
