#include "endcord_bot_config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>

namespace endcord {
namespace {

using json = nlohmann::json;

std::vector<std::uint64_t> loadSnowflakeList(const json &node)
{
    std::vector<std::uint64_t> values;
    if (!node.is_array()) {
        return values;
    }

    values.reserve(node.size());
    for (const auto &item : node) {
        if (item.is_number_unsigned()) {
            values.push_back(item.get<std::uint64_t>());
        }
        else if (item.is_number_integer()) {
            values.push_back(static_cast<std::uint64_t>(std::max<std::int64_t>(item.get<std::int64_t>(), 0)));
        }
    }
    return values;
}

SlashCommandRuleConfig loadCommandRule(const json &slash_cfg, const std::string &name,
                                       const std::vector<std::uint64_t> &fallback_role_ids, bool default_admin_only)
{
    const auto &node = slash_cfg.contains(name) && slash_cfg[name].is_object() ? slash_cfg[name] : json::object();
    SlashCommandRuleConfig rule;
    if (node.is_object()) {
        rule.enabled = node.value("enabled", true);
        rule.admin_only = node.value("admin_only", default_admin_only);
        rule.role_ids = node.contains("role_ids") ? loadSnowflakeList(node["role_ids"]) : fallback_role_ids;
    }
    else {
        rule.enabled = true;
        rule.admin_only = default_admin_only;
        rule.role_ids = fallback_role_ids;
    }
    return rule;
}

}  // namespace

void writeDefaultBotConfigIfMissing(const std::filesystem::path &path)
{
    if (std::filesystem::exists(path)) {
        return;
    }

    std::filesystem::create_directories(path.parent_path());
    const json root = {
        {"discord",
         {{"token", "replace-me"},
          {"guild_id", 0},
          {"relay_channel_ids", json::array({0})},
          {"outbound_channel_id", 0},
          {"command_role_ids", json::array()},
          {"status_role_ids", json::array()},
          {"relay_to_game_enabled", true},
          {"sync_commands_globally", false},
          {"auto_create_webhook", true},
          {"webhook_name", "Endcord"}}},
        {"plugin_bridge",
         {{"base_url", "http://127.0.0.1:8089/endcord/api"},
          {"shared_secret", ""},
          {"request_timeout_seconds", 10},
          {"configure_webhook_on_startup", true},
          {"request_max_retries", 3},
          {"request_retry_base_seconds", 1.5},
          {"request_retry_max_seconds", 15.0}}},
        {"relay",
         {{"include_attachment_urls", true},
          {"include_jump_url", false},
          {"ignore_bot_messages", true},
          {"ignore_webhook_messages", true},
          {"message_template", "{content}{attachments}{jump_url}"},
          {"attachment_template", "\n[attachment] {url}"},
          {"jump_url_template", "\n[jump] {url}"},
          {"join_separator", "\n"},
          {"max_message_length", 1800}}},
        {"slash_commands",
         {{"enabled", true},
          {"ephemeral_responses", true},
          {"status", {{"enabled", true}, {"admin_only", false}, {"role_ids", json::array()}}},
          {"players", {{"enabled", true}, {"admin_only", false}, {"role_ids", json::array()}}},
          {"ping", {{"enabled", true}, {"admin_only", false}, {"role_ids", json::array()}}},
          {"command", {{"enabled", true}, {"admin_only", true}, {"role_ids", json::array()}}},
          {"configreload", {{"enabled", true}, {"admin_only", true}, {"role_ids", json::array()}}}}},
        {"presence",
         {{"enabled", true},
          {"status", "online"},
          {"activity_type", "watching"},
          {"activity_text", "{server_name} | {online_players} online"},
          {"fallback_text", "Endcord online"},
          {"streaming_url", ""},
          {"update_interval_seconds", 120}}},
        {"logging",
         {{"level", "INFO"},
          {"log_ignored_messages", false},
          {"log_relay_successes", false},
          {"log_presence_updates", false}}},
        {"system_messages",
         {{"enabled", false},
          {"channel_id", 0},
          {"poll_interval_seconds", 2},
          {"failure_backoff_seconds", 5},
          {"max_backoff_seconds", 60},
          {"message_template", "{content}"},
          {"max_messages_per_poll", 20}}}
    };

    std::ofstream output(path);
    output << root.dump(2) << '\n';
}

BotConfig loadBotConfig(const std::filesystem::path &path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("Could not open bot config: " + path.string());
    }

    const auto root = json::parse(input);
    const auto &discord_cfg = root.contains("discord") && root["discord"].is_object() ? root["discord"] : json::object();
    const auto &relay_cfg = root.contains("relay") && root["relay"].is_object() ? root["relay"] : json::object();
    const auto &plugin_bridge_cfg =
        root.contains("plugin_bridge") && root["plugin_bridge"].is_object() ? root["plugin_bridge"] : json::object();
    const auto &slash_cfg =
        root.contains("slash_commands") && root["slash_commands"].is_object() ? root["slash_commands"] : json::object();
    const auto &presence_cfg =
        root.contains("presence") && root["presence"].is_object() ? root["presence"] : json::object();
    const auto &logging_cfg =
        root.contains("logging") && root["logging"].is_object() ? root["logging"] : json::object();
    const auto &system_cfg =
        root.contains("system_messages") && root["system_messages"].is_object() ? root["system_messages"] : json::object();

    BotConfig config;
    config.discord.token = discord_cfg.value("token", "");
    config.discord.guild_id = discord_cfg.value("guild_id", 0ULL);
    config.discord.relay_channel_ids =
        discord_cfg.contains("relay_channel_ids") ? loadSnowflakeList(discord_cfg["relay_channel_ids"]) : std::vector<std::uint64_t>{};
    config.discord.outbound_channel_id = discord_cfg.value("outbound_channel_id", 0ULL);
    config.discord.command_role_ids =
        discord_cfg.contains("command_role_ids") ? loadSnowflakeList(discord_cfg["command_role_ids"]) : std::vector<std::uint64_t>{};
    config.discord.status_role_ids =
        discord_cfg.contains("status_role_ids") ? loadSnowflakeList(discord_cfg["status_role_ids"]) : std::vector<std::uint64_t>{};
    config.discord.relay_to_game_enabled = discord_cfg.value("relay_to_game_enabled", true);
    config.discord.sync_commands_globally = discord_cfg.value("sync_commands_globally", false);
    config.discord.auto_create_webhook = discord_cfg.value("auto_create_webhook", true);
    config.discord.webhook_name = discord_cfg.value("webhook_name", std::string("Endcord"));

    config.plugin_bridge.base_url =
        plugin_bridge_cfg.value("base_url", std::string("http://127.0.0.1:8089/endcord/api"));
    config.plugin_bridge.shared_secret = plugin_bridge_cfg.value("shared_secret", std::string());
    config.plugin_bridge.request_timeout_seconds = std::max(plugin_bridge_cfg.value("request_timeout_seconds", 10), 1);
    config.plugin_bridge.configure_webhook_on_startup = plugin_bridge_cfg.value("configure_webhook_on_startup", true);
    config.plugin_bridge.request_max_retries = std::max(plugin_bridge_cfg.value("request_max_retries", 3), 0);
    config.plugin_bridge.request_retry_base_seconds =
        std::max(plugin_bridge_cfg.value("request_retry_base_seconds", 1.5), 0.1);
    config.plugin_bridge.request_retry_max_seconds =
        std::max(plugin_bridge_cfg.value("request_retry_max_seconds", 15.0), 0.1);

    config.relay.include_attachment_urls = relay_cfg.value("include_attachment_urls", true);
    config.relay.include_jump_url = relay_cfg.value("include_jump_url", false);
    config.relay.ignore_bot_messages = relay_cfg.value("ignore_bot_messages", true);
    config.relay.ignore_webhook_messages = relay_cfg.value("ignore_webhook_messages", true);
    config.relay.message_template = relay_cfg.value("message_template", std::string("{content}{attachments}{jump_url}"));
    config.relay.attachment_template = relay_cfg.value("attachment_template", std::string("\n[attachment] {url}"));
    config.relay.jump_url_template = relay_cfg.value("jump_url_template", std::string("\n[jump] {url}"));
    config.relay.join_separator = relay_cfg.value("join_separator", std::string("\n"));
    config.relay.max_message_length = std::max(relay_cfg.value("max_message_length", 1800), 32);

    config.slash_commands.enabled = slash_cfg.value("enabled", true);
    config.slash_commands.ephemeral_responses = slash_cfg.value("ephemeral_responses", true);
    config.slash_commands.status = loadCommandRule(slash_cfg, "status", config.discord.status_role_ids, false);
    config.slash_commands.players = loadCommandRule(slash_cfg, "players", config.discord.status_role_ids, false);
    config.slash_commands.ping = loadCommandRule(slash_cfg, "ping", config.discord.status_role_ids, false);
    config.slash_commands.command = loadCommandRule(slash_cfg, "command", config.discord.command_role_ids, true);
    config.slash_commands.configreload =
        loadCommandRule(slash_cfg, "configreload", config.discord.command_role_ids, true);

    config.presence.enabled = presence_cfg.value("enabled", true);
    config.presence.status = presence_cfg.value("status", std::string("online"));
    config.presence.activity_type = presence_cfg.value("activity_type", std::string("watching"));
    config.presence.activity_text =
        presence_cfg.value("activity_text", std::string("{server_name} | {online_players} online"));
    config.presence.fallback_text = presence_cfg.value("fallback_text", std::string("Endcord online"));
    config.presence.streaming_url = presence_cfg.value("streaming_url", std::string());
    config.presence.update_interval_seconds = std::max(presence_cfg.value("update_interval_seconds", 120), 0);

    config.logging.level = logging_cfg.value("level", std::string("INFO"));
    config.logging.log_ignored_messages = logging_cfg.value("log_ignored_messages", false);
    config.logging.log_relay_successes = logging_cfg.value("log_relay_successes", false);
    config.logging.log_presence_updates = logging_cfg.value("log_presence_updates", false);

    config.system_messages.enabled = system_cfg.value("enabled", false);
    config.system_messages.channel_id = system_cfg.value("channel_id", 0ULL);
    config.system_messages.poll_interval_seconds = std::max(system_cfg.value("poll_interval_seconds", 2), 1);
    config.system_messages.failure_backoff_seconds = std::max(system_cfg.value("failure_backoff_seconds", 5), 1);
    config.system_messages.max_backoff_seconds = std::max(system_cfg.value("max_backoff_seconds", 60), 1);
    config.system_messages.message_template = system_cfg.value("message_template", std::string("{content}"));
    config.system_messages.max_messages_per_poll = std::max(system_cfg.value("max_messages_per_poll", 20), 1);

    return config;
}

}  // namespace endcord
