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
}

class BedrockDiscordBridgePlugin : public endstone::Plugin {
public:
    ~BedrockDiscordBridgePlugin() override;

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

    struct AvatarHttpServerOptions {
        bool enabled = true;
        std::string bind_host = "0.0.0.0";
        int port = 8089;
        std::string route_prefix = "/bedrock-discord-bridge/avatars";
        std::string public_base_url;
        std::string cache_control = "public, max-age=86400";
        int thread_count = 4;
    };

    struct AvatarOptions {
        bool enabled = true;
        int size = 64;
        std::string cache_subdirectory = "avatars";
        std::string public_base_url;
        AvatarHttpServerOptions http_server{};
    };

    struct BotBridgeOptions {
        bool enabled = true;
        std::string shared_secret;
        std::string api_route_prefix = "/bedrock-discord-bridge/api";
        bool allow_local_requests_only = true;
        bool inbound_chat_enabled = true;
        bool command_enabled = true;
        std::string inbound_chat_template = "[Discord] #{channel} <{author}> {content}";
        int inbound_chat_max_length = 2000;
        int request_timeout_ms = 5000;
    };

    struct LoggingOptions {
        bool log_filtered_events = false;
        bool log_webhook_successes = false;
        bool log_http_requests = false;
        bool log_avatar_cache_hits = false;
        bool log_avatar_cache_misses = false;
        bool log_inbound_chat = false;
        bool log_remote_commands = true;
    };

    struct BridgeConfig {
        int config_version = 2;
        bool enabled = true;
        DiscordOptions discord{};
        RelayOptions relay{};
        QueueOptions queue{};
        AvatarOptions avatar{};
        BotBridgeOptions bot_bridge{};
        LoggingOptions logging{};
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

    struct AvatarCacheEntry {
        std::filesystem::path file_path;
        std::optional<std::string> public_url;
    };

    void ensureDataFolder() const;
    void writeDefaultConfigIfMissing() const;
    void loadConfig();
    void restartRuntime();
    void clearQueue();
    void startWorker();
    void stopWorker();
    void startAvatarServer();
    void stopAvatarServer();
    void workerLoop();
    void sendStatus(endstone::CommandSender &sender) const;
    void forwardChatToDiscord(const endstone::Player &player, const std::string &message);
    void forwardLifecycleEventToDiscord(const endstone::Player &player, const std::string &event_name,
                                        const std::string &content_template, const std::string &event_message);
    void enqueueDiscordMessage(const std::string &source_name, std::string username, std::string content,
                               const std::optional<std::string> &avatar_url);
    void enqueueWebhookPayload(WebhookJob job);
    void processWebhookJob(WebhookJob job);
    void installBotBridgeRoutes();
    void handleBotBridgeChat(const httplib::Request &req, httplib::Response &res);
    void handleBotBridgeCommand(const httplib::Request &req, httplib::Response &res);
    void handleBotBridgeStatus(const httplib::Request &req, httplib::Response &res);
    bool isAuthorizedBotBridgeRequest(const httplib::Request &req, httplib::Response &res) const;

    std::optional<std::string> getOrCreateAvatarUrl(const endstone::Player &player);
    std::optional<AvatarCacheEntry> renderAvatarIfNeeded(const endstone::Player &player);
    bool writeHeadPng(const endstone::Skin &skin, const std::filesystem::path &output_path) const;
    static std::vector<std::uint8_t> renderHeadRgba(const endstone::Image &image, int avatar_size);
    static endstone::Color sampleSkinCell(const endstone::Image &image, int origin_x, int origin_y, int scale);
    static endstone::Color alphaBlend(endstone::Color base, endstone::Color overlay);
    static std::string computeSkinCacheKey(const endstone::Skin &skin, int avatar_size);
    static std::optional<WebhookTarget> parseWebhookUrl(const std::string &url);
    static std::optional<std::int64_t> parseRetryDelayMs(const std::string &value);
    static std::optional<std::int64_t> parseRetryDelayMsFromBody(const std::string &body);
    static std::string applyTemplate(std::string value,
                                     const std::vector<std::pair<std::string, std::string>> &replacements);
    static std::string replaceAll(std::string value, const std::string &needle, const std::string &replacement);
    static std::string normalizeRoutePrefix(const std::string &value);
    static std::string normalizeSecret(std::string value);
    static bool isWildcardHost(const std::string &host);
    static bool isLoopbackAddress(const std::string &host);
    static std::string joinUrl(const std::string &base, const std::string &leaf);
    static std::string messageToPlainText(const endstone::Message &message);
    static std::string truncateUtf8Bytes(const std::string &value, std::size_t max_bytes);

    std::filesystem::path getConfigPath() const;
    std::filesystem::path getAvatarCacheDir() const;
    std::optional<std::string> getEffectiveAvatarBaseUrl() const;

    static std::string escapeJson(const std::string &value);

    BridgeConfig config_;
    std::optional<WebhookTarget> webhook_target_;
    std::unordered_map<std::string, AvatarCacheEntry> avatar_cache_;

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<WebhookJob> webhook_queue_;
    std::thread worker_thread_;
    std::unique_ptr<httplib::Server> avatar_server_;
    std::thread avatar_server_thread_;
    bool stop_worker_ = false;
    std::chrono::steady_clock::time_point next_request_at_ = std::chrono::steady_clock::now();
};
