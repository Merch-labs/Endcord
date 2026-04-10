#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

namespace endcord {

struct SlashCommandRuleConfig {
    bool enabled = true;
    bool admin_only = false;
    std::vector<std::uint64_t> role_ids{};
};

struct SlashCommandConfig {
    bool enabled = true;
    bool ephemeral_responses = true;
    SlashCommandRuleConfig status{};
    SlashCommandRuleConfig players{};
    SlashCommandRuleConfig ping{};
    SlashCommandRuleConfig command{.enabled = true, .admin_only = true};
    SlashCommandRuleConfig configreload{.enabled = true, .admin_only = true};
};

struct DiscordRuntimeConfig {
    std::string token;
    std::uint64_t guild_id = 0;
    std::vector<std::uint64_t> relay_channel_ids{};
    std::uint64_t outbound_channel_id = 0;
    std::vector<std::uint64_t> command_role_ids{};
    std::vector<std::uint64_t> status_role_ids{};
    bool relay_to_game_enabled = true;
    bool sync_commands_globally = false;
    bool auto_create_webhook = true;
    std::string webhook_name = "Endcord";
};

struct RelayRuntimeConfig {
    bool include_attachment_urls = true;
    bool include_jump_url = false;
    bool ignore_bot_messages = true;
    bool ignore_webhook_messages = true;
    std::string message_template = "{content}{attachments}{jump_url}";
    std::string attachment_template = "\n[attachment] {url}";
    std::string jump_url_template = "\n[jump] {url}";
    std::string join_separator = "\n";
    int max_message_length = 1800;
};

struct PresenceRuntimeConfig {
    bool enabled = true;
    std::string status = "online";
    std::string activity_type = "watching";
    std::string activity_text = "{server_name} | {online_players} online";
    std::string fallback_text = "Endcord online";
    std::string streaming_url;
    int update_interval_seconds = 120;
};

struct LoggingRuntimeConfig {
    std::string level = "WARNING";
    bool log_ignored_messages = false;
    bool log_relay_successes = false;
    bool log_presence_updates = false;
};

struct SystemMessageRuntimeConfig {
    bool enabled = true;
    std::uint64_t channel_id = 0;
    int poll_interval_seconds = 2;
    int failure_backoff_seconds = 5;
    int max_backoff_seconds = 60;
    std::string message_template = "{content}";
    int max_messages_per_poll = 20;
};

struct BotConfig {
    DiscordRuntimeConfig discord{};
    RelayRuntimeConfig relay{};
    SlashCommandConfig slash_commands{};
    PresenceRuntimeConfig presence{};
    LoggingRuntimeConfig logging{};
    SystemMessageRuntimeConfig system_messages{};
};

nlohmann::json buildDefaultBotConfigJson();
BotConfig loadBotConfig(const nlohmann::json &root);
BotConfig loadBotConfig(const std::filesystem::path &path);

}  // namespace endcord
