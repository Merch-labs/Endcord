#pragma once

#include <endstone/endstone.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace httplib {
struct Request;
struct Response;
class Server;
class Client;
}

class EndcordPlugin : public endstone::Plugin {
public:
    ~EndcordPlugin() override;

    void onLoad() override;
    void onEnable() override;
    void onDisable() override;
    bool onCommand(endstone::CommandSender &sender, const endstone::Command &command,
                   const std::vector<std::string> &args) override;

    void onPlayerChat(endstone::PlayerChatEvent &event);
    void onPlayerJoin(endstone::PlayerJoinEvent &event);
    void onPlayerQuit(endstone::PlayerQuitEvent &event);
    void onPlayerDeath(endstone::PlayerDeathEvent &event);

private:
    struct DiscordOptions {
        std::string webhook_url;
        bool allow_runtime_webhook_override = true;
        std::string username_template = "{player}";
        std::string content_template = "{message}";
        std::string system_username_template = "{server}";
        std::string join_content_template = ":inbox_tray: **{player}** joined the server.";
        std::string quit_content_template = ":outbox_tray: **{player}** left the server.";
        std::string death_content_template = ":skull: {event_message}";
        bool allow_mentions = false;
        bool use_player_avatar_for_system_messages = true;
        int max_username_length = 80;
        int max_content_length = 2000;
    };

    struct RelayOptions {
        bool minecraft_to_discord_enabled = true;
        bool chat_enabled = true;
        bool join_enabled = true;
        bool quit_enabled = true;
        bool death_enabled = true;
    };

    struct QueueOptions {
        int max_size = 256;
        int max_attempts = 5;
        int retry_delay_ms = 2000;
        int connect_timeout_ms = 3000;
        int read_timeout_ms = 5000;
        int write_timeout_ms = 5000;
    };

    struct AvatarOptions {
        bool enabled = true;
        std::string provider = "tabavatars";
        std::string provider_url_template;
        bool provider_prefer_xuid = true;
        std::string provider_render_type = "helm";
        std::string provider_bedrock_username_prefix = ".";
        int size = 64;
    };

    struct BotBridgeOptions {
        bool enabled = true;
        std::string shared_secret;
        std::string api_route_prefix = "/endcord/api";
        bool allow_local_requests_only = true;
        std::vector<std::string> allowed_remote_addresses{};
        bool inbound_chat_enabled = true;
        bool command_enabled = true;
        bool outbound_system_messages_enabled = false;
        std::string inbound_chat_template = "[Discord] #{channel} <{author}> {content}";
        int inbound_chat_max_length = 2000;
        int outbound_system_message_max_batch = 20;
        int outbound_system_message_queue_max_size = 256;
        int request_timeout_ms = 5000;
    };

    struct LoggingOptions {
        bool log_filtered_events = false;
        bool log_webhook_successes = false;
        bool log_http_requests = false;
        bool log_inbound_chat = false;
        bool log_remote_commands = true;
    };

    struct ManagedBotOptions {
        bool enabled = true;
        std::string executable_path = "{plugin_data}/bot/.venv/bin/endcord-bot";
        std::string config_path = "{plugin_data}/bot/config.json";
        std::string working_directory = "{plugin_data}/bot";
        std::string log_path = "{plugin_data}/bot/bot.log";
        int stop_timeout_ms = 5000;
    };

    struct BridgeConfig {
        int config_version = 8;
        bool enabled = true;
        DiscordOptions discord{};
        RelayOptions relay{};
        QueueOptions queue{};
        AvatarOptions avatar{};
        BotBridgeOptions bot_bridge{};
        LoggingOptions logging{};
        ManagedBotOptions managed_bot{};
    };

    struct WebhookTarget {
        std::string origin;
        std::string path;
    };

    struct WebhookJob {
        std::string player_name;
        std::string payload;
        int attempt = 0;
    };

    struct PendingSystemMessage {
        std::string event_name;
        std::string player_name;
        std::string content;
    };

    void ensureDataFolder() const;
    void writeDefaultConfigIfMissing() const;
    void writeDefaultBotConfigIfMissing() const;
    void loadConfig();
    void restartRuntime();
    void clearQueue();
    void startWorker();
    void stopWorker();
    void startBridgeServer();
    void stopBridgeServer();
    void startManagedBot();
    void stopManagedBot();
    void workerLoop();
    void sendStatus(endstone::CommandSender &sender) const;
    void forwardChatToDiscord(const endstone::Player &player, const std::string &message);
    void forwardLifecycleEventToDiscord(const endstone::Player &player, const std::string &event_name,
                                        const std::string &content_template, const std::string &event_message);
    void enqueueDiscordMessage(const std::string &source_name, std::string username, std::string content,
                               const std::optional<std::string> &avatar_url);
    void enqueueBotSystemMessage(std::string event_name, std::string player_name, std::string content);
    void enqueueWebhookPayload(WebhookJob job);
    void processWebhookJob(WebhookJob job);
    void installBotBridgeRoutes();
    void handleBotBridgeChat(const httplib::Request &req, httplib::Response &res);
    void handleBotBridgeCommand(const httplib::Request &req, httplib::Response &res);
    void handleBotBridgeDrainSystemMessages(const httplib::Request &req, httplib::Response &res);
    void handleBotBridgeConfigureWebhook(const httplib::Request &req, httplib::Response &res);
    void handleBotBridgeHealth(const httplib::Request &req, httplib::Response &res);
    void handleBotBridgeStatus(const httplib::Request &req, httplib::Response &res);
    bool isAuthorizedBotBridgeRequest(const httplib::Request &req, httplib::Response &res) const;
    bool isAuthorizedBotBridgeHealthRequest(const httplib::Request &req) const;
    void loadWebhookState();
    void persistWebhookState() const;
    void clearWebhookState() const;

    std::optional<std::string> getOrCreateAvatarUrl(const endstone::Player &player);
    std::optional<std::string> buildProviderAvatarUrl(const endstone::Player &player) const;
    static std::optional<WebhookTarget> parseWebhookUrl(const std::string &url);
    static std::optional<std::int64_t> parseRetryDelayMs(const std::string &value);
    static std::optional<std::int64_t> parseRetryDelayMsFromBody(const std::string &body);
    static std::string normalizeRoutePrefix(const std::string &value);
    static std::string normalizeSecret(std::string value);
    static bool isLoopbackAddress(const std::string &host);
    static std::string messageToPlainText(const endstone::Message &message);
    static std::string truncateUtf8Bytes(const std::string &value, std::size_t max_bytes);
    std::filesystem::path expandManagedBotPath(const std::string &value) const;
    bool isManagedBotRunning() const;

    std::filesystem::path getConfigPath() const;
    std::filesystem::path getBotConfigPath() const;
    std::filesystem::path getWebhookStatePath() const;

    static std::string escapeJson(const std::string &value);

    BridgeConfig config_;
    std::optional<WebhookTarget> webhook_target_;
    bool runtime_webhook_override_active_ = false;

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<WebhookJob> webhook_queue_;
    std::thread worker_thread_;
    std::unique_ptr<httplib::Client> webhook_client_;
    std::unique_ptr<httplib::Server> bridge_server_;
    std::thread bridge_server_thread_;
    bool stop_worker_ = false;
    std::chrono::steady_clock::time_point next_request_at_ = std::chrono::steady_clock::now();
    mutable std::mutex system_message_mutex_;
    std::deque<PendingSystemMessage> pending_system_messages_;
    int managed_bot_pid_ = -1;
};
