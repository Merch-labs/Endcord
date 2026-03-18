#include "bedrock_discord_bridge.h"

#include <httplib.h>
#include <lodepng.h>
#include <nlohmann/json.hpp>
#include <endstone/command/command_sender_wrapper.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <system_error>
#include <utility>
#include <variant>

namespace fs = std::filesystem;
using json = nlohmann::json;

ENDSTONE_PLUGIN(/*name=*/"bedrock_discord_bridge",
                /*version=*/"0.5.0",
                /*main_class=*/BedrockDiscordBridgePlugin)
{
    prefix = "BedrockDiscordBridge";
    description = "Endstone C++ Bedrock-to-Discord bridge with JSON config, webhook queue, and skin-head avatar cache.";
    authors = {"Bedrock Discord Bridge contributors"};

    command("discordbridge")
        .description("Inspect or reload the Bedrock Discord bridge.")
        .usages("/discordbridge status", "/discordbridge reload")
        .aliases("dbbridge")
        .permissions("bedrock_discord_bridge.command.status", "bedrock_discord_bridge.command.reload");

    permission("bedrock_discord_bridge.command")
        .description("Allows use of all Bedrock Discord bridge commands.")
        .children("bedrock_discord_bridge.command.status", true)
        .children("bedrock_discord_bridge.command.reload", true);

    permission("bedrock_discord_bridge.command.status")
        .description("Allows viewing Bedrock Discord bridge status.")
        .default_(endstone::PermissionDefault::Operator);

    permission("bedrock_discord_bridge.command.reload")
        .description("Allows reloading Bedrock Discord bridge configuration.")
        .default_(endstone::PermissionDefault::Operator);
}

BedrockDiscordBridgePlugin::~BedrockDiscordBridgePlugin()
{
    stopBridgeServer();
    stopWorker();
}

void BedrockDiscordBridgePlugin::onLoad()
{
    getLogger().info("Loading plugin.");
}

void BedrockDiscordBridgePlugin::onEnable()
{
    ensureDataFolder();
    writeDefaultConfigIfMissing();
    loadConfig();
    registerEvent(&BedrockDiscordBridgePlugin::onPlayerChat, *this);
    registerEvent(&BedrockDiscordBridgePlugin::onPlayerJoin, *this);
    registerEvent(&BedrockDiscordBridgePlugin::onPlayerQuit, *this);
    registerEvent(&BedrockDiscordBridgePlugin::onPlayerDeath, *this);
    restartRuntime();

    getLogger().info("Plugin enabled. Chat forwarding pipeline is active.");
}

void BedrockDiscordBridgePlugin::onDisable()
{
    stopBridgeServer();
    stopWorker();
    getLogger().info("Plugin disabled.");
}

bool BedrockDiscordBridgePlugin::onCommand(endstone::CommandSender &sender, const endstone::Command &command,
                                           const std::vector<std::string> &args)
{
    if (command.getName() != "discordbridge") {
        return false;
    }

    const auto subcommand = args.empty() ? std::string("status") : args[0];
    if (subcommand == "status") {
        if (!sender.hasPermission("bedrock_discord_bridge.command.status")) {
            sender.sendErrorMessage("You do not have permission to view bridge status.");
            return true;
        }

        sendStatus(sender);
        return true;
    }

    if (subcommand == "reload") {
        if (!sender.hasPermission("bedrock_discord_bridge.command.reload")) {
            sender.sendErrorMessage("You do not have permission to reload bridge configuration.");
            return true;
        }

        stopBridgeServer();
        stopWorker();
        clearQueue();
        avatar_cache_.clear();
        loadConfig();
        restartRuntime();
        sender.sendMessage("Bedrock Discord bridge configuration reloaded from '{}'.", getConfigPath().string());
        return true;
    }

    sender.sendErrorMessage("Usage: /discordbridge <status|reload>");
    return true;
}

void BedrockDiscordBridgePlugin::onPlayerChat(endstone::PlayerChatEvent &event)
{
    if (event.isCancelled()) {
        return;
    }

    forwardChatToDiscord(event.getPlayer(), event.getMessage());
}

void BedrockDiscordBridgePlugin::onPlayerJoin(endstone::PlayerJoinEvent &event)
{
    if (!config_.relay.join_enabled) {
        if (config_.logging.log_filtered_events) {
            getLogger().debug("Skipping join relay for '{}' because relay.join_enabled is false.",
                              event.getPlayer().getName());
        }
        return;
    }

    const auto join_message = event.getJoinMessage().has_value() ? messageToPlainText(*event.getJoinMessage()) : "";
    forwardLifecycleEventToDiscord(event.getPlayer(), "join", config_.discord.join_content_template, join_message);
}

void BedrockDiscordBridgePlugin::onPlayerQuit(endstone::PlayerQuitEvent &event)
{
    if (!config_.relay.quit_enabled) {
        if (config_.logging.log_filtered_events) {
            getLogger().debug("Skipping quit relay for '{}' because relay.quit_enabled is false.",
                              event.getPlayer().getName());
        }
        return;
    }

    const auto quit_message = event.getQuitMessage().has_value() ? messageToPlainText(*event.getQuitMessage()) : "";
    forwardLifecycleEventToDiscord(event.getPlayer(), "quit", config_.discord.quit_content_template, quit_message);
}

void BedrockDiscordBridgePlugin::onPlayerDeath(endstone::PlayerDeathEvent &event)
{
    if (!config_.relay.death_enabled) {
        if (config_.logging.log_filtered_events) {
            getLogger().debug("Skipping death relay for '{}' because relay.death_enabled is false.",
                              event.getPlayer().getName());
        }
        return;
    }

    const auto death_message = event.getDeathMessage().has_value() ? messageToPlainText(*event.getDeathMessage()) : "";
    forwardLifecycleEventToDiscord(event.getPlayer(), "death", config_.discord.death_content_template, death_message);
}

void BedrockDiscordBridgePlugin::ensureDataFolder() const
{
    std::error_code ec;
    fs::create_directories(getDataFolder(), ec);
    if (ec) {
        getLogger().warning("Could not create plugin data folder '{}': {}", getDataFolder().string(), ec.message());
    }
}

void BedrockDiscordBridgePlugin::writeDefaultConfigIfMissing() const
{
    const auto config_path = getConfigPath();
    if (fs::exists(config_path)) {
        return;
    }

    json root = {
        {"config_version", 5},
        {"enabled", true},
        {"discord",
         {{"webhook_url", ""},
          {"allow_runtime_webhook_override", true},
          {"username_template", "{player}"},
          {"content_template", "{message}"},
          {"system_username_template", "{server}"},
          {"join_content_template", ":inbox_tray: **{player}** joined the server."},
          {"quit_content_template", ":outbox_tray: **{player}** left the server."},
          {"death_content_template", ":skull: {event_message}"},
          {"allow_mentions", false},
          {"use_player_avatar_for_system_messages", true},
          {"max_username_length", 80},
          {"max_content_length", 2000}}},
        {"relay",
         {{"minecraft_to_discord_enabled", true},
          {"chat_enabled", true},
          {"join_enabled", true},
          {"quit_enabled", true},
          {"death_enabled", true}}},
        {"queue",
         {{"max_size", 256},
          {"max_attempts", 5},
          {"retry_delay_ms", 2000},
          {"connect_timeout_ms", 3000},
          {"read_timeout_ms", 5000},
          {"write_timeout_ms", 5000}}},
        {"avatar",
         {{"enabled", true},
          {"mode", "provider"},
          {"provider", "tabavatars"},
          {"provider_url_template", ""},
          {"provider_prefer_xuid", true},
          {"provider_render_type", "helm"},
          {"provider_bedrock_username_prefix", "."},
          {"size", 64},
          {"cache_subdirectory", "avatars"},
          {"public_base_url", ""},
          {"http_server",
           {{"enabled", true},
            {"bind_host", "0.0.0.0"},
            {"port", 8089},
            {"route_prefix", "/bedrock-discord-bridge/avatars"},
            {"public_base_url", ""},
            {"cache_control", "public, max-age=86400"},
            {"thread_count", 4}}}}},
        {"bot_bridge",
         {{"enabled", true},
          {"shared_secret", "change-me"},
          {"api_route_prefix", "/bedrock-discord-bridge/api"},
          {"allow_local_requests_only", true},
          {"allowed_remote_addresses", json::array()},
          {"inbound_chat_enabled", true},
          {"command_enabled", true},
          {"outbound_system_messages_enabled", false},
          {"inbound_chat_template", "[Discord] #{channel} <{author}> {content}"},
          {"inbound_chat_max_length", 2000},
          {"outbound_system_message_max_batch", 20},
          {"outbound_system_message_queue_max_size", 256},
          {"request_timeout_ms", 5000}}},
        {"logging",
         {{"log_filtered_events", false},
          {"log_webhook_successes", false},
          {"log_http_requests", false},
          {"log_avatar_cache_hits", false},
          {"log_avatar_cache_misses", false},
          {"log_inbound_chat", false},
          {"log_remote_commands", true}}}
    };

    std::ofstream output(config_path);
    output << root.dump(2) << '\n';
    getLogger().info("Wrote default config to '{}'.", config_path.string());
}

void BedrockDiscordBridgePlugin::loadConfig()
{
    config_ = {};
    webhook_target_.reset();
    runtime_webhook_override_active_ = false;

    const auto config_path = getConfigPath();
    std::ifstream input(config_path);
    if (!input.is_open()) {
        getLogger().warning("Could not open config '{}'. Using defaults.", config_path.string());
        return;
    }

    try {
        json root = json::parse(input);

        if (root.contains("config_version") && root["config_version"].is_number_integer()) {
            config_.config_version = root["config_version"].get<int>();
        }
        if (root.contains("enabled") && root["enabled"].is_boolean()) {
            config_.enabled = root["enabled"].get<bool>();
        }

        if (root.contains("discord") && root["discord"].is_object()) {
            const auto &discord = root["discord"];
            if (discord.contains("webhook_url") && discord["webhook_url"].is_string()) {
                config_.discord.webhook_url = discord["webhook_url"].get<std::string>();
            }
            if (discord.contains("allow_runtime_webhook_override") && discord["allow_runtime_webhook_override"].is_boolean()) {
                config_.discord.allow_runtime_webhook_override = discord["allow_runtime_webhook_override"].get<bool>();
            }
            if (discord.contains("username_template") && discord["username_template"].is_string()) {
                config_.discord.username_template = discord["username_template"].get<std::string>();
            }
            if (discord.contains("content_template") && discord["content_template"].is_string()) {
                config_.discord.content_template = discord["content_template"].get<std::string>();
            }
            if (discord.contains("system_username_template") && discord["system_username_template"].is_string()) {
                config_.discord.system_username_template = discord["system_username_template"].get<std::string>();
            }
            if (discord.contains("join_content_template") && discord["join_content_template"].is_string()) {
                config_.discord.join_content_template = discord["join_content_template"].get<std::string>();
            }
            if (discord.contains("quit_content_template") && discord["quit_content_template"].is_string()) {
                config_.discord.quit_content_template = discord["quit_content_template"].get<std::string>();
            }
            if (discord.contains("death_content_template") && discord["death_content_template"].is_string()) {
                config_.discord.death_content_template = discord["death_content_template"].get<std::string>();
            }
            if (discord.contains("allow_mentions") && discord["allow_mentions"].is_boolean()) {
                config_.discord.allow_mentions = discord["allow_mentions"].get<bool>();
            }
            if (discord.contains("use_player_avatar_for_system_messages") &&
                discord["use_player_avatar_for_system_messages"].is_boolean()) {
                config_.discord.use_player_avatar_for_system_messages =
                    discord["use_player_avatar_for_system_messages"].get<bool>();
            }
            if (discord.contains("max_username_length") && discord["max_username_length"].is_number_integer()) {
                config_.discord.max_username_length = discord["max_username_length"].get<int>();
            }
            if (discord.contains("max_content_length") && discord["max_content_length"].is_number_integer()) {
                config_.discord.max_content_length = discord["max_content_length"].get<int>();
            }
        }

        if (root.contains("relay") && root["relay"].is_object()) {
            const auto &relay = root["relay"];
            if (relay.contains("minecraft_to_discord_enabled") && relay["minecraft_to_discord_enabled"].is_boolean()) {
                config_.relay.minecraft_to_discord_enabled = relay["minecraft_to_discord_enabled"].get<bool>();
            }
            if (relay.contains("chat_enabled") && relay["chat_enabled"].is_boolean()) {
                config_.relay.chat_enabled = relay["chat_enabled"].get<bool>();
            }
            if (relay.contains("join_enabled") && relay["join_enabled"].is_boolean()) {
                config_.relay.join_enabled = relay["join_enabled"].get<bool>();
            }
            if (relay.contains("quit_enabled") && relay["quit_enabled"].is_boolean()) {
                config_.relay.quit_enabled = relay["quit_enabled"].get<bool>();
            }
            if (relay.contains("death_enabled") && relay["death_enabled"].is_boolean()) {
                config_.relay.death_enabled = relay["death_enabled"].get<bool>();
            }
        }

        if (root.contains("queue") && root["queue"].is_object()) {
            const auto &queue = root["queue"];
            if (queue.contains("max_size") && queue["max_size"].is_number_integer()) {
                config_.queue.max_size = queue["max_size"].get<int>();
            }
            if (queue.contains("max_attempts") && queue["max_attempts"].is_number_integer()) {
                config_.queue.max_attempts = queue["max_attempts"].get<int>();
            }
            if (queue.contains("retry_delay_ms") && queue["retry_delay_ms"].is_number_integer()) {
                config_.queue.retry_delay_ms = queue["retry_delay_ms"].get<int>();
            }
            if (queue.contains("connect_timeout_ms") && queue["connect_timeout_ms"].is_number_integer()) {
                config_.queue.connect_timeout_ms = queue["connect_timeout_ms"].get<int>();
            }
            if (queue.contains("read_timeout_ms") && queue["read_timeout_ms"].is_number_integer()) {
                config_.queue.read_timeout_ms = queue["read_timeout_ms"].get<int>();
            }
            if (queue.contains("write_timeout_ms") && queue["write_timeout_ms"].is_number_integer()) {
                config_.queue.write_timeout_ms = queue["write_timeout_ms"].get<int>();
            }
        }

        if (root.contains("avatar") && root["avatar"].is_object()) {
            const auto &avatar = root["avatar"];
            if (avatar.contains("enabled") && avatar["enabled"].is_boolean()) {
                config_.avatar.enabled = avatar["enabled"].get<bool>();
            }
            if (avatar.contains("mode") && avatar["mode"].is_string()) {
                config_.avatar.mode = avatar["mode"].get<std::string>();
            }
            if (avatar.contains("provider") && avatar["provider"].is_string()) {
                config_.avatar.provider = avatar["provider"].get<std::string>();
            }
            if (avatar.contains("provider_url_template") && avatar["provider_url_template"].is_string()) {
                config_.avatar.provider_url_template = avatar["provider_url_template"].get<std::string>();
            }
            if (avatar.contains("provider_prefer_xuid") && avatar["provider_prefer_xuid"].is_boolean()) {
                config_.avatar.provider_prefer_xuid = avatar["provider_prefer_xuid"].get<bool>();
            }
            if (avatar.contains("provider_render_type") && avatar["provider_render_type"].is_string()) {
                config_.avatar.provider_render_type = avatar["provider_render_type"].get<std::string>();
            }
            if (avatar.contains("provider_bedrock_username_prefix") &&
                avatar["provider_bedrock_username_prefix"].is_string()) {
                config_.avatar.provider_bedrock_username_prefix =
                    avatar["provider_bedrock_username_prefix"].get<std::string>();
            }
            if (avatar.contains("size") && avatar["size"].is_number_integer()) {
                config_.avatar.size = avatar["size"].get<int>();
            }
            if (avatar.contains("cache_subdirectory") && avatar["cache_subdirectory"].is_string()) {
                config_.avatar.cache_subdirectory = avatar["cache_subdirectory"].get<std::string>();
            }
            if (avatar.contains("public_base_url") && avatar["public_base_url"].is_string()) {
                config_.avatar.public_base_url = avatar["public_base_url"].get<std::string>();
            }

            if (avatar.contains("http_server") && avatar["http_server"].is_object()) {
                const auto &http = avatar["http_server"];
                if (http.contains("enabled") && http["enabled"].is_boolean()) {
                    config_.avatar.http_server.enabled = http["enabled"].get<bool>();
                }
                if (http.contains("bind_host") && http["bind_host"].is_string()) {
                    config_.avatar.http_server.bind_host = http["bind_host"].get<std::string>();
                }
                if (http.contains("port") && http["port"].is_number_integer()) {
                    config_.avatar.http_server.port = http["port"].get<int>();
                }
                if (http.contains("route_prefix") && http["route_prefix"].is_string()) {
                    config_.avatar.http_server.route_prefix = http["route_prefix"].get<std::string>();
                }
                if (http.contains("public_base_url") && http["public_base_url"].is_string()) {
                    config_.avatar.http_server.public_base_url = http["public_base_url"].get<std::string>();
                }
                if (http.contains("cache_control") && http["cache_control"].is_string()) {
                    config_.avatar.http_server.cache_control = http["cache_control"].get<std::string>();
                }
                if (http.contains("thread_count") && http["thread_count"].is_number_integer()) {
                    config_.avatar.http_server.thread_count = http["thread_count"].get<int>();
                }
            }
        }

        if (root.contains("bot_bridge") && root["bot_bridge"].is_object()) {
            const auto &bot_bridge = root["bot_bridge"];
            if (bot_bridge.contains("enabled") && bot_bridge["enabled"].is_boolean()) {
                config_.bot_bridge.enabled = bot_bridge["enabled"].get<bool>();
            }
            if (bot_bridge.contains("shared_secret") && bot_bridge["shared_secret"].is_string()) {
                config_.bot_bridge.shared_secret = bot_bridge["shared_secret"].get<std::string>();
            }
            if (bot_bridge.contains("api_route_prefix") && bot_bridge["api_route_prefix"].is_string()) {
                config_.bot_bridge.api_route_prefix = bot_bridge["api_route_prefix"].get<std::string>();
            }
            if (bot_bridge.contains("allow_local_requests_only") && bot_bridge["allow_local_requests_only"].is_boolean()) {
                config_.bot_bridge.allow_local_requests_only = bot_bridge["allow_local_requests_only"].get<bool>();
            }
            if (bot_bridge.contains("allowed_remote_addresses") && bot_bridge["allowed_remote_addresses"].is_array()) {
                config_.bot_bridge.allowed_remote_addresses.clear();
                for (const auto &entry : bot_bridge["allowed_remote_addresses"]) {
                    if (entry.is_string()) {
                        config_.bot_bridge.allowed_remote_addresses.push_back(entry.get<std::string>());
                    }
                }
            }
            if (bot_bridge.contains("inbound_chat_enabled") && bot_bridge["inbound_chat_enabled"].is_boolean()) {
                config_.bot_bridge.inbound_chat_enabled = bot_bridge["inbound_chat_enabled"].get<bool>();
            }
            if (bot_bridge.contains("command_enabled") && bot_bridge["command_enabled"].is_boolean()) {
                config_.bot_bridge.command_enabled = bot_bridge["command_enabled"].get<bool>();
            }
            if (bot_bridge.contains("outbound_system_messages_enabled") &&
                bot_bridge["outbound_system_messages_enabled"].is_boolean()) {
                config_.bot_bridge.outbound_system_messages_enabled =
                    bot_bridge["outbound_system_messages_enabled"].get<bool>();
            }
            if (bot_bridge.contains("inbound_chat_template") && bot_bridge["inbound_chat_template"].is_string()) {
                config_.bot_bridge.inbound_chat_template = bot_bridge["inbound_chat_template"].get<std::string>();
            }
            if (bot_bridge.contains("inbound_chat_max_length") &&
                bot_bridge["inbound_chat_max_length"].is_number_integer()) {
                config_.bot_bridge.inbound_chat_max_length = bot_bridge["inbound_chat_max_length"].get<int>();
            }
            if (bot_bridge.contains("outbound_system_message_max_batch") &&
                bot_bridge["outbound_system_message_max_batch"].is_number_integer()) {
                config_.bot_bridge.outbound_system_message_max_batch =
                    bot_bridge["outbound_system_message_max_batch"].get<int>();
            }
            if (bot_bridge.contains("outbound_system_message_queue_max_size") &&
                bot_bridge["outbound_system_message_queue_max_size"].is_number_integer()) {
                config_.bot_bridge.outbound_system_message_queue_max_size =
                    bot_bridge["outbound_system_message_queue_max_size"].get<int>();
            }
            if (bot_bridge.contains("request_timeout_ms") && bot_bridge["request_timeout_ms"].is_number_integer()) {
                config_.bot_bridge.request_timeout_ms = bot_bridge["request_timeout_ms"].get<int>();
            }
        }

        if (root.contains("logging") && root["logging"].is_object()) {
            const auto &logging = root["logging"];
            if (logging.contains("log_filtered_events") && logging["log_filtered_events"].is_boolean()) {
                config_.logging.log_filtered_events = logging["log_filtered_events"].get<bool>();
            }
            if (logging.contains("log_webhook_successes") && logging["log_webhook_successes"].is_boolean()) {
                config_.logging.log_webhook_successes = logging["log_webhook_successes"].get<bool>();
            }
            if (logging.contains("log_http_requests") && logging["log_http_requests"].is_boolean()) {
                config_.logging.log_http_requests = logging["log_http_requests"].get<bool>();
            }
            if (logging.contains("log_avatar_cache_hits") && logging["log_avatar_cache_hits"].is_boolean()) {
                config_.logging.log_avatar_cache_hits = logging["log_avatar_cache_hits"].get<bool>();
            }
            if (logging.contains("log_avatar_cache_misses") && logging["log_avatar_cache_misses"].is_boolean()) {
                config_.logging.log_avatar_cache_misses = logging["log_avatar_cache_misses"].get<bool>();
            }
            if (logging.contains("log_inbound_chat") && logging["log_inbound_chat"].is_boolean()) {
                config_.logging.log_inbound_chat = logging["log_inbound_chat"].get<bool>();
            }
            if (logging.contains("log_remote_commands") && logging["log_remote_commands"].is_boolean()) {
                config_.logging.log_remote_commands = logging["log_remote_commands"].get<bool>();
            }
        }
    }
    catch (const std::exception &e) {
        getLogger().error("Failed to parse config '{}': {}", config_path.string(), e.what());
        return;
    }

    config_.discord.max_username_length = std::clamp(config_.discord.max_username_length, 1, 80);
    config_.discord.max_content_length = std::clamp(config_.discord.max_content_length, 1, 2000);
    config_.queue.max_size = std::max(config_.queue.max_size, 1);
    config_.queue.max_attempts = std::clamp(config_.queue.max_attempts, 1, 20);
    config_.queue.retry_delay_ms = std::max(config_.queue.retry_delay_ms, 100);
    config_.queue.connect_timeout_ms = std::max(config_.queue.connect_timeout_ms, 100);
    config_.queue.read_timeout_ms = std::max(config_.queue.read_timeout_ms, 100);
    config_.queue.write_timeout_ms = std::max(config_.queue.write_timeout_ms, 100);
    config_.avatar.size = std::clamp(config_.avatar.size, 8, 512);
    config_.avatar.mode = normalizeAvatarMode(config_.avatar.mode);
    config_.avatar.provider = normalizeAvatarProvider(config_.avatar.provider);
    config_.avatar.http_server.port = std::clamp(config_.avatar.http_server.port, 1, 65535);
    config_.avatar.http_server.thread_count = std::clamp(config_.avatar.http_server.thread_count, 1, 64);
    config_.avatar.http_server.route_prefix =
        normalizeRoutePrefix(config_.avatar.http_server.route_prefix.empty() ? "/bedrock-discord-bridge/avatars"
                                                                             : config_.avatar.http_server.route_prefix);
    config_.bot_bridge.api_route_prefix =
        normalizeRoutePrefix(config_.bot_bridge.api_route_prefix.empty() ? "/bedrock-discord-bridge/api"
                                                                         : config_.bot_bridge.api_route_prefix);
    config_.bot_bridge.shared_secret = normalizeSecret(config_.bot_bridge.shared_secret);
    config_.bot_bridge.inbound_chat_max_length = std::clamp(config_.bot_bridge.inbound_chat_max_length, 1, 4000);
    config_.bot_bridge.outbound_system_message_max_batch =
        std::clamp(config_.bot_bridge.outbound_system_message_max_batch, 1, 100);
    config_.bot_bridge.outbound_system_message_queue_max_size =
        std::clamp(config_.bot_bridge.outbound_system_message_queue_max_size, 1, 4096);
    config_.bot_bridge.request_timeout_ms = std::max(config_.bot_bridge.request_timeout_ms, 250);

    if (!config_.avatar.public_base_url.empty() && config_.avatar.public_base_url.ends_with('/')) {
        config_.avatar.public_base_url.pop_back();
    }
    if (!config_.avatar.provider_url_template.empty() && config_.avatar.provider_url_template.ends_with('/')) {
        config_.avatar.provider_url_template.pop_back();
    }
    if (!config_.avatar.http_server.public_base_url.empty() && config_.avatar.http_server.public_base_url.ends_with('/')) {
        config_.avatar.http_server.public_base_url.pop_back();
    }

    if (!config_.discord.webhook_url.empty()) {
        webhook_target_ = parseWebhookUrl(config_.discord.webhook_url);
        if (!webhook_target_) {
            getLogger().error("Configured Discord webhook URL is invalid.");
        }
        clearWebhookState();
    }
    else if (config_.discord.allow_runtime_webhook_override) {
        loadWebhookState();
    }
    else {
        clearWebhookState();
    }

    if (config_.bot_bridge.enabled && config_.bot_bridge.shared_secret.empty()) {
        getLogger().warning("Bot bridge is enabled but bot_bridge.shared_secret is empty. Bot API endpoints will reject requests.");
    }

    getLogger().info("Loaded config version {} from '{}'.", config_.config_version, config_path.string());
}

void BedrockDiscordBridgePlugin::restartRuntime()
{
    clearQueue();
    avatar_cache_.clear();

    std::error_code ec;
    fs::create_directories(getAvatarCacheDir(), ec);
    if (ec) {
        getLogger().warning("Could not create avatar cache directory '{}': {}", getAvatarCacheDir().string(),
                            ec.message());
    }

    if (!config_.enabled) {
        getLogger().warning("Bridge is disabled in config.");
        return;
    }

    startBridgeServer();
    startWorker();
}

void BedrockDiscordBridgePlugin::clearQueue()
{
    {
        std::lock_guard lock(queue_mutex_);
        webhook_queue_.clear();
        next_request_at_ = std::chrono::steady_clock::now();
    }

    {
        std::lock_guard lock(system_message_mutex_);
        pending_system_messages_.clear();
    }
}

void BedrockDiscordBridgePlugin::startWorker()
{
    stopWorker();
    webhook_client_.reset();

    if (!config_.enabled) {
        return;
    }

    if (!config_.relay.minecraft_to_discord_enabled) {
        if (config_.logging.log_filtered_events) {
            getLogger().debug("Webhook worker not started because relay.minecraft_to_discord_enabled is false.");
        }
        return;
    }

    if (!webhook_target_) {
        getLogger().warning("Webhook worker not started because the Discord webhook URL is missing or invalid.");
        return;
    }

    webhook_client_ = std::make_unique<httplib::Client>(webhook_target_->origin);
    webhook_client_->set_connection_timeout(std::chrono::milliseconds(config_.queue.connect_timeout_ms));
    webhook_client_->set_read_timeout(std::chrono::milliseconds(config_.queue.read_timeout_ms));
    webhook_client_->set_write_timeout(std::chrono::milliseconds(config_.queue.write_timeout_ms));
    webhook_client_->set_follow_location(true);
    webhook_client_->set_keep_alive(true);
    webhook_client_->set_default_headers({{"User-Agent", "bedrock_discord_bridge/0.5.0"}});

    stop_worker_ = false;
    next_request_at_ = std::chrono::steady_clock::now();
    worker_thread_ = std::thread(&BedrockDiscordBridgePlugin::workerLoop, this);
    getLogger().info("Webhook worker started.");
}

void BedrockDiscordBridgePlugin::stopWorker()
{
    {
        std::lock_guard lock(queue_mutex_);
        stop_worker_ = true;
    }
    queue_cv_.notify_all();

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    webhook_client_.reset();
}

void BedrockDiscordBridgePlugin::startBridgeServer()
{
    stopBridgeServer();

    const bool needs_http_server = config_.enabled &&
                                   ((config_.avatar.enabled && config_.avatar.mode == "rendered" &&
                                     config_.avatar.http_server.enabled) ||
                                    config_.bot_bridge.enabled);
    if (!needs_http_server) {
        return;
    }

    bridge_server_ = std::make_unique<httplib::Server>();
    bridge_server_->set_default_headers({{"Cache-Control", config_.avatar.http_server.cache_control}});
    bridge_server_->new_task_queue = [thread_count = static_cast<size_t>(config_.avatar.http_server.thread_count)] {
        return new httplib::ThreadPool(thread_count);
    };

    if (config_.avatar.enabled && config_.avatar.mode == "rendered" && config_.avatar.http_server.enabled) {
        bridge_server_->set_file_extension_and_mimetype_mapping("png", "image/png");

        const auto cache_dir = getAvatarCacheDir().string();
        if (!bridge_server_->set_mount_point(config_.avatar.http_server.route_prefix, cache_dir,
                                             {{"Cache-Control", config_.avatar.http_server.cache_control}})) {
            getLogger().error("Failed to mount avatar cache directory '{}' at route '{}'.", cache_dir,
                              config_.avatar.http_server.route_prefix);
            bridge_server_.reset();
            return;
        }
    }

    installBotBridgeRoutes();

    bridge_server_->set_logger([this](const auto &req, const auto &res) {
        if (config_.logging.log_http_requests || res.status >= 400) {
            getLogger().debug("Bridge HTTP {} {} -> {}", req.method, req.path, res.status);
        }
    });

    const auto host = config_.avatar.http_server.bind_host;
    const auto port = config_.avatar.http_server.port;
    bridge_server_thread_ = std::thread([this, host, port] {
        if (!bridge_server_) {
            return;
        }

        if (!bridge_server_->listen(host.c_str(), port)) {
            getLogger().error("Bridge HTTP server failed to listen on {}:{}.", host, port);
        }
    });

    getLogger().info("Bridge HTTP server starting on {}:{}.", host, port);
}

void BedrockDiscordBridgePlugin::stopBridgeServer()
{
    if (bridge_server_) {
        bridge_server_->stop();
    }

    if (bridge_server_thread_.joinable()) {
        bridge_server_thread_.join();
    }

    bridge_server_.reset();
}

void BedrockDiscordBridgePlugin::workerLoop()
{
    while (true) {
        WebhookJob job;

        {
            std::unique_lock lock(queue_mutex_);

            while (!stop_worker_) {
                if (webhook_queue_.empty()) {
                    queue_cv_.wait(lock, [this] { return stop_worker_ || !webhook_queue_.empty(); });
                    continue;
                }

                const auto now = std::chrono::steady_clock::now();
                if (now < next_request_at_) {
                    queue_cv_.wait_until(lock, next_request_at_);
                    continue;
                }

                job = webhook_queue_.front();
                webhook_queue_.pop_front();
                break;
            }

            if (stop_worker_) {
                return;
            }
        }

        processWebhookJob(std::move(job));
    }
}

void BedrockDiscordBridgePlugin::forwardChatToDiscord(const endstone::Player &player, const std::string &message)
{
    if (!config_.enabled || !config_.relay.minecraft_to_discord_enabled) {
        if (config_.logging.log_filtered_events) {
            getLogger().debug("Skipping chat relay for '{}' because Minecraft-to-Discord relay is disabled.",
                              player.getName());
        }
        return;
    }

    if (!config_.relay.chat_enabled) {
        if (config_.logging.log_filtered_events) {
            getLogger().debug("Skipping chat relay for '{}' because relay.chat_enabled is false.", player.getName());
        }
        return;
    }

    const auto avatar_url = getOrCreateAvatarUrl(player);
    const auto skin = player.getSkin();
    auto username = applyTemplate(config_.discord.username_template,
                                  {{"{player}", player.getName()},
                                   {"{message}", message},
                                   {"{event_message}", message},
                                   {"{event}", "chat"},
                                   {"{skin_id}", skin.getId()},
                                   {"{server}", getServer().getName()}});
    auto content = applyTemplate(config_.discord.content_template,
                                 {{"{player}", player.getName()},
                                  {"{message}", message},
                                  {"{event_message}", message},
                                  {"{event}", "chat"},
                                  {"{skin_id}", skin.getId()},
                                  {"{server}", getServer().getName()}});

    enqueueDiscordMessage(player.getName(), std::move(username), std::move(content), avatar_url);
}

void BedrockDiscordBridgePlugin::forwardLifecycleEventToDiscord(const endstone::Player &player,
                                                                const std::string &event_name,
                                                                const std::string &content_template,
                                                                const std::string &event_message)
{
    if (!config_.enabled || !config_.relay.minecraft_to_discord_enabled) {
        if (config_.logging.log_filtered_events) {
            getLogger().debug("Skipping {} relay for '{}' because Minecraft-to-Discord relay is disabled.", event_name,
                              player.getName());
        }
        return;
    }

    const auto skin = player.getSkin();
    auto username = applyTemplate(config_.discord.system_username_template,
                                  {{"{player}", player.getName()},
                                   {"{message}", event_message},
                                   {"{event_message}", event_message},
                                   {"{event}", event_name},
                                   {"{skin_id}", skin.getId()},
                                   {"{server}", getServer().getName()}});
    auto content = applyTemplate(content_template,
                                 {{"{player}", player.getName()},
                                  {"{message}", event_message},
                                  {"{event_message}", event_message},
                                  {"{event}", event_name},
                                  {"{skin_id}", skin.getId()},
                                  {"{server}", getServer().getName()}});

    std::optional<std::string> avatar_url = std::nullopt;
    if (config_.discord.use_player_avatar_for_system_messages) {
        avatar_url = getOrCreateAvatarUrl(player);
    }

    if (config_.bot_bridge.enabled && config_.bot_bridge.outbound_system_messages_enabled) {
        enqueueBotSystemMessage(event_name, player.getName(), content);
        return;
    }

    enqueueDiscordMessage(player.getName(), std::move(username), std::move(content), avatar_url);
}

void BedrockDiscordBridgePlugin::enqueueDiscordMessage(const std::string &source_name, std::string username,
                                                       std::string content,
                                                       const std::optional<std::string> &avatar_url)
{
    if (username.empty()) {
        username = source_name;
    }
    if (content.empty()) {
        if (config_.logging.log_filtered_events) {
            getLogger().debug("Skipping Discord webhook POST for '{}' because the formatted content is empty.",
                              source_name);
        }
        return;
    }

    if (static_cast<int>(username.size()) > config_.discord.max_username_length) {
        username.resize(static_cast<std::size_t>(config_.discord.max_username_length));
    }
    if (static_cast<int>(content.size()) > config_.discord.max_content_length) {
        const auto limit = static_cast<std::size_t>(std::max(config_.discord.max_content_length - 3, 0));
        content = content.substr(0, limit) + "...";
    }

    std::ostringstream payload;
    payload << "{"
            << "\"username\":\"" << escapeJson(username) << "\","
            << "\"content\":\"" << escapeJson(content) << "\"";
    if (avatar_url.has_value()) {
        payload << ",\"avatar_url\":\"" << escapeJson(*avatar_url) << "\"";
    }
    if (!config_.discord.allow_mentions) {
        payload << ",\"allowed_mentions\":{\"parse\":[]}";
    }
    payload << "}";

    if (!webhook_target_) {
        if (config_.logging.log_filtered_events) {
            getLogger().debug("Skipping Discord webhook POST for '{}' because the Discord webhook URL is not configured.",
                              source_name);
        }
        return;
    }

    enqueueWebhookPayload({source_name, payload.str(), 0});
}

void BedrockDiscordBridgePlugin::enqueueBotSystemMessage(std::string event_name, std::string player_name,
                                                         std::string content)
{
    if (content.empty()) {
        if (config_.logging.log_filtered_events) {
            getLogger().debug("Skipping bot system message for '{}' because the formatted content is empty.",
                              player_name);
        }
        return;
    }

    std::lock_guard lock(system_message_mutex_);
    if (static_cast<int>(pending_system_messages_.size()) >= config_.bot_bridge.outbound_system_message_queue_max_size) {
        getLogger().warning("Dropping bot-owned system message for '{}' because the queue is full ({} items).",
                            player_name, config_.bot_bridge.outbound_system_message_queue_max_size);
        return;
    }
    pending_system_messages_.push_back(
        {std::move(event_name), std::move(player_name), truncateUtf8Bytes(content, 2000)});
}

void BedrockDiscordBridgePlugin::sendStatus(endstone::CommandSender &sender) const
{
    std::size_t queue_depth = 0;
    {
        std::lock_guard lock(queue_mutex_);
        queue_depth = webhook_queue_.size();
    }
    std::size_t system_message_queue_depth = 0;
    {
        std::lock_guard lock(system_message_mutex_);
        system_message_queue_depth = pending_system_messages_.size();
    }

    sender.sendMessage("Bridge enabled: {}", config_.enabled ? "yes" : "no");
    sender.sendMessage("Minecraft -> Discord relay enabled: {}", config_.relay.minecraft_to_discord_enabled ? "yes" : "no");
    sender.sendMessage("Chat relay enabled: {}", config_.relay.chat_enabled ? "yes" : "no");
    sender.sendMessage("Join relay enabled: {}", config_.relay.join_enabled ? "yes" : "no");
    sender.sendMessage("Quit relay enabled: {}", config_.relay.quit_enabled ? "yes" : "no");
    sender.sendMessage("Death relay enabled: {}", config_.relay.death_enabled ? "yes" : "no");
    sender.sendMessage("Webhook configured: {}", webhook_target_.has_value() ? "yes" : "no");
    sender.sendMessage("Runtime webhook override active: {}", runtime_webhook_override_active_ ? "yes" : "no");
    sender.sendMessage("Webhook queue depth: {}", queue_depth);
    sender.sendMessage("Bot system messages enabled: {}", config_.bot_bridge.outbound_system_messages_enabled ? "yes" : "no");
    sender.sendMessage("Bot system message queue depth: {}", system_message_queue_depth);
    sender.sendMessage("Bot system message queue max: {}", config_.bot_bridge.outbound_system_message_queue_max_size);
    sender.sendMessage("Avatar rendering enabled: {}", config_.avatar.enabled ? "yes" : "no");
    sender.sendMessage("Avatar mode: {}", config_.avatar.mode);
    sender.sendMessage("Avatar provider: {}", config_.avatar.provider);
    sender.sendMessage("Avatar cache dir: {}", getAvatarCacheDir().string());
    sender.sendMessage("Avatar HTTP server enabled: {}", config_.avatar.http_server.enabled ? "yes" : "no");
    sender.sendMessage("Avatar HTTP server running: {}",
                       (bridge_server_ && bridge_server_->is_running()) ? "yes" : "no");
    sender.sendMessage("Avatar public base URL: {}", getEffectiveAvatarBaseUrl().value_or("<not configured>"));
    sender.sendMessage("Bot bridge enabled: {}", config_.bot_bridge.enabled ? "yes" : "no");
    sender.sendMessage("Bot bridge API prefix: {}", config_.bot_bridge.api_route_prefix);
}

void BedrockDiscordBridgePlugin::enqueueWebhookPayload(WebhookJob job)
{
    std::lock_guard lock(queue_mutex_);

    if (static_cast<int>(webhook_queue_.size()) >= config_.queue.max_size) {
        getLogger().warning("Dropping Discord webhook payload for '{}' because the queue is full ({} items).",
                            job.player_name, config_.queue.max_size);
        return;
    }

    webhook_queue_.push_back(std::move(job));
    queue_cv_.notify_one();
}

void BedrockDiscordBridgePlugin::processWebhookJob(WebhookJob job)
{
    if (!webhook_target_ || !webhook_client_) {
        return;
    }

    auto result = webhook_client_->Post(webhook_target_->path.c_str(), job.payload, "application/json");
    if (!result) {
        getLogger().warning("Discord webhook POST failed for '{}': {}", job.player_name,
                            httplib::to_string(result.error()));

        if (job.attempt < config_.queue.max_attempts) {
            job.attempt++;
            {
                std::lock_guard lock(queue_mutex_);
                next_request_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.queue.retry_delay_ms);
                webhook_queue_.push_front(std::move(job));
            }
            queue_cv_.notify_one();
        }
        return;
    }

    if (result->status == 200 || result->status == 204) {
        if (config_.logging.log_webhook_successes) {
            getLogger().info("Discord webhook POST succeeded for '{}' with status {}.", job.player_name, result->status);
        }
        if (result->has_header("X-RateLimit-Remaining") && result->get_header_value("X-RateLimit-Remaining") == "0") {
            if (const auto reset_after = parseRetryDelayMs(result->get_header_value("X-RateLimit-Reset-After"))) {
                std::lock_guard lock(queue_mutex_);
                next_request_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(*reset_after);
            }
        }
        return;
    }

    if (result->status == 429 || result->status >= 500) {
        std::int64_t delay_ms = config_.queue.retry_delay_ms;
        if (const auto header_delay = parseRetryDelayMs(result->get_header_value("Retry-After"))) {
            delay_ms = *header_delay;
        }
        else if (const auto reset_after = parseRetryDelayMs(result->get_header_value("X-RateLimit-Reset-After"))) {
            delay_ms = *reset_after;
        }
        else if (const auto body_delay = parseRetryDelayMsFromBody(result->body)) {
            delay_ms = *body_delay;
        }

        getLogger().warning("Discord webhook returned status {} for '{}'. Retrying in {} ms.", result->status,
                            job.player_name, delay_ms);
        if (job.attempt < config_.queue.max_attempts) {
            job.attempt++;
            {
                std::lock_guard lock(queue_mutex_);
                next_request_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
                webhook_queue_.push_front(std::move(job));
            }
            queue_cv_.notify_one();
        }
        return;
    }

    getLogger().warning("Discord webhook rejected payload for '{}' with status {}.", job.player_name, result->status);
}

void BedrockDiscordBridgePlugin::installBotBridgeRoutes()
{
    if (!bridge_server_ || !config_.bot_bridge.enabled) {
        return;
    }

    bridge_server_->Get(config_.bot_bridge.api_route_prefix + "/status",
                        [this](const httplib::Request &req, httplib::Response &res) { handleBotBridgeStatus(req, res); });
    bridge_server_->Post(config_.bot_bridge.api_route_prefix + "/chat",
                         [this](const httplib::Request &req, httplib::Response &res) { handleBotBridgeChat(req, res); });
    bridge_server_->Post(config_.bot_bridge.api_route_prefix + "/command",
                         [this](const httplib::Request &req, httplib::Response &res) { handleBotBridgeCommand(req, res); });
    bridge_server_->Post(config_.bot_bridge.api_route_prefix + "/system-messages/drain",
                         [this](const httplib::Request &req, httplib::Response &res) {
                             handleBotBridgeDrainSystemMessages(req, res);
                         });
    bridge_server_->Post(config_.bot_bridge.api_route_prefix + "/webhook",
                         [this](const httplib::Request &req, httplib::Response &res) {
                             handleBotBridgeConfigureWebhook(req, res);
                         });
    bridge_server_->Get(config_.bot_bridge.api_route_prefix + "/healthz",
                        [this](const httplib::Request &req, httplib::Response &res) { handleBotBridgeHealth(req, res); });
}

void BedrockDiscordBridgePlugin::handleBotBridgeChat(const httplib::Request &req, httplib::Response &res)
{
    if (!isAuthorizedBotBridgeRequest(req, res)) {
        return;
    }
    if (!config_.bot_bridge.inbound_chat_enabled) {
        res.status = 403;
        res.set_content(R"({"ok":false,"error":"inbound chat relay disabled"})", "application/json");
        return;
    }

    json body;
    try {
        body = json::parse(req.body);
    }
    catch (const std::exception &e) {
        res.status = 400;
        res.set_content(json({{"ok", false}, {"error", std::string("invalid json: ") + e.what()}}).dump(),
                        "application/json");
        return;
    }

    const auto author = body.value("author", "");
    auto content = body.value("content", "");
    const auto channel = body.value("channel_name", "discord");
    const auto guild = body.value("guild_name", "");
    const auto message_url = body.value("message_url", "");

    if (author.empty() || content.empty()) {
        res.status = 400;
        res.set_content(R"({"ok":false,"error":"author and content are required"})", "application/json");
        return;
    }

    content = truncateUtf8Bytes(content, static_cast<std::size_t>(config_.bot_bridge.inbound_chat_max_length));
    const auto formatted = replaceAll(
        replaceAll(
            replaceAll(
                replaceAll(
                    replaceAll(
                        replaceAll(config_.bot_bridge.inbound_chat_template, "{author}", author), "{content}", content),
                    "{channel}", channel),
                "{guild}", guild),
            "{message_url}", message_url),
        "{server}", getServer().getName());
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();

    if (config_.logging.log_inbound_chat) {
        getLogger().info("Relaying Discord message from '{}' in '#{}' into Minecraft.", author, channel);
    }

    getServer().getScheduler().runTask(*this, [this, promise, formatted] {
        getServer().broadcastMessage(formatted);
        promise->set_value();
    });

    if (future.wait_for(std::chrono::milliseconds(config_.bot_bridge.request_timeout_ms)) != std::future_status::ready) {
        res.status = 504;
        res.set_content(R"({"ok":false,"error":"timed out waiting for main-thread chat relay"})", "application/json");
        return;
    }

    res.status = 200;
    res.set_content(json({{"ok", true}, {"message", "chat relayed"}, {"message_url", message_url}}).dump(),
                    "application/json");
}

void BedrockDiscordBridgePlugin::handleBotBridgeCommand(const httplib::Request &req, httplib::Response &res)
{
    if (!isAuthorizedBotBridgeRequest(req, res)) {
        return;
    }
    if (!config_.bot_bridge.command_enabled) {
        res.status = 403;
        res.set_content(R"({"ok":false,"error":"remote command relay disabled"})", "application/json");
        return;
    }

    json body;
    try {
        body = json::parse(req.body);
    }
    catch (const std::exception &e) {
        res.status = 400;
        res.set_content(json({{"ok", false}, {"error", std::string("invalid json: ") + e.what()}}).dump(),
                        "application/json");
        return;
    }

    const auto actor = body.value("actor_name", "discord");
    auto command_line = body.value("command", "");
    if (command_line.empty()) {
        res.status = 400;
        res.set_content(R"({"ok":false,"error":"command is required"})", "application/json");
        return;
    }
    if (!command_line.empty() && command_line.front() == '/') {
        command_line.erase(command_line.begin());
    }

    struct CommandResult {
        bool ok = false;
        bool dispatched = false;
        std::vector<std::string> output;
        std::vector<std::string> errors;
    };

    auto promise = std::make_shared<std::promise<CommandResult>>();
    auto future = promise->get_future();

    getServer().getScheduler().runTask(*this, [this, promise, actor, command_line] {
        CommandResult result;
        auto &console = getServer().getCommandSender();
        endstone::CommandSenderWrapper wrapper(
            console,
            [&result](const endstone::Message &message) { result.output.push_back(messageToPlainText(message)); },
            [&result](const endstone::Message &message) { result.errors.push_back(messageToPlainText(message)); });

        if (config_.logging.log_remote_commands) {
            getLogger().info("Executing remote Discord command from '{}': {}", actor, command_line);
        }
        result.dispatched = getServer().dispatchCommand(wrapper, command_line);
        result.ok = result.dispatched;
        promise->set_value(std::move(result));
    });

    if (future.wait_for(std::chrono::milliseconds(config_.bot_bridge.request_timeout_ms)) != std::future_status::ready) {
        res.status = 504;
        res.set_content(R"({"ok":false,"error":"timed out waiting for main-thread command execution"})", "application/json");
        return;
    }

    const auto result = future.get();
    res.status = result.ok ? 200 : 400;
    res.set_content(json({{"ok", result.ok},
                          {"dispatched", result.dispatched},
                          {"output", result.output},
                          {"errors", result.errors}})
                        .dump(),
                    "application/json");
}

void BedrockDiscordBridgePlugin::handleBotBridgeStatus(const httplib::Request &req, httplib::Response &res)
{
    if (!isAuthorizedBotBridgeRequest(req, res)) {
        return;
    }

    std::size_t queue_depth = 0;
    {
        std::lock_guard lock(queue_mutex_);
        queue_depth = webhook_queue_.size();
    }

    std::size_t system_message_queue_depth = 0;
    {
        std::lock_guard lock(system_message_mutex_);
        system_message_queue_depth = pending_system_messages_.size();
    }

    res.status = 200;
    res.set_content(json({{"ok", true},
                          {"server_name", getServer().getName()},
                          {"minecraft_version", getServer().getMinecraftVersion()},
                          {"online_players", getServer().getOnlinePlayers().size()},
                          {"webhook_configured", webhook_target_.has_value()},
                          {"runtime_webhook_override_active", runtime_webhook_override_active_},
                          {"webhook_queue_depth", queue_depth},
                          {"system_message_queue_depth", system_message_queue_depth},
                          {"system_message_queue_max", config_.bot_bridge.outbound_system_message_queue_max_size},
                          {"minecraft_to_discord_enabled", config_.relay.minecraft_to_discord_enabled},
                          {"discord_to_minecraft_enabled", config_.bot_bridge.inbound_chat_enabled},
                          {"bot_system_messages_enabled", config_.bot_bridge.outbound_system_messages_enabled},
                          {"avatar_enabled", config_.avatar.enabled},
                          {"avatar_mode", config_.avatar.mode},
                          {"avatar_provider", config_.avatar.provider},
                          {"avatar_base_url", getEffectiveAvatarBaseUrl().value_or("")},
                          {"bot_bridge_enabled", config_.bot_bridge.enabled}})
                        .dump(),
                    "application/json");
}

void BedrockDiscordBridgePlugin::handleBotBridgeDrainSystemMessages(const httplib::Request &req,
                                                                    httplib::Response &res)
{
    if (!isAuthorizedBotBridgeRequest(req, res)) {
        return;
    }
    if (!config_.bot_bridge.outbound_system_messages_enabled) {
        res.status = 403;
        res.set_content(R"({"ok":false,"error":"bot system message relay disabled"})", "application/json");
        return;
    }

    json messages = json::array();
    {
        std::lock_guard lock(system_message_mutex_);
        const auto batch_size =
            std::min<std::size_t>(pending_system_messages_.size(),
                                  static_cast<std::size_t>(config_.bot_bridge.outbound_system_message_max_batch));
        for (std::size_t i = 0; i < batch_size; ++i) {
            auto message = std::move(pending_system_messages_.front());
            pending_system_messages_.pop_front();
            messages.push_back({{"event", message.event_name},
                                {"player_name", message.player_name},
                                {"content", message.content}});
        }
    }

    res.status = 200;
    res.set_content(json({{"ok", true}, {"messages", messages}}).dump(), "application/json");
}

void BedrockDiscordBridgePlugin::handleBotBridgeConfigureWebhook(const httplib::Request &req, httplib::Response &res)
{
    if (!isAuthorizedBotBridgeRequest(req, res)) {
        return;
    }
    if (!config_.discord.allow_runtime_webhook_override) {
        res.status = 403;
        res.set_content(R"({"ok":false,"error":"runtime webhook override disabled"})", "application/json");
        return;
    }

    json body;
    try {
        body = json::parse(req.body);
    }
    catch (const std::exception &e) {
        res.status = 400;
        res.set_content(json({{"ok", false}, {"error", std::string("invalid json: ") + e.what()}}).dump(),
                        "application/json");
        return;
    }

    const auto webhook_url = body.value("webhook_url", "");
    if (webhook_url.empty()) {
        res.status = 400;
        res.set_content(R"({"ok":false,"error":"webhook_url is required"})", "application/json");
        return;
    }

    const auto parsed = parseWebhookUrl(webhook_url);
    if (!parsed) {
        res.status = 400;
        res.set_content(R"({"ok":false,"error":"webhook_url is invalid"})", "application/json");
        return;
    }

    webhook_target_ = parsed;
    runtime_webhook_override_active_ = true;
    persistWebhookState();
    startWorker();

    res.status = 200;
    res.set_content(json({{"ok", true}, {"webhook_configured", true}, {"runtime_override", true}}).dump(),
                    "application/json");
}

void BedrockDiscordBridgePlugin::handleBotBridgeHealth(const httplib::Request &req, httplib::Response &res)
{
    if (!isAuthorizedBotBridgeHealthRequest(req)) {
        res.status = 403;
        res.set_content(R"({"ok":false,"error":"request is not from an allowed address"})", "application/json");
        return;
    }

    std::size_t queue_depth = 0;
    {
        std::lock_guard lock(queue_mutex_);
        queue_depth = webhook_queue_.size();
    }

    res.status = 200;
    res.set_content(json({{"ok", true},
                          {"server_running", true},
                          {"http_server_running", bridge_server_ && bridge_server_->is_running()},
                          {"webhook_worker_running", worker_thread_.joinable()},
                          {"webhook_configured", webhook_target_.has_value()},
                          {"webhook_queue_depth", queue_depth}})
                        .dump(),
                    "application/json");
}

bool BedrockDiscordBridgePlugin::isAuthorizedBotBridgeRequest(const httplib::Request &req, httplib::Response &res) const
{
    if (!config_.bot_bridge.enabled) {
        res.status = 403;
        res.set_content(R"({"ok":false,"error":"bot bridge disabled"})", "application/json");
        return false;
    }

    const bool remote_is_loopback = isLoopbackAddress(req.remote_addr);
    const bool remote_is_allowlisted =
        isAllowedRemoteAddress(req.remote_addr, config_.bot_bridge.allowed_remote_addresses);

    if (config_.bot_bridge.allow_local_requests_only && !remote_is_loopback && !remote_is_allowlisted) {
        res.status = 403;
        res.set_content(R"({"ok":false,"error":"request is not from a loopback or allowlisted address"})",
                        "application/json");
        return false;
    }
    if (!config_.bot_bridge.allow_local_requests_only && !config_.bot_bridge.allowed_remote_addresses.empty() &&
        !remote_is_allowlisted && !remote_is_loopback) {
        res.status = 403;
        res.set_content(R"({"ok":false,"error":"request is not from an allowed remote address"})", "application/json");
        return false;
    }

    const auto expected_secret = normalizeSecret(config_.bot_bridge.shared_secret);
    if (expected_secret.empty()) {
        res.status = 503;
        res.set_content(R"({"ok":false,"error":"bot bridge secret is not configured"})", "application/json");
        return false;
    }

    auto provided_secret = normalizeSecret(req.get_header_value("X-Bridge-Secret"));
    if (provided_secret.empty()) {
        auto authorization = req.get_header_value("Authorization");
        if (authorization.starts_with("Bearer ")) {
            provided_secret = normalizeSecret(authorization.substr(7));
        }
    }

    if (provided_secret != expected_secret) {
        res.status = 401;
        res.set_content(R"({"ok":false,"error":"invalid bridge secret"})", "application/json");
        return false;
    }

    return true;
}

bool BedrockDiscordBridgePlugin::isAuthorizedBotBridgeHealthRequest(const httplib::Request &req) const
{
    return isLoopbackAddress(req.remote_addr) ||
           isAllowedRemoteAddress(req.remote_addr, config_.bot_bridge.allowed_remote_addresses);
}

void BedrockDiscordBridgePlugin::loadWebhookState()
{
    const auto state_path = getWebhookStatePath();
    if (!fs::exists(state_path)) {
        return;
    }

    std::ifstream input(state_path);
    if (!input.is_open()) {
        getLogger().warning("Could not open runtime webhook state '{}'.", state_path.string());
        return;
    }

    try {
        const auto state = json::parse(input);
        const auto webhook_url = state.value("webhook_url", "");
        const auto parsed = parseWebhookUrl(webhook_url);
        if (!parsed) {
            getLogger().warning("Ignoring invalid runtime webhook state from '{}'.", state_path.string());
            clearWebhookState();
            return;
        }

        webhook_target_ = parsed;
        runtime_webhook_override_active_ = true;
        getLogger().info("Loaded runtime webhook override from '{}'.", state_path.string());
    }
    catch (const std::exception &e) {
        getLogger().warning("Failed to parse runtime webhook state '{}': {}", state_path.string(), e.what());
        clearWebhookState();
    }
}

void BedrockDiscordBridgePlugin::persistWebhookState() const
{
    if (!runtime_webhook_override_active_ || !webhook_target_.has_value()) {
        return;
    }

    const auto state_path = getWebhookStatePath();
    std::error_code ec;
    fs::create_directories(state_path.parent_path(), ec);
    if (ec) {
        getLogger().warning("Could not create runtime webhook state directory '{}': {}", state_path.parent_path().string(),
                            ec.message());
        return;
    }

    std::ofstream output(state_path);
    if (!output.is_open()) {
        getLogger().warning("Could not write runtime webhook state '{}'.", state_path.string());
        return;
    }

    output << json({{"webhook_url", webhook_target_->origin + webhook_target_->path}}).dump(2) << '\n';
}

void BedrockDiscordBridgePlugin::clearWebhookState() const
{
    std::error_code ec;
    fs::remove(getWebhookStatePath(), ec);
    if (ec && ec.value() != 2) {
        getLogger().warning("Could not clear runtime webhook state '{}': {}", getWebhookStatePath().string(),
                            ec.message());
    }
}

std::optional<std::string> BedrockDiscordBridgePlugin::getOrCreateAvatarUrl(const endstone::Player &player)
{
    if (!config_.avatar.enabled) {
        return std::nullopt;
    }

    if (config_.avatar.mode == "provider") {
        return buildProviderAvatarUrl(player);
    }
    if (config_.avatar.mode == "disabled") {
        return std::nullopt;
    }

    const auto skin = player.getSkin();
    const auto skin_key = computeSkinCacheKey(skin, config_.avatar.size);

    if (const auto it = avatar_cache_.find(skin_key); it != avatar_cache_.end()) {
        if (config_.logging.log_avatar_cache_hits) {
            getLogger().debug("Avatar cache hit for '{}' (skin {}).", player.getName(), skin.getId());
        }
        return it->second.public_url;
    }

    if (const auto entry = renderAvatarIfNeeded(player)) {
        avatar_cache_.emplace(skin_key, *entry);
        return entry->public_url;
    }

    return std::nullopt;
}

std::optional<std::string> BedrockDiscordBridgePlugin::buildProviderAvatarUrl(const endstone::Player &player) const
{
    const auto xuid = player.getXuid();
    const auto username = player.getName();
    const auto skin = player.getSkin();
    const auto uuid = player.getUniqueId().str();

    if (!config_.avatar.provider_url_template.empty()) {
        auto url = applyTemplate(config_.avatar.provider_url_template,
                                 {{"{player}", urlEncode(username)},
                                  {"{username}", urlEncode(username)},
                                  {"{xuid}", urlEncode(xuid)},
                                  {"{uuid}", urlEncode(uuid)},
                                  {"{uuid_nodashes}", urlEncode(replaceAll(uuid, "-", ""))},
                                  {"{skin_id}", urlEncode(skin.getId())},
                                  {"{size}", std::to_string(config_.avatar.size)},
                                  {"{render_type}", urlEncode(config_.avatar.provider_render_type)}});
        return url.empty() ? std::nullopt : std::optional<std::string>(url);
    }

    if (config_.avatar.provider == "tabavatars") {
        if (config_.avatar.provider_prefer_xuid && !xuid.empty()) {
            return "https://tabavatars.net/avatar/?xuid=" + urlEncode(xuid) + "&size=" +
                   std::to_string(config_.avatar.size) + "&type=" + urlEncode(config_.avatar.provider_render_type);
        }
        return "https://tabavatars.net/avatar/?username=" + urlEncode(username) + "&platform=bedrock&size=" +
               std::to_string(config_.avatar.size) + "&type=" + urlEncode(config_.avatar.provider_render_type);
    }

    if (config_.avatar.provider == "mcheads") {
        std::string identifier = username;
        if (config_.avatar.provider_prefer_xuid && !xuid.empty()) {
            identifier = xuid;
        }
        else if (!config_.avatar.provider_bedrock_username_prefix.empty()) {
            identifier = config_.avatar.provider_bedrock_username_prefix + username;
        }
        return "https://api.mcheads.org/head/" + urlEncode(identifier) + "/" + std::to_string(config_.avatar.size);
    }

    return std::nullopt;
}

std::optional<BedrockDiscordBridgePlugin::AvatarCacheEntry>
BedrockDiscordBridgePlugin::renderAvatarIfNeeded(const endstone::Player &player)
{
    const auto skin = player.getSkin();
    const auto skin_key = computeSkinCacheKey(skin, config_.avatar.size);
    const auto avatar_file_name = skin_key + ".png";
    const auto avatar_path = getAvatarCacheDir() / avatar_file_name;

    if (config_.logging.log_avatar_cache_misses) {
        getLogger().debug("Avatar cache miss for '{}' (skin {}).", player.getName(), skin.getId());
    }

    if (!fs::exists(avatar_path) && !writeHeadPng(skin, avatar_path)) {
        return std::nullopt;
    }

    AvatarCacheEntry entry{avatar_path, std::nullopt};
    if (const auto base_url = getEffectiveAvatarBaseUrl()) {
        entry.public_url = joinUrl(*base_url, avatar_file_name);
    }

    return entry;
}

bool BedrockDiscordBridgePlugin::writeHeadPng(const endstone::Skin &skin, const fs::path &output_path) const
{
    std::error_code ec;
    fs::create_directories(output_path.parent_path(), ec);
    if (ec) {
        getLogger().warning("Could not create avatar output directory '{}': {}", output_path.parent_path().string(),
                            ec.message());
        return false;
    }

    const auto rgba = renderHeadRgba(skin.getImage(), config_.avatar.size);
    if (rgba.empty()) {
        getLogger().warning("Could not render a Discord avatar from skin '{}': unsupported skin dimensions {}x{}.",
                            skin.getId(), skin.getImage().getWidth(), skin.getImage().getHeight());
        return false;
    }

    const auto error = lodepng::encode(output_path.string(), rgba,
                                       static_cast<unsigned>(config_.avatar.size),
                                       static_cast<unsigned>(config_.avatar.size));
    if (error != 0U) {
        getLogger().warning("Failed to encode avatar PNG '{}': {}", output_path.string(), lodepng_error_text(error));
        return false;
    }

    return true;
}

std::vector<std::uint8_t> BedrockDiscordBridgePlugin::renderHeadRgba(const endstone::Image &image, int avatar_size)
{
    if (image.getWidth() <= 0 || image.getHeight() <= 0 || image.getWidth() % 64 != 0) {
        return {};
    }

    const int scale = image.getWidth() / 64;
    if (scale <= 0 || image.getHeight() < (16 * scale)) {
        return {};
    }

    std::vector<endstone::Color> face(64, endstone::Color::fromRGBA(0, 0, 0, 0));
    std::vector<endstone::Color> hat(64, endstone::Color::fromRGBA(0, 0, 0, 0));

    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            face[(y * 8) + x] = sampleSkinCell(image, (8 + x) * scale, (8 + y) * scale, scale);
            hat[(y * 8) + x] = sampleSkinCell(image, (40 + x) * scale, (8 + y) * scale, scale);
        }
    }

    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(avatar_size) * static_cast<std::size_t>(avatar_size) * 4U,
                                   0);
    for (int y = 0; y < avatar_size; ++y) {
        const int sample_y = std::min(7, (y * 8) / avatar_size);
        for (int x = 0; x < avatar_size; ++x) {
            const int sample_x = std::min(7, (x * 8) / avatar_size);
            auto color = face[(sample_y * 8) + sample_x];
            color = alphaBlend(color, hat[(sample_y * 8) + sample_x]);

            const auto index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(avatar_size) +
                                static_cast<std::size_t>(x)) *
                               4U;
            rgba[index] = static_cast<std::uint8_t>(color.getRed());
            rgba[index + 1] = static_cast<std::uint8_t>(color.getGreen());
            rgba[index + 2] = static_cast<std::uint8_t>(color.getBlue());
            rgba[index + 3] = static_cast<std::uint8_t>(color.getAlpha());
        }
    }

    return rgba;
}

endstone::Color BedrockDiscordBridgePlugin::sampleSkinCell(const endstone::Image &image, int origin_x, int origin_y,
                                                           int scale)
{
    std::uint32_t red = 0;
    std::uint32_t green = 0;
    std::uint32_t blue = 0;
    std::uint32_t alpha = 0;
    std::uint32_t count = 0;

    for (int dy = 0; dy < scale; ++dy) {
        for (int dx = 0; dx < scale; ++dx) {
            const auto color = image.getColor(origin_x + dx, origin_y + dy);
            red += static_cast<std::uint32_t>(color.getRed());
            green += static_cast<std::uint32_t>(color.getGreen());
            blue += static_cast<std::uint32_t>(color.getBlue());
            alpha += static_cast<std::uint32_t>(color.getAlpha());
            ++count;
        }
    }

    if (count == 0) {
        return endstone::Color::fromRGBA(0, 0, 0, 0);
    }

    return endstone::Color::fromRGBA(static_cast<int>(red / count), static_cast<int>(green / count),
                                     static_cast<int>(blue / count), static_cast<int>(alpha / count));
}

endstone::Color BedrockDiscordBridgePlugin::alphaBlend(endstone::Color base, endstone::Color overlay)
{
    const auto overlay_alpha = overlay.getAlpha();
    if (overlay_alpha <= 0) {
        return base;
    }
    if (overlay_alpha >= 255) {
        return overlay;
    }

    const auto base_alpha = base.getAlpha();
    const auto out_alpha = overlay_alpha + ((base_alpha * (255 - overlay_alpha)) / 255);
    if (out_alpha <= 0) {
        return endstone::Color::fromRGBA(0, 0, 0, 0);
    }

    const auto out_red =
        ((overlay.getRed() * overlay_alpha) + (base.getRed() * base_alpha * (255 - overlay_alpha) / 255)) / out_alpha;
    const auto out_green = ((overlay.getGreen() * overlay_alpha) +
                            (base.getGreen() * base_alpha * (255 - overlay_alpha) / 255)) /
                           out_alpha;
    const auto out_blue = ((overlay.getBlue() * overlay_alpha) +
                           (base.getBlue() * base_alpha * (255 - overlay_alpha) / 255)) /
                          out_alpha;

    return endstone::Color::fromRGBA(out_red, out_green, out_blue, out_alpha);
}

std::string BedrockDiscordBridgePlugin::computeSkinCacheKey(const endstone::Skin &skin, int avatar_size)
{
    constexpr std::uint64_t fnv_offset = 1469598103934665603ULL;
    constexpr std::uint64_t fnv_prime = 1099511628211ULL;

    auto hash = fnv_offset;
    const auto mix = [&hash](const unsigned char byte) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= fnv_prime;
    };

    for (const auto ch : skin.getId()) {
        mix(static_cast<unsigned char>(ch));
    }

    mix(static_cast<unsigned char>(avatar_size & 0xff));
    mix(static_cast<unsigned char>((avatar_size >> 8) & 0xff));

    const auto &image = skin.getImage();
    mix(static_cast<unsigned char>(image.getWidth() & 0xff));
    mix(static_cast<unsigned char>((image.getWidth() >> 8) & 0xff));
    mix(static_cast<unsigned char>(image.getHeight() & 0xff));
    mix(static_cast<unsigned char>((image.getHeight() >> 8) & 0xff));
    const auto image_size = image.getData().size();
    for (std::size_t shift = 0; shift < sizeof(image_size); ++shift) {
        mix(static_cast<unsigned char>((image_size >> (shift * 8U)) & 0xffU));
    }

    std::ostringstream stream;
    stream << std::hex << hash;
    return stream.str();
}

std::string BedrockDiscordBridgePlugin::applyTemplate(
    std::string value, const std::vector<std::pair<std::string, std::string>> &replacements)
{
    for (const auto &[needle, replacement] : replacements) {
        value = replaceAll(std::move(value), needle, replacement);
    }
    return value;
}

std::string BedrockDiscordBridgePlugin::replaceAll(std::string value, const std::string &needle,
                                                   const std::string &replacement)
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

std::optional<BedrockDiscordBridgePlugin::WebhookTarget>
BedrockDiscordBridgePlugin::parseWebhookUrl(const std::string &url)
{
    static const std::regex pattern(R"(^(https?)://([^/\s]+)(/.*)$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_match(url, match, pattern)) {
        return std::nullopt;
    }

    return WebhookTarget{match[1].str() + "://" + match[2].str(), match[3].str()};
}

std::optional<std::int64_t> BedrockDiscordBridgePlugin::parseRetryDelayMs(const std::string &value)
{
    if (value.empty()) {
        return std::nullopt;
    }

    try {
        const auto seconds = std::stod(value);
        return static_cast<std::int64_t>(std::ceil(seconds * 1000.0));
    }
    catch (...) {
        return std::nullopt;
    }
}

std::optional<std::int64_t> BedrockDiscordBridgePlugin::parseRetryDelayMsFromBody(const std::string &body)
{
    static const std::regex pattern(R"("retry_after"\s*:\s*([0-9]+(?:\.[0-9]+)?))");
    std::smatch match;
    if (!std::regex_search(body, match, pattern)) {
        return std::nullopt;
    }

    return parseRetryDelayMs(match[1].str());
}

std::string BedrockDiscordBridgePlugin::normalizeRoutePrefix(const std::string &value)
{
    if (value.empty() || value == "/") {
        return "/avatars";
    }

    std::string route = value;
    if (!route.starts_with('/')) {
        route.insert(route.begin(), '/');
    }
    while (route.size() > 1 && route.ends_with('/')) {
        route.pop_back();
    }
    return route;
}

std::string BedrockDiscordBridgePlugin::normalizeSecret(std::string value)
{
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(),
                value.end());
    return value;
}

bool BedrockDiscordBridgePlugin::isWildcardHost(const std::string &host)
{
    return host == "0.0.0.0" || host == "::" || host == "[::]";
}

bool BedrockDiscordBridgePlugin::isLoopbackAddress(const std::string &host)
{
    return host == "127.0.0.1" || host == "::1" || host == "::ffff:127.0.0.1" || host == "localhost";
}

bool BedrockDiscordBridgePlugin::isAllowedRemoteAddress(const std::string &host,
                                                        const std::vector<std::string> &allowed_patterns)
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

std::optional<std::uint32_t> BedrockDiscordBridgePlugin::parseIpv4Address(const std::string &value)
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

std::string BedrockDiscordBridgePlugin::normalizeAvatarMode(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return std::tolower(ch); });
    if (value == "self_hosted" || value == "self-hosted" || value == "http_server") {
        return "rendered";
    }
    if (value == "off" || value == "none") {
        return "disabled";
    }
    if (value != "provider" && value != "rendered" && value != "disabled") {
        return "provider";
    }
    return value;
}

std::string BedrockDiscordBridgePlugin::normalizeAvatarProvider(std::string value)
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

std::string BedrockDiscordBridgePlugin::joinUrl(const std::string &base, const std::string &leaf)
{
    if (base.empty()) {
        return leaf;
    }
    if (base.ends_with('/')) {
        return base + leaf;
    }
    return base + "/" + leaf;
}

std::string BedrockDiscordBridgePlugin::urlEncode(const std::string &value)
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

std::filesystem::path BedrockDiscordBridgePlugin::getConfigPath() const
{
    return getDataFolder() / "config.json";
}

std::filesystem::path BedrockDiscordBridgePlugin::getWebhookStatePath() const
{
    return getDataFolder() / "runtime_webhook.json";
}

std::filesystem::path BedrockDiscordBridgePlugin::getAvatarCacheDir() const
{
    return getDataFolder() / config_.avatar.cache_subdirectory;
}

std::optional<std::string> BedrockDiscordBridgePlugin::getEffectiveAvatarBaseUrl() const
{
    if (!config_.avatar.enabled || config_.avatar.mode != "rendered") {
        return std::nullopt;
    }

    if (!config_.avatar.public_base_url.empty()) {
        return config_.avatar.public_base_url;
    }

    if (!config_.avatar.http_server.enabled) {
        return std::nullopt;
    }

    if (!config_.avatar.http_server.public_base_url.empty()) {
        return config_.avatar.http_server.public_base_url;
    }

    if (!isWildcardHost(config_.avatar.http_server.bind_host)) {
        return "http://" + config_.avatar.http_server.bind_host + ":" +
               std::to_string(config_.avatar.http_server.port) + config_.avatar.http_server.route_prefix;
    }

    return std::nullopt;
}

std::string BedrockDiscordBridgePlugin::messageToPlainText(const endstone::Message &message)
{
    return std::visit(
        [](const auto &value) -> std::string {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return value;
            }
            else {
                std::string rendered = value.getText();
                if (!value.getParameters().empty()) {
                    rendered += " [";
                    for (std::size_t i = 0; i < value.getParameters().size(); ++i) {
                        if (i > 0) {
                            rendered += ", ";
                        }
                        rendered += value.getParameters()[i];
                    }
                    rendered += "]";
                }
                return rendered;
            }
        },
        message);
}

std::string BedrockDiscordBridgePlugin::truncateUtf8Bytes(const std::string &value, std::size_t max_bytes)
{
    if (value.size() <= max_bytes) {
        return value;
    }

    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U) {
        --end;
    }
    return value.substr(0, end);
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
