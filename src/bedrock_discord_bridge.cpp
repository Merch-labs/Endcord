#include "bedrock_discord_bridge.h"
#include "bridge_support.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <endstone/command/command_sender_wrapper.h>

#include <algorithm>
#include <cctype>
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

namespace {
using ReplacementList = bridge_support::ReplacementList;
constexpr auto kBridgeBindHost = "127.0.0.1";
constexpr int kBridgePort = 8089;
constexpr size_t kBridgeThreadCount = 4;

ReplacementList makePlayerTemplateReplacements(const endstone::Player &player, const std::string &payload,
                                               const std::string &event_name, const std::string &server_name)
{
    const auto &skin = player.getSkin();
    return {{"{player}", player.getName()},
            {"{message}", payload},
            {"{event_message}", payload},
            {"{event}", event_name},
            {"{skin_id}", skin.getId()},
            {"{server}", server_name}};
}

ReplacementList makeInboundChatTemplateReplacements(const std::string &author, const std::string &content,
                                                    const std::string &channel, const std::string &guild,
                                                    const std::string &message_url, const std::string &server_name)
{
    return {{"{author}", author},
            {"{content}", content},
            {"{channel}", channel},
            {"{guild}", guild},
            {"{message_url}", message_url},
            {"{server}", server_name}};
}
}  // namespace

ENDSTONE_PLUGIN(/*name=*/"endcord",
                /*version=*/"0.6.0",
                /*main_class=*/EndcordPlugin)
{
    prefix = "Endcord";
    description = "Endstone C++ Bedrock-to-Discord bridge with JSON config, webhook queue, and provider-based avatars.";
    authors = {"Endcord contributors"};

    command("endcord")
        .description("Inspect or reload Endcord.")
        .usages("/endcord status", "/endcord reload")
        .permissions("endcord.command.status", "endcord.command.reload");

    permission("endcord.command")
        .description("Allows use of all Endcord commands.")
        .children("endcord.command.status", true)
        .children("endcord.command.reload", true);

    permission("endcord.command.status")
        .description("Allows viewing Endcord status.")
        .default_(endstone::PermissionDefault::Operator);

    permission("endcord.command.reload")
        .description("Allows reloading Endcord configuration.")
        .default_(endstone::PermissionDefault::Operator);
}

EndcordPlugin::~EndcordPlugin()
{
    stopBridgeServer();
    stopWorker();
}

void EndcordPlugin::onLoad()
{
    getLogger().info("Loading plugin.");
}

void EndcordPlugin::onEnable()
{
    ensureDataFolder();
    writeDefaultConfigIfMissing();
    loadConfig();
    registerEvent(&EndcordPlugin::onPlayerChat, *this);
    registerEvent(&EndcordPlugin::onPlayerJoin, *this);
    registerEvent(&EndcordPlugin::onPlayerQuit, *this);
    registerEvent(&EndcordPlugin::onPlayerDeath, *this);
    restartRuntime();

    getLogger().info("Plugin enabled. Chat forwarding pipeline is active.");
}

void EndcordPlugin::onDisable()
{
    stopBridgeServer();
    stopWorker();
    getLogger().info("Plugin disabled.");
}

bool EndcordPlugin::onCommand(endstone::CommandSender &sender, const endstone::Command &command,
                              const std::vector<std::string> &args)
{
    if (command.getName() != "endcord") {
        return false;
    }

    const auto subcommand = args.empty() ? std::string("status") : args[0];
    if (subcommand == "status") {
        if (!sender.hasPermission("endcord.command.status")) {
            sender.sendErrorMessage("You do not have permission to view bridge status.");
            return true;
        }

        sendStatus(sender);
        return true;
    }

    if (subcommand == "reload") {
        if (!sender.hasPermission("endcord.command.reload")) {
            sender.sendErrorMessage("You do not have permission to reload bridge configuration.");
            return true;
        }

        stopBridgeServer();
        stopWorker();
        clearQueue();
        loadConfig();
        restartRuntime();
        sender.sendMessage("Endcord configuration reloaded from '{}'.", getConfigPath().string());
        return true;
    }

    sender.sendErrorMessage("Usage: /endcord <status|reload>");
    return true;
}

void EndcordPlugin::onPlayerChat(endstone::PlayerChatEvent &event)
{
    if (event.isCancelled()) {
        return;
    }

    forwardChatToDiscord(event.getPlayer(), event.getMessage());
}

void EndcordPlugin::onPlayerJoin(endstone::PlayerJoinEvent &event)
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

void EndcordPlugin::onPlayerQuit(endstone::PlayerQuitEvent &event)
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

void EndcordPlugin::onPlayerDeath(endstone::PlayerDeathEvent &event)
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

void EndcordPlugin::ensureDataFolder() const
{
    std::error_code ec;
    fs::create_directories(getDataFolder(), ec);
    if (ec) {
        getLogger().warning("Could not create plugin data folder '{}': {}", getDataFolder().string(), ec.message());
    }
}

void EndcordPlugin::writeDefaultConfigIfMissing() const
{
    const auto config_path = getConfigPath();
    if (fs::exists(config_path)) {
        return;
    }

    json root = {
        {"config_version", 7},
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
          {"provider", "tabavatars"},
          {"provider_url_template", ""},
          {"provider_prefer_xuid", true},
          {"provider_render_type", "helm"},
          {"provider_bedrock_username_prefix", "."},
          {"size", 64}}},
        {"bot_bridge",
         {{"enabled", true},
          {"shared_secret", "change-me"},
          {"api_route_prefix", "/endcord/api"},
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
          {"log_inbound_chat", false},
          {"log_remote_commands", true}}}
    };

    std::ofstream output(config_path);
    output << root.dump(2) << '\n';
    getLogger().info("Wrote default config to '{}'.", config_path.string());
}

void EndcordPlugin::loadConfig()
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
    config_.avatar.provider = bridge_support::normalizeAvatarProvider(config_.avatar.provider);
    config_.bot_bridge.api_route_prefix =
        normalizeRoutePrefix(config_.bot_bridge.api_route_prefix.empty() ? "/endcord/api"
                                                                         : config_.bot_bridge.api_route_prefix);
    config_.bot_bridge.shared_secret = normalizeSecret(config_.bot_bridge.shared_secret);
    config_.bot_bridge.inbound_chat_max_length = std::clamp(config_.bot_bridge.inbound_chat_max_length, 1, 4000);
    config_.bot_bridge.outbound_system_message_max_batch =
        std::clamp(config_.bot_bridge.outbound_system_message_max_batch, 1, 100);
    config_.bot_bridge.outbound_system_message_queue_max_size =
        std::clamp(config_.bot_bridge.outbound_system_message_queue_max_size, 1, 4096);
    config_.bot_bridge.request_timeout_ms = std::max(config_.bot_bridge.request_timeout_ms, 250);

    if (!config_.avatar.provider_url_template.empty() && config_.avatar.provider_url_template.ends_with('/')) {
        config_.avatar.provider_url_template.pop_back();
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

void EndcordPlugin::restartRuntime()
{
    clearQueue();

    if (!config_.enabled) {
        getLogger().warning("Bridge is disabled in config.");
        return;
    }

    startBridgeServer();
    startWorker();
}

void EndcordPlugin::clearQueue()
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

void EndcordPlugin::startWorker()
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
    webhook_client_->set_default_headers({{"User-Agent", "endcord/0.6.0"}});

    stop_worker_ = false;
    next_request_at_ = std::chrono::steady_clock::now();
    worker_thread_ = std::thread(&EndcordPlugin::workerLoop, this);
    getLogger().info("Webhook worker started.");
}

void EndcordPlugin::stopWorker()
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

void EndcordPlugin::startBridgeServer()
{
    stopBridgeServer();

    if (!config_.enabled || !config_.bot_bridge.enabled) {
        return;
    }

    bridge_server_ = std::make_unique<httplib::Server>();
    bridge_server_->new_task_queue = [] {
        const auto thread_count = kBridgeThreadCount;
        return new httplib::ThreadPool(thread_count);
    };

    installBotBridgeRoutes();

    bridge_server_->set_logger([this](const auto &req, const auto &res) {
        if (config_.logging.log_http_requests || res.status >= 400) {
            getLogger().debug("Bridge HTTP {} {} -> {}", req.method, req.path, res.status);
        }
    });

    const auto host = std::string(kBridgeBindHost);
    const auto port = kBridgePort;
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

void EndcordPlugin::stopBridgeServer()
{
    if (bridge_server_) {
        bridge_server_->stop();
    }

    if (bridge_server_thread_.joinable()) {
        bridge_server_thread_.join();
    }

    bridge_server_.reset();
}

void EndcordPlugin::workerLoop()
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

void EndcordPlugin::forwardChatToDiscord(const endstone::Player &player, const std::string &message)
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
    const auto replacements = makePlayerTemplateReplacements(player, message, "chat", getServer().getName());
    auto username = bridge_support::applyTemplate(config_.discord.username_template, replacements);
    auto content = bridge_support::applyTemplate(config_.discord.content_template, replacements);

    enqueueDiscordMessage(player.getName(), std::move(username), std::move(content), avatar_url);
}

void EndcordPlugin::forwardLifecycleEventToDiscord(const endstone::Player &player, const std::string &event_name,
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

    const auto replacements = makePlayerTemplateReplacements(player, event_message, event_name, getServer().getName());
    auto username = bridge_support::applyTemplate(config_.discord.system_username_template, replacements);
    auto content = bridge_support::applyTemplate(content_template, replacements);

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

void EndcordPlugin::enqueueDiscordMessage(const std::string &source_name, std::string username, std::string content,
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

void EndcordPlugin::enqueueBotSystemMessage(std::string event_name, std::string player_name, std::string content)
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

void EndcordPlugin::sendStatus(endstone::CommandSender &sender) const
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
    sender.sendMessage("Avatar support enabled: {}", config_.avatar.enabled ? "yes" : "no");
    sender.sendMessage("Avatar provider: {}", config_.avatar.provider);
    sender.sendMessage("Bridge HTTP server running: {}", (bridge_server_ && bridge_server_->is_running()) ? "yes" : "no");
    sender.sendMessage("Bot bridge enabled: {}", config_.bot_bridge.enabled ? "yes" : "no");
    sender.sendMessage("Bot bridge API prefix: {}", config_.bot_bridge.api_route_prefix);
}

void EndcordPlugin::enqueueWebhookPayload(WebhookJob job)
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

void EndcordPlugin::processWebhookJob(WebhookJob job)
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

void EndcordPlugin::installBotBridgeRoutes()
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

void EndcordPlugin::handleBotBridgeChat(const httplib::Request &req, httplib::Response &res)
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
    const auto formatted = bridge_support::applyTemplate(
        config_.bot_bridge.inbound_chat_template,
        makeInboundChatTemplateReplacements(author, content, channel, guild, message_url, getServer().getName()));
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

void EndcordPlugin::handleBotBridgeCommand(const httplib::Request &req, httplib::Response &res)
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

void EndcordPlugin::handleBotBridgeStatus(const httplib::Request &req, httplib::Response &res)
{
    if (!isAuthorizedBotBridgeRequest(req, res)) {
        return;
    }

    json online_player_names = json::array();
    const auto online_players = getServer().getOnlinePlayers();
    for (const auto *player : online_players) {
        if (player != nullptr) {
            online_player_names.push_back(player->getName());
        }
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
                          {"online_players", online_players.size()},
                          {"online_player_names", online_player_names},
                          {"webhook_configured", webhook_target_.has_value()},
                          {"runtime_webhook_override_active", runtime_webhook_override_active_},
                          {"webhook_queue_depth", queue_depth},
                          {"system_message_queue_depth", system_message_queue_depth},
                          {"system_message_queue_max", config_.bot_bridge.outbound_system_message_queue_max_size},
                          {"minecraft_to_discord_enabled", config_.relay.minecraft_to_discord_enabled},
                          {"discord_to_minecraft_enabled", config_.bot_bridge.inbound_chat_enabled},
                          {"bot_system_messages_enabled", config_.bot_bridge.outbound_system_messages_enabled},
                          {"avatar_enabled", config_.avatar.enabled},
                          {"avatar_provider", config_.avatar.provider},
                          {"bot_bridge_enabled", config_.bot_bridge.enabled}})
                        .dump(),
                    "application/json");
}

void EndcordPlugin::handleBotBridgeDrainSystemMessages(const httplib::Request &req, httplib::Response &res)
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

void EndcordPlugin::handleBotBridgeConfigureWebhook(const httplib::Request &req, httplib::Response &res)
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

void EndcordPlugin::handleBotBridgeHealth(const httplib::Request &req, httplib::Response &res)
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
                          {"bridge_server_running", bridge_server_ && bridge_server_->is_running()},
                          {"webhook_worker_running", worker_thread_.joinable()},
                          {"webhook_configured", webhook_target_.has_value()},
                          {"webhook_queue_depth", queue_depth}})
                        .dump(),
                    "application/json");
}

bool EndcordPlugin::isAuthorizedBotBridgeRequest(const httplib::Request &req, httplib::Response &res) const
{
    if (!config_.bot_bridge.enabled) {
        res.status = 403;
        res.set_content(R"({"ok":false,"error":"bot bridge disabled"})", "application/json");
        return false;
    }

    const bool remote_is_loopback = isLoopbackAddress(req.remote_addr);
    const bool remote_is_allowlisted =
        bridge_support::isAllowedRemoteAddress(req.remote_addr, config_.bot_bridge.allowed_remote_addresses);

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

bool EndcordPlugin::isAuthorizedBotBridgeHealthRequest(const httplib::Request &req) const
{
    return isLoopbackAddress(req.remote_addr) ||
           bridge_support::isAllowedRemoteAddress(req.remote_addr, config_.bot_bridge.allowed_remote_addresses);
}

void EndcordPlugin::loadWebhookState()
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

void EndcordPlugin::persistWebhookState() const
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

void EndcordPlugin::clearWebhookState() const
{
    std::error_code ec;
    fs::remove(getWebhookStatePath(), ec);
    if (ec && ec.value() != 2) {
        getLogger().warning("Could not clear runtime webhook state '{}': {}", getWebhookStatePath().string(),
                            ec.message());
    }
}

std::optional<std::string> EndcordPlugin::getOrCreateAvatarUrl(const endstone::Player &player)
{
    if (!config_.avatar.enabled) {
        return std::nullopt;
    }
    return buildProviderAvatarUrl(player);
}

std::optional<std::string> EndcordPlugin::buildProviderAvatarUrl(const endstone::Player &player) const
{
    const auto xuid = player.getXuid();
    const auto username = player.getName();
    const auto skin = player.getSkin();
    const auto uuid = player.getUniqueId().str();

    if (!config_.avatar.provider_url_template.empty()) {
        return bridge_support::buildProviderAvatarUrl(
            {.provider = config_.avatar.provider,
             .provider_url_template = config_.avatar.provider_url_template,
             .provider_prefer_xuid = config_.avatar.provider_prefer_xuid,
             .provider_render_type = config_.avatar.provider_render_type,
             .provider_bedrock_username_prefix = config_.avatar.provider_bedrock_username_prefix,
             .size = config_.avatar.size},
            {.username = username, .xuid = xuid, .uuid = uuid, .skin_id = skin.getId()});
    }

    return bridge_support::buildProviderAvatarUrl(
        {.provider = config_.avatar.provider,
         .provider_url_template = config_.avatar.provider_url_template,
         .provider_prefer_xuid = config_.avatar.provider_prefer_xuid,
         .provider_render_type = config_.avatar.provider_render_type,
         .provider_bedrock_username_prefix = config_.avatar.provider_bedrock_username_prefix,
         .size = config_.avatar.size},
        {.username = username, .xuid = xuid, .uuid = uuid, .skin_id = skin.getId()});
}

std::optional<EndcordPlugin::WebhookTarget>
EndcordPlugin::parseWebhookUrl(const std::string &url)
{
    static const std::regex pattern(R"(^(https?)://([^/\s]+)(/.*)$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_match(url, match, pattern)) {
        return std::nullopt;
    }

    return WebhookTarget{match[1].str() + "://" + match[2].str(), match[3].str()};
}

std::optional<std::int64_t> EndcordPlugin::parseRetryDelayMs(const std::string &value)
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

std::optional<std::int64_t> EndcordPlugin::parseRetryDelayMsFromBody(const std::string &body)
{
    static const std::regex pattern(R"("retry_after"\s*:\s*([0-9]+(?:\.[0-9]+)?))");
    std::smatch match;
    if (!std::regex_search(body, match, pattern)) {
        return std::nullopt;
    }

    return parseRetryDelayMs(match[1].str());
}

std::string EndcordPlugin::normalizeRoutePrefix(const std::string &value)
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

std::string EndcordPlugin::normalizeSecret(std::string value)
{
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(),
                value.end());
    return value;
}

bool EndcordPlugin::isLoopbackAddress(const std::string &host)
{
    return host == "127.0.0.1" || host == "::1" || host == "::ffff:127.0.0.1" || host == "localhost";
}

std::filesystem::path EndcordPlugin::getConfigPath() const
{
    return getDataFolder() / "config.json";
}

std::filesystem::path EndcordPlugin::getWebhookStatePath() const
{
    return getDataFolder() / "runtime_webhook.json";
}

std::string EndcordPlugin::messageToPlainText(const endstone::Message &message)
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

std::string EndcordPlugin::truncateUtf8Bytes(const std::string &value, std::size_t max_bytes)
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

std::string EndcordPlugin::escapeJson(const std::string &value)
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
