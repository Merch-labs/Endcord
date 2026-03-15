#include "bedrock_discord_bridge.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

ENDSTONE_PLUGIN(/*name=*/"bedrock_discord_bridge",
                /*version=*/"0.1.0",
                /*main_class=*/BedrockDiscordBridgePlugin)
{
    prefix = "BedrockDiscordBridge";
    description = "Endstone C++ plugin skeleton for Bedrock chat forwarding to Discord.";
    authors = {"Bedrock Discord Bridge contributors"};
}

void BedrockDiscordBridgePlugin::onLoad()
{
    getLogger().info("Loading plugin.");
}

void BedrockDiscordBridgePlugin::onEnable()
{
    ensureDataFolder();
    loadDiscordSettings();
    registerEvent(&BedrockDiscordBridgePlugin::onPlayerChat, *this);

    getLogger().info("Plugin enabled. Chat forwarding stub is active.");
}

void BedrockDiscordBridgePlugin::onDisable()
{
    getLogger().info("Plugin disabled.");
}

void BedrockDiscordBridgePlugin::onPlayerChat(endstone::PlayerChatEvent &event)
{
    if (event.isCancelled()) {
        return;
    }

    forwardChatToDiscord(event.getPlayer(), event.getMessage());
}

void BedrockDiscordBridgePlugin::ensureDataFolder() const
{
    std::error_code ec;
    fs::create_directories(getDataFolder(), ec);
    if (ec) {
        getLogger().warning("Could not create plugin data folder '{}': {}", getDataFolder().string(), ec.message());
    }
}

void BedrockDiscordBridgePlugin::loadDiscordSettings()
{
    settings_ = {};

    const auto config_path = getDataFolder() / "discord.env";
    std::ifstream input(config_path);
    if (!input.is_open()) {
        getLogger().warning(
            "No Discord config found at '{}'. Copy config/discord.env.example there and set DISCORD_WEBHOOK_URL.",
            config_path.string());
        return;
    }

    std::string line;
    while (std::getline(input, line)) {
        const auto trimmed = trim(line);
        if (trimmed.empty() || trimmed.starts_with('#')) {
            continue;
        }

        const auto separator = trimmed.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const auto key = trim(trimmed.substr(0, separator));
        const auto value = trim(trimmed.substr(separator + 1));
        if (key == "DISCORD_WEBHOOK_URL") {
            settings_.webhook_url = value;
        }
    }

    if (settings_.webhook_url.empty()) {
        getLogger().warning("DISCORD_WEBHOOK_URL is empty in '{}'.", config_path.string());
    }
    else {
        getLogger().info("Loaded Discord webhook configuration.");
    }
}

void BedrockDiscordBridgePlugin::forwardChatToDiscord(const endstone::Player &player, const std::string &message)
{
    const auto avatar_url = getCachedOrPlannedAvatarUrl(player);

    std::ostringstream payload;
    payload << "{"
            << "\"username\":\"" << escapeJson(player.getName()) << "\","
            << "\"content\":\"" << escapeJson(message) << "\"";
    if (avatar_url.has_value()) {
        payload << ",\"avatar_url\":\"" << escapeJson(*avatar_url) << "\"";
    }
    payload << "}";

    if (settings_.webhook_url.empty()) {
        getLogger().debug("Skipping Discord webhook POST because DISCORD_WEBHOOK_URL is not configured.");
        return;
    }

    getLogger().info("Prepared Discord webhook payload for '{}'.", player.getName());
    getLogger().debug("Webhook payload preview: {}", payload.str());

    // TODO: POST payload.str() to settings_.webhook_url with an HTTP client.
    // TODO: Honor Discord rate-limit headers such as Retry-After and X-RateLimit-Bucket.
}

std::optional<std::string> BedrockDiscordBridgePlugin::getCachedOrPlannedAvatarUrl(const endstone::Player &player)
{
    const auto skin = player.getSkin();
    const auto &image = skin.getImage();

    if (auto it = avatar_url_cache_.find(skin.getId()); it != avatar_url_cache_.end()) {
        return it->second;
    }

    getLogger().debug("Observed skin '{}' for '{}' with image size {}x{}.", skin.getId(), player.getName(),
                      image.getWidth(), image.getHeight());

    // TODO: Extract the 8x8 face layer plus hat/overlay from the Bedrock skin RGBA data.
    // TODO: Render a 64x64 or 128x128 head icon, upload it to a stable CDN/object store, and cache by skin id.
    return std::nullopt;
}

std::string BedrockDiscordBridgePlugin::trim(const std::string &value)
{
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }

    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string BedrockDiscordBridgePlugin::escapeJson(const std::string &value)
{
    std::string escaped;
    escaped.reserve(value.size());

    for (const char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += ch;
            break;
        }
    }

    return escaped;
}
