#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace puyasol::json
{

/// RFC1924 base-85 encoding for binary data in AWST JSON.
std::string base85Encode(std::vector<uint8_t> const& _data);
std::vector<uint8_t> base85Decode(std::string const& _encoded);

} // namespace puyasol::json
