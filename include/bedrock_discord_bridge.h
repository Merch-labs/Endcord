#pragma once

#include <endstone/endstone.hpp>

#include <optional>
#include <string>
#include <unordered_map>

class BedrockDiscordBridgePlugin : public endstone::Plugin {
public:
    void onLoad() override;
    void onEnable() override;
    void onDisable() override;

    void onPlayerChat(endstone::PlayerChatEvent &event);

private:
    struct DiscordSettings {
        std::string webhook_url;
    };

    void ensureDataFolder() const;
    void loadDiscordSettings();
    void forwardChatToDiscord(const endstone::Player &player, const std::string &message);
    std::optional<std::string> getCachedOrPlannedAvatarUrl(const endstone::Player &player);

    static std::string trim(const std::string &value);
    static std::string escapeJson(const std::string &value);

    DiscordSettings settings_;
    std::unordered_map<std::string, std::string> avatar_url_cache_;
};
