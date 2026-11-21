#pragma once

#include <string>
#include <string_view>

namespace hb {

// Standard base64 (RFC 4648, with padding). decode throws std::invalid_argument on bad input;
// whitespace is ignored.
std::string base64Encode(std::string_view bytes);
std::string base64Decode(std::string_view text);

}  // namespace hb
