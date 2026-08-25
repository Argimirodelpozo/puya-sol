#include "json/Base85.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace puyasol::json
{

namespace
{

// RFC1924 character set (85 printable ASCII characters)
constexpr char const* ALPHABET =
	"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{|}~";

int charIndex(char _c)
{
	char const* pos = std::find(ALPHABET, ALPHABET + 85, _c);
	if (pos == ALPHABET + 85)
		throw std::runtime_error("Invalid base85 character");
	return static_cast<int>(pos - ALPHABET);
}

} // namespace

std::string base85Encode(std::vector<uint8_t> const& _data)
{
	if (_data.empty())
		return "";

	std::string result;
	size_t i = 0;

	// Process 4-byte groups → 5 base-85 characters
	while (i + 4 <= _data.size())
	{
		uint32_t acc = (static_cast<uint32_t>(_data[i]) << 24)
			| (static_cast<uint32_t>(_data[i + 1]) << 16)
			| (static_cast<uint32_t>(_data[i + 2]) << 8)
			| static_cast<uint32_t>(_data[i + 3]);

		char chunk[5];
		for (int j = 4; j >= 0; --j)
		{
			chunk[j] = ALPHABET[acc % 85];
			acc /= 85;
		}
		result.append(chunk, 5);
		i += 4;
	}

	// Handle remaining bytes (1-3)
	size_t remaining = _data.size() - i;
	if (remaining > 0)
	{
		uint32_t acc = 0;
		for (size_t j = 0; j < remaining; ++j)
			acc = (acc << 8) | _data[i + j];

		// Pad to full group for encoding
		for (size_t j = remaining; j < 4; ++j)
			acc <<= 8;

		// Encode as 5 chars, then truncate to (remaining + 1)
		char chunk[5];
		for (int j = 4; j >= 0; --j)
		{
			chunk[j] = ALPHABET[acc % 85];
			acc /= 85;
		}
		result.append(chunk, remaining + 1);
	}

	return result;
}

} // namespace puyasol::json
