#pragma once

#include <charconv>
#include <optional>
#include <string>
#include <string_view>

namespace Launcher::ReleaseVersion {

struct Parts {
    std::string_view prefix, suffix;
    int major{}, minor{}, patch{};
};

inline bool ReadNumber(std::string_view text, int& number) {
    if (text.empty() || text.front() < '0' || text.front() > '9' ||
        (text.size() > 1 && text.front() == '0'))
        return false;
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, number);
    return result.ec == std::errc{} && result.ptr == end;
}

inline std::optional<Parts> Parse(std::string_view version) {
    const auto suffix_at = version.find_first_of("-+");
    const auto base = version.substr(0, suffix_at);
    const auto first = base.find('.');
    const auto last = base.rfind('.');
    if (first == last)
        return std::nullopt;
    Parts parts{base.substr(0, last + 1), version.substr(base.size())};
    if (!ReadNumber(base.substr(0, first), parts.major) ||
        !ReadNumber(base.substr(first + 1, last - first - 1), parts.minor) ||
        !ReadNumber(base.substr(last + 1), parts.patch))
        return std::nullopt;
    return parts;
}

// Keep released package identities through 0.9.2. After that, reserve patch 100–199 for
// the package feed: 100 + ten times the public patch + the optional hotfix digit.
// Velopack can then apply its ordinary upgrade rules to 0.9.21, 0.9.22 and 0.9.3.
inline std::optional<std::string> Package(std::string_view version) {
    const auto parts = Parse(version);
    if (!parts || parts->patch > 99)
        return std::nullopt;
    if (parts->major == 0 && (parts->minor < 9 || (parts->minor == 9 && parts->patch <= 2)))
        return std::string(version);

    const int order = parts->patch < 10 ? parts->patch * 10 : parts->patch;
    if ((parts->patch >= 10 && order % 10 == 0) ||
        (parts->major == 0 && parts->minor == 9 && order < 21))
        return std::nullopt;
    return std::string(parts->prefix) + std::to_string(100 + order) + std::string(parts->suffix);
}

inline std::string Display(std::string_view package) {
    const auto parts = Parse(package);
    if (!parts || parts->patch < 100 || parts->patch > 199)
        return std::string(package);
    const int order = parts->patch - 100;
    return std::string(parts->prefix) + std::to_string(order % 10 == 0 ? order / 10 : order) +
           std::string(parts->suffix);
}

} // namespace Launcher::ReleaseVersion
