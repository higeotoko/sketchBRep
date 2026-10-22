#pragma once
// Small byte codecs for embedded assets (images): content hashes and text
// encoding inside the JSON project file.

#include <string>
#include <string_view>

namespace sbr::codec {

// SHA-256 of the bytes, lowercase hex (64 characters).
std::string sha256Hex(std::string_view bytes);
std::string base64Encode(std::string_view bytes);
// Empty on malformed input.
std::string base64Decode(std::string_view text);

}  // namespace sbr::codec
