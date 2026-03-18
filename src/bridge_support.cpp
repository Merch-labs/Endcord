#include "bridge_support.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace bridge_support {

std::string applyTemplate(std::string value, const ReplacementList &replacements)
{
    for (const auto &[needle, replacement] : replacements) {
        value = replaceAll(std::move(value), needle, replacement);
    }
    return value;
}

std::string replaceAll(std::string value, const std::string &needle, const std::string &replacement)
{
    if (needle.empty()) {
        return value;
    }

    std::size_t pos = 0;
    while ((pos = value.find(needle, pos)) != std::string::npos) {
        value.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
    return value;
}

bool isAllowedRemoteAddress(const std::string &host, const std::vector<std::string> &allowed_patterns)
{
    if (allowed_patterns.empty()) {
        return false;
    }

    const auto parsed_host = parseIpv4Address(host);
    for (const auto &pattern : allowed_patterns) {
        if (pattern == host) {
            return true;
        }

        const auto slash = pattern.find('/');
        if (slash == std::string::npos) {
            if (parsed_host.has_value() && parseIpv4Address(pattern) == parsed_host) {
                return true;
            }
            continue;
        }

        if (!parsed_host.has_value()) {
            continue;
        }

        const auto network = parseIpv4Address(pattern.substr(0, slash));
        if (!network.has_value()) {
            continue;
        }

        try {
            const auto bits = std::stoi(pattern.substr(slash + 1));
            if (bits < 0 || bits > 32) {
                continue;
            }
            const std::uint32_t mask = bits == 0 ? 0U : (0xFFFFFFFFU << (32 - bits));
            if ((*parsed_host & mask) == (*network & mask)) {
                return true;
            }
        }
        catch (...) {
            continue;
        }
    }

    return false;
}

std::optional<std::uint32_t> parseIpv4Address(const std::string &value)
{
    std::array<unsigned int, 4> octets{};
    char dot = '\0';
    std::istringstream stream(value);
    if (!(stream >> octets[0] >> dot) || dot != '.' || !(stream >> octets[1] >> dot) || dot != '.' ||
        !(stream >> octets[2] >> dot) || dot != '.' || !(stream >> octets[3]) || !stream.eof()) {
        return std::nullopt;
    }

    for (const auto octet : octets) {
        if (octet > 255U) {
            return std::nullopt;
        }
    }

    return (octets[0] << 24U) | (octets[1] << 16U) | (octets[2] << 8U) | octets[3];
}

std::string normalizeAvatarProvider(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return std::tolower(ch); });
    if (value == "mc-heads" || value == "mc_heads") {
        return "mcheads";
    }
    if (value.empty()) {
        return "tabavatars";
    }
    return value;
}

std::string urlEncode(const std::string &value)
{
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (const unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            escaped << static_cast<char>(ch);
        }
        else {
            escaped << '%' << std::uppercase << std::setw(2) << static_cast<int>(ch) << std::nouppercase;
        }
    }

    return escaped.str();
}

std::optional<std::string> buildProviderAvatarUrl(const AvatarProviderOptions &raw_options, const PlayerIdentity &identity)
{
    const AvatarProviderOptions options{
        .provider = normalizeAvatarProvider(raw_options.provider),
        .provider_url_template = raw_options.provider_url_template,
        .provider_prefer_xuid = raw_options.provider_prefer_xuid,
        .provider_render_type = raw_options.provider_render_type,
        .provider_bedrock_username_prefix = raw_options.provider_bedrock_username_prefix,
        .size = raw_options.size,
    };

    if (!options.provider_url_template.empty()) {
        auto url = applyTemplate(options.provider_url_template,
                                 {{"{player}", urlEncode(identity.username)},
                                  {"{username}", urlEncode(identity.username)},
                                  {"{xuid}", urlEncode(identity.xuid)},
                                  {"{uuid}", urlEncode(identity.uuid)},
                                  {"{uuid_nodashes}", urlEncode(replaceAll(identity.uuid, "-", ""))},
                                  {"{skin_id}", urlEncode(identity.skin_id)},
                                  {"{size}", std::to_string(options.size)},
                                  {"{render_type}", urlEncode(options.provider_render_type)}});
        return url.empty() ? std::nullopt : std::optional<std::string>(url);
    }

    if (options.provider == "tabavatars") {
        if (options.provider_prefer_xuid && !identity.xuid.empty()) {
            return "https://tabavatars.net/avatar/?xuid=" + urlEncode(identity.xuid) + "&size=" +
                   std::to_string(options.size) + "&type=" + urlEncode(options.provider_render_type);
        }
        return "https://tabavatars.net/avatar/?username=" + urlEncode(identity.username) + "&platform=bedrock&size=" +
               std::to_string(options.size) + "&type=" + urlEncode(options.provider_render_type);
    }

    if (options.provider == "mcheads") {
        std::string identifier = identity.username;
        if (options.provider_prefer_xuid && !identity.xuid.empty()) {
            identifier = identity.xuid;
        }
        else if (!options.provider_bedrock_username_prefix.empty()) {
            identifier = options.provider_bedrock_username_prefix + identity.username;
        }
        return "https://api.mcheads.org/head/" + urlEncode(identifier) + "/" + std::to_string(options.size);
    }

    return std::nullopt;
}

}  // namespace bridge_support
