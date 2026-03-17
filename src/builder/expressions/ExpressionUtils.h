#pragma once

/// @file ExpressionUtils.h
/// Shared utility functions used across expression builder files.

#include <string>

namespace puyasol::builder
{

/// Compute 2^bits - 1 as a decimal string (for type(uintN).max).
/// Uses arbitrary-precision decimal arithmetic to produce the exact value.
inline std::string computeMaxUint(unsigned _bits)
{
	// Start with "1", double `_bits` times, then subtract 1
	std::string result = "1";
	for (unsigned i = 0; i < _bits; ++i)
	{
		int carry = 0;
		for (int j = static_cast<int>(result.size()) - 1; j >= 0; --j)
		{
			int digit = (result[static_cast<size_t>(j)] - '0') * 2 + carry;
			result[static_cast<size_t>(j)] = static_cast<char>('0' + digit % 10);
			carry = digit / 10;
		}
		if (carry)
			result = std::string(1, static_cast<char>('0' + carry)) + result;
	}
	// Subtract 1
	for (int j = static_cast<int>(result.size()) - 1; j >= 0; --j)
	{
		if (result[static_cast<size_t>(j)] > '0')
		{
			result[static_cast<size_t>(j)]--;
			break;
		}
		result[static_cast<size_t>(j)] = '9';
	}
	// Remove leading zeros
	size_t start = result.find_first_not_of('0');
	return start == std::string::npos ? "0" : result.substr(start);
}

} // namespace puyasol::builder
