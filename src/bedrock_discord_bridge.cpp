#include "bedrock_discord_bridge.h"

#include <httplib.h>
#include <lodepng.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

ENDSTONE_PLUGIN(/*name=*/"bedrock_discord_bridge",
                /*version=*/"0.2.0",
                /*main_class=*/BedrockDiscordBridgePlugin)
{
    prefix = "BedrockDiscordBridge";
    description = "Endstone C++ Bedrock-to-Discord bridge with webhook queue and skin-head avatar cache.";
    authors = {"Bedrock Discord Bridge contributors"};
}

BedrockDiscordBridgePlugin::~BedrockDiscordBridgePlugin()
{
    stopWorker();
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
    startWorker();

    getLogger().info("Plugin enabled. Chat forwarding pipeline is active.");
}

void BedrockDiscordBridgePlugin::onDisable()
{
    stopWorker();
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

    fs::create_directories(getDataFolder() / "avatars", ec);
    if (ec) {
        getLogger().warning("Could not create avatar cache folder '{}': {}", (getDataFolder() / "avatars").string(),
                            ec.message());
    }
}

void BedrockDiscordBridgePlugin::loadDiscordSettings()
{
    settings_ = {};
    webhook_target_.reset();

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
        else if (key == "DISCORD_ENABLED") {
            settings_.enabled = parseBool(value, settings_.enabled);
        }
        else if (key == "AVATAR_PUBLIC_BASE_URL") {
            settings_.avatar_public_base_url = value;
        }
        else if (key == "WEBHOOK_CONNECT_TIMEOUT_MS") {
            settings_.connect_timeout_ms = parseInt(value, settings_.connect_timeout_ms);
        }
        else if (key == "WEBHOOK_READ_TIMEOUT_MS") {
            settings_.read_timeout_ms = parseInt(value, settings_.read_timeout_ms);
        }
        else if (key == "WEBHOOK_WRITE_TIMEOUT_MS") {
            settings_.write_timeout_ms = parseInt(value, settings_.write_timeout_ms);
        }
        else if (key == "MAX_QUEUE_SIZE") {
            settings_.max_queue_size = parseInt(value, settings_.max_queue_size);
        }
        else if (key == "AVATAR_SIZE") {
            settings_.avatar_size = parseInt(value, settings_.avatar_size);
        }
    }

    settings_.connect_timeout_ms = std::max(settings_.connect_timeout_ms, 250);
    settings_.read_timeout_ms = std::max(settings_.read_timeout_ms, 250);
    settings_.write_timeout_ms = std::max(settings_.write_timeout_ms, 250);
    settings_.max_queue_size = std::max(settings_.max_queue_size, 1);
    settings_.avatar_size = std::clamp(settings_.avatar_size, 8, 512);

    if (!settings_.avatar_public_base_url.empty() && settings_.avatar_public_base_url.ends_with('/')) {
        settings_.avatar_public_base_url.pop_back();
    }

    if (!settings_.enabled) {
        getLogger().warning("Discord bridge is disabled by config.");
        return;
    }

    if (settings_.webhook_url.empty()) {
        getLogger().warning("DISCORD_WEBHOOK_URL is empty in '{}'.", config_path.string());
    }
    else {
        webhook_target_ = parseWebhookUrl(settings_.webhook_url);
        if (!webhook_target_) {
            getLogger().error("DISCORD_WEBHOOK_URL is invalid and could not be parsed.");
        }
    }

    getLogger().info("Loaded Discord configuration. Avatar public base URL is {}configured.",
                     settings_.avatar_public_base_url.empty() ? "not " : "");
}

void BedrockDiscordBridgePlugin::startWorker()
{
    stopWorker();

    if (!settings_.enabled) {
        return;
    }

    if (!webhook_target_) {
        getLogger().warning("Webhook worker not started because DISCORD_WEBHOOK_URL is missing or invalid.");
        return;
    }

    stop_worker_ = false;
    worker_running_ = true;
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

    worker_running_ = false;
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
    if (!settings_.enabled) {
        return;
    }

    const auto avatar_url = getOrCreateAvatarUrl(player);

    std::ostringstream payload;
    payload << "{"
            << "\"username\":\"" << escapeJson(player.getName()) << "\","
            << "\"content\":\"" << escapeJson(message) << "\"";
    if (avatar_url.has_value()) {
        payload << ",\"avatar_url\":\"" << escapeJson(*avatar_url) << "\"";
    }
    payload << "}";

    if (!webhook_target_) {
        getLogger().debug("Skipping Discord webhook POST because DISCORD_WEBHOOK_URL is not configured.");
        return;
    }

    enqueueWebhookPayload({player.getName(), payload.str(), 0});
}

void BedrockDiscordBridgePlugin::enqueueWebhookPayload(WebhookJob job)
{
    std::lock_guard lock(queue_mutex_);

    if (static_cast<int>(webhook_queue_.size()) >= settings_.max_queue_size) {
        getLogger().warning("Dropping Discord webhook payload for '{}' because the queue is full ({} items).",
                            job.player_name, settings_.max_queue_size);
        return;
    }

    webhook_queue_.push_back(std::move(job));
    queue_cv_.notify_one();
}

void BedrockDiscordBridgePlugin::processWebhookJob(WebhookJob job)
{
    if (!webhook_target_) {
        return;
    }

    httplib::Client client(webhook_target_->origin);
    client.set_connection_timeout(std::chrono::milliseconds(settings_.connect_timeout_ms));
    client.set_read_timeout(std::chrono::milliseconds(settings_.read_timeout_ms));
    client.set_write_timeout(std::chrono::milliseconds(settings_.write_timeout_ms));
    client.set_follow_location(true);
    client.set_default_headers({{"User-Agent", "bedrock_discord_bridge/0.2.0"}});

    auto result = client.Post(webhook_target_->path.c_str(), job.payload, "application/json");
    if (!result) {
        getLogger().warning("Discord webhook POST failed for '{}': {}", job.player_name,
                            httplib::to_string(result.error()));

        if (job.attempt < 3) {
            job.attempt++;
            {
                std::lock_guard lock(queue_mutex_);
                next_request_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                webhook_queue_.push_front(std::move(job));
            }
            queue_cv_.notify_one();
        }
        return;
    }

    if (result->status == 200 || result->status == 204) {
        if (result->has_header("X-RateLimit-Remaining") && result->get_header_value("X-RateLimit-Remaining") == "0") {
            if (const auto reset_after = parseRetryDelayMs(result->get_header_value("X-RateLimit-Reset-After"))) {
                std::lock_guard lock(queue_mutex_);
                next_request_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(*reset_after);
            }
        }
        return;
    }

    if (result->status == 429 || result->status >= 500) {
        std::int64_t delay_ms = 2000;
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
        if (job.attempt < 5) {
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

std::optional<std::string> BedrockDiscordBridgePlugin::getOrCreateAvatarUrl(const endstone::Player &player)
{
    const auto skin = player.getSkin();
    const auto skin_key = computeSkinCacheKey(skin);

    if (const auto it = avatar_cache_.find(skin_key); it != avatar_cache_.end()) {
        return it->second.public_url;
    }

    if (const auto entry = renderAvatarIfNeeded(player)) {
        avatar_cache_.emplace(skin_key, *entry);
        return entry->public_url;
    }

    return std::nullopt;
}

std::optional<BedrockDiscordBridgePlugin::AvatarCacheEntry>
BedrockDiscordBridgePlugin::renderAvatarIfNeeded(const endstone::Player &player)
{
    const auto skin = player.getSkin();
    const auto skin_key = computeSkinCacheKey(skin);
    const auto avatar_file_name = skin_key + ".png";
    const auto avatar_path = getDataFolder() / "avatars" / avatar_file_name;

    if (!fs::exists(avatar_path) && !writeHeadPng(skin, avatar_path)) {
        return std::nullopt;
    }

    AvatarCacheEntry entry{avatar_path, std::nullopt};
    if (!settings_.avatar_public_base_url.empty()) {
        entry.public_url = joinUrl(settings_.avatar_public_base_url, avatar_file_name);
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

    const auto rgba = renderHeadRgba(skin.getImage(), settings_.avatar_size);
    if (rgba.empty()) {
        getLogger().warning("Could not render a Discord avatar from skin '{}': unsupported skin dimensions {}x{}.",
                            skin.getId(), skin.getImage().getWidth(), skin.getImage().getHeight());
        return false;
    }

    const auto error = lodepng::encode(output_path.string(), rgba,
                                       static_cast<unsigned>(settings_.avatar_size),
                                       static_cast<unsigned>(settings_.avatar_size));
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

std::string BedrockDiscordBridgePlugin::computeSkinCacheKey(const endstone::Skin &skin)
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

    const auto &image = skin.getImage();
    mix(static_cast<unsigned char>(image.getWidth() & 0xff));
    mix(static_cast<unsigned char>((image.getWidth() >> 8) & 0xff));
    mix(static_cast<unsigned char>(image.getHeight() & 0xff));
    mix(static_cast<unsigned char>((image.getHeight() >> 8) & 0xff));

    for (const auto ch : image.getData()) {
        mix(static_cast<unsigned char>(ch));
    }

    std::ostringstream stream;
    stream << std::hex << hash;
    return stream.str();
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

std::string BedrockDiscordBridgePlugin::trim(const std::string &value)
{
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }

    const auto end = value.find_last_not_of(" \t\r\n");
    auto trimmed = value.substr(start, end - start + 1);
    if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"') {
        trimmed = trimmed.substr(1, trimmed.size() - 2);
    }
    return trimmed;
}

bool BedrockDiscordBridgePlugin::parseBool(const std::string &value, bool fallback)
{
    std::string normalized;
    normalized.reserve(value.size());
    for (const auto ch : value) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on") {
        return true;
    }
    if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off") {
        return false;
    }
    return fallback;
}

int BedrockDiscordBridgePlugin::parseInt(const std::string &value, int fallback)
{
    try {
        return std::stoi(value);
    }
    catch (...) {
        return fallback;
    }
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
