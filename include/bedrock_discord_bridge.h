#pragma once

#include <endstone/endstone.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class BedrockDiscordBridgePlugin : public endstone::Plugin {
public:
    ~BedrockDiscordBridgePlugin() override;

    void onLoad() override;
    void onEnable() override;
    void onDisable() override;

    void onPlayerChat(endstone::PlayerChatEvent &event);

private:
    struct DiscordSettings {
        bool enabled = true;
        std::string webhook_url;
        std::string avatar_public_base_url;
        int connect_timeout_ms = 3000;
        int read_timeout_ms = 5000;
        int write_timeout_ms = 5000;
        int max_queue_size = 256;
        int avatar_size = 64;
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
    void loadDiscordSettings();
    void startWorker();
    void stopWorker();
    void workerLoop();
    void forwardChatToDiscord(const endstone::Player &player, const std::string &message);
    void enqueueWebhookPayload(WebhookJob job);
    void processWebhookJob(WebhookJob job);

    std::optional<std::string> getOrCreateAvatarUrl(const endstone::Player &player);
    std::optional<AvatarCacheEntry> renderAvatarIfNeeded(const endstone::Player &player);
    bool writeHeadPng(const endstone::Skin &skin, const std::filesystem::path &output_path) const;
    static std::vector<std::uint8_t> renderHeadRgba(const endstone::Image &image, int avatar_size);
    static endstone::Color sampleSkinCell(const endstone::Image &image, int origin_x, int origin_y, int scale);
    static endstone::Color alphaBlend(endstone::Color base, endstone::Color overlay);
    static std::string computeSkinCacheKey(const endstone::Skin &skin);
    static std::optional<WebhookTarget> parseWebhookUrl(const std::string &url);
    static std::optional<std::int64_t> parseRetryDelayMs(const std::string &value);
    static std::optional<std::int64_t> parseRetryDelayMsFromBody(const std::string &body);
    static std::string joinUrl(const std::string &base, const std::string &leaf);

    static std::string trim(const std::string &value);
    static bool parseBool(const std::string &value, bool fallback);
    static int parseInt(const std::string &value, int fallback);
    static std::string escapeJson(const std::string &value);

    DiscordSettings settings_;
    std::optional<WebhookTarget> webhook_target_;
    std::unordered_map<std::string, AvatarCacheEntry> avatar_cache_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<WebhookJob> webhook_queue_;
    std::thread worker_thread_;
    bool stop_worker_ = false;
    bool worker_running_ = false;
    std::chrono::steady_clock::time_point next_request_at_ = std::chrono::steady_clock::now();
};
