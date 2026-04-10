#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bridge_support {

using ReplacementList = std::vector<std::pair<std::string, std::string>>;

struct AvatarProviderOptions {
    std::string provider;
    std::string provider_url_template;
    bool provider_prefer_xuid = true;
    std::string provider_render_type = "helm";
    std::string provider_bedrock_username_prefix = ".";
    int size = 64;
};

struct PlayerIdentity {
    std::string username;
    std::string xuid;
    std::string uuid;
    std::string skin_id;
};

// Discord API hard limit for message content (code units).
inline constexpr std::size_t kDiscordMaxMessageLength = 2000;

std::string applyTemplate(std::string value, const ReplacementList &replacements);
std::string replaceAll(std::string value, const std::string &needle, const std::string &replacement);
// Truncates value to at most max_bytes bytes, backing up to the nearest valid
// UTF-8 character boundary so multi-byte sequences are never split.
std::string truncateUtf8Bytes(const std::string &value, std::size_t max_bytes);
// Strips Minecraft § formatting codes (§ followed by one character) from value.
// § is U+00A7 (UTF-8: 0xC2 0xA7); this handles that encoding correctly.
std::string stripMinecraftFormatting(std::string value);
bool isAllowedRemoteAddress(const std::string &host, const std::vector<std::string> &allowed_patterns);
std::optional<std::uint32_t> parseIpv4Address(const std::string &value);
std::string normalizeAvatarProvider(std::string value);
std::string urlEncode(const std::string &value);
std::optional<std::string> buildProviderAvatarUrl(const AvatarProviderOptions &options, const PlayerIdentity &identity);

}  // namespace bridge_support
