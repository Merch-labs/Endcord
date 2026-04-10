#include "endcord_integrated_bot.h"

#include "bridge_support.h"

#include <dpp/dpp.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace endcord {
namespace {

// Safe limit for slash-command replies — leaves headroom for Discord's own
// formatting overhead so the response is never silently truncated by the API.
static constexpr std::size_t kDiscordSafeReplyLength = 1800;

using json = nlohmann::json;
using ReplacementList = bridge_support::ReplacementList;

std::string boolText(bool value)
{
    return value ? "true" : "false";
}

std::string joinStrings(const std::vector<std::string> &values, const std::string &separator)
{
    std::ostringstream stream;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            stream << separator;
        }
        stream << values[index];
    }
    return stream.str();
}

std::string joinLines(const std::vector<std::string> &lines)
{
    return joinStrings(lines, "\n");
}

// Truncates to at most max_bytes bytes at a UTF-8 boundary, appending "..."
// when the string is shortened.
std::string truncateText(const std::string &value, std::size_t max_bytes)
{
    if (value.size() <= max_bytes) {
        return value;
    }
    if (max_bytes <= 3) {
        return bridge_support::truncateUtf8Bytes(value, max_bytes);
    }
    return bridge_support::truncateUtf8Bytes(value, max_bytes - 3) + "...";
}

std::string firstNonEmpty(std::initializer_list<std::string> values)
{
    for (const auto &value : values) {
        if (!value.empty()) {
            return value;
        }
    }
    return {};
}

std::string toUpper(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return value;
}

std::string toTitle(std::string value)
{
    bool capitalize_next = true;
    for (char &ch : value) {
        const auto uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) != 0) {
            ch = static_cast<char>(capitalize_next ? std::toupper(uch) : std::tolower(uch));
            capitalize_next = false;
        }
        else {
            capitalize_next = true;
        }
    }
    return value;
}

std::string getDisplayName(const dpp::message &message)
{
    auto name = firstNonEmpty({message.member.get_nickname(), message.author.global_name, message.author.username});
    return name.empty() ? "Unknown User" : name;
}

std::string getDisplayName(const dpp::guild_member &member, const dpp::user &user)
{
    return firstNonEmpty({member.get_nickname(), user.global_name, user.username});
}

ReplacementList buildRelayReplacements(const dpp::message &message, const std::string &content,
                                       const std::string &attachments, const std::string &jump_url)
{
    const auto channel_text = std::to_string(message.channel_id);
    const auto guild_text = std::to_string(message.guild_id);

    return {
        {"{author}", getDisplayName(message)},
        {"{author_name}", getDisplayName(message)},
        {"{author_username}", message.author.username},
        {"{author_global_name}", message.author.global_name},
        {"{author_id}", std::to_string(message.author.id)},
        {"{content}", content},
        {"{content_raw}", message.content},
        {"{content_length}", std::to_string(content.size())},
        {"{attachments}", attachments},
        {"{attachments_count}", std::to_string(message.attachments.size())},
        {"{jump_url}", jump_url},
        {"{channel}", channel_text},
        {"{channel_name}", channel_text},
        {"{channel_id}", channel_text},
        {"{channel_mention}", dpp::channel::get_mention(message.channel_id)},
        {"{guild}", guild_text},
        {"{guild_name}", guild_text},
        {"{guild_id}", guild_text},
        {"{message_url}", message.get_url()},
        {"{message_id}", std::to_string(message.id)},
    };
}

dpp::presence_status toPresenceStatus(const std::string &value)
{
    if (value == "idle") {
        return dpp::ps_idle;
    }
    if (value == "dnd") {
        return dpp::ps_dnd;
    }
    if (value == "invisible") {
        return dpp::ps_invisible;
    }
    return dpp::ps_online;
}

dpp::activity buildActivity(const BotConfig &config, const std::string &activity_text)
{
    if (config.presence.activity_type == "streaming") {
        return dpp::activity(dpp::at_streaming, activity_text, "", config.presence.streaming_url);
    }
    if (config.presence.activity_type == "listening") {
        return dpp::activity(dpp::at_listening, activity_text, "", "");
    }
    if (config.presence.activity_type == "watching") {
        return dpp::activity(dpp::at_watching, activity_text, "", "");
    }
    if (config.presence.activity_type == "competing") {
        return dpp::activity(dpp::at_competing, activity_text, "", "");
    }
    if (config.presence.activity_type == "custom") {
        return dpp::activity(dpp::at_custom, activity_text, "", "");
    }
    return dpp::activity(dpp::at_game, activity_text, "", "");
}

dpp::message makeReplyMessage(const std::string &text, bool ephemeral)
{
    dpp::message message(text);
    if (ephemeral) {
        message.set_flags(dpp::m_ephemeral);
    }
    return message;
}

std::runtime_error makeBridgeError(const json &payload, const std::string &fallback)
{
    return std::runtime_error(payload.value("error", fallback));
}

}  // namespace

struct IntegratedBot::Impl {
    explicit Impl(IntegratedBot *owner) : owner(owner) {}

    json getStatus() const
    {
        if (!callbacks.get_status) {
            throw std::runtime_error("status callback is not configured");
        }
        const auto payload = callbacks.get_status();
        if (!payload.value("ok", false)) {
            throw makeBridgeError(payload, "status callback failed");
        }
        return payload;
    }

    void postChat(const std::string &author, const std::string &content, const std::string &channel,
                  const std::string &guild, const std::string &message_url, const std::string &author_id,
                  const std::string &channel_id, const std::string &guild_id, const std::string &message_id) const
    {
        if (!callbacks.relay_chat) {
            throw std::runtime_error("chat callback is not configured");
        }
        const auto payload =
            callbacks.relay_chat(author, content, channel, guild, message_url, author_id, channel_id, guild_id, message_id);
        if (!payload.value("ok", false)) {
            throw makeBridgeError(payload, "chat callback failed");
        }
    }

    json postCommand(const std::string &actor_name, const std::string &command) const
    {
        if (!callbacks.execute_command) {
            throw std::runtime_error("command callback is not configured");
        }
        const auto payload = callbacks.execute_command(actor_name, command);
        if (!payload.value("ok", false)) {
            throw makeBridgeError(payload, "command callback failed");
        }
        return payload;
    }

    void configureWebhook(const std::string &webhook_url) const
    {
        if (!callbacks.configure_webhook) {
            throw std::runtime_error("webhook callback is not configured");
        }
        const auto payload = callbacks.configure_webhook(webhook_url);
        if (!payload.value("ok", false)) {
            throw makeBridgeError(payload, "webhook callback failed");
        }
    }

    std::vector<json> drainSystemMessages() const
    {
        if (!callbacks.drain_system_messages) {
            throw std::runtime_error("system message callback is not configured");
        }
        const auto payload = callbacks.drain_system_messages();
        if (!payload.value("ok", false)) {
            throw makeBridgeError(payload, "system message callback failed");
        }

        std::vector<json> result;
        if (!payload.contains("messages") || !payload["messages"].is_array()) {
            return result;
        }

        for (const auto &item : payload["messages"]) {
            if (item.is_object()) {
                result.push_back(item);
            }
        }
        return result;
    }

    std::uint64_t resolvedGuildId() const
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        return resolved_guild_id;
    }

    std::uint64_t resolvedOutboundChannelId() const
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        return resolved_outbound_channel_id;
    }

    void setResolvedGuildId(std::uint64_t guild_id)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        resolved_guild_id = guild_id;
    }

    void setResolvedOutboundChannelId(std::uint64_t channel_id)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        resolved_outbound_channel_id = channel_id;
    }

    std::uint64_t configuredRelayChannelId() const
    {
        for (const auto channel_id : config.discord.relay_channel_ids) {
            if (channel_id != 0) {
                return channel_id;
            }
        }
        return 0;
    }

    std::vector<std::uint64_t> configuredRelayChannelIds() const
    {
        std::vector<std::uint64_t> channel_ids;
        channel_ids.reserve(config.discord.relay_channel_ids.size());
        for (const auto channel_id : config.discord.relay_channel_ids) {
            if (channel_id != 0) {
                channel_ids.push_back(channel_id);
            }
        }
        return channel_ids;
    }

    bool isRelayChannel(std::uint64_t channel_id) const
    {
        if (channel_id == 0) {
            return false;
        }

        if (const auto configured_channel_ids = configuredRelayChannelIds(); !configured_channel_ids.empty()) {
            return std::find(configured_channel_ids.begin(), configured_channel_ids.end(), channel_id) !=
                   configured_channel_ids.end();
        }

        if (config.discord.outbound_channel_id != 0 && config.discord.outbound_channel_id == channel_id) {
            return true;
        }
        if (config.system_messages.channel_id != 0 && config.system_messages.channel_id == channel_id) {
            return true;
        }
        return resolvedOutboundChannelId() == channel_id;
    }

    std::uint64_t getOutboundChannelId() const
    {
        if (config.discord.outbound_channel_id != 0) {
            return config.discord.outbound_channel_id;
        }
        if (const auto configured_relay_channel_id = configuredRelayChannelId(); configured_relay_channel_id != 0) {
            return configured_relay_channel_id;
        }
        if (config.system_messages.channel_id != 0) {
            return config.system_messages.channel_id;
        }
        return resolvedOutboundChannelId();
    }

    std::uint64_t getSystemChannelId() const
    {
        if (config.system_messages.channel_id != 0) {
            return config.system_messages.channel_id;
        }
        if (const auto configured_relay_channel_id = configuredRelayChannelId(); configured_relay_channel_id != 0) {
            return configured_relay_channel_id;
        }
        return resolvedOutboundChannelId();
    }

    std::uint64_t resolvePreferredGuildId() const
    {
        if (config.discord.guild_id != 0) {
            return config.discord.guild_id;
        }
        return resolvedGuildId();
    }

    static bool isWebhookCandidateChannel(const dpp::channel &channel)
    {
        return channel.is_text_channel() || channel.is_news_channel();
    }

    void ensureWebhookBindingForChannel(std::uint64_t channel_id)
    {
        if (!cluster || channel_id == 0 || !config.discord.auto_create_webhook) {
            return;
        }

        cluster->get_channel_webhooks(channel_id, [this, channel_id](const dpp::confirmation_callback_t &callback) {
            if (callback.is_error()) {
                owner->warning_logger_("Failed to list channel webhooks for channel " + std::to_string(channel_id) + ": " +
                                       callback.get_error().message);
                return;
            }

            const auto webhooks = callback.get<dpp::webhook_map>();
            for (const auto &[id, webhook] : webhooks) {
                (void)id;
                if (webhook.name == config.discord.webhook_name && !webhook.url.empty()) {
                    try {
                        configureWebhook(webhook.url);
                        owner->info_logger_("Bound existing Discord webhook '" + webhook.name + "' to Endcord.");
                    }
                    catch (const std::exception &ex) {
                        owner->warning_logger_("Failed to configure existing webhook: " + std::string(ex.what()));
                    }
                    return;
                }
            }

            dpp::webhook webhook;
            webhook.channel_id = channel_id;
            webhook.name = config.discord.webhook_name;
            cluster->create_webhook(webhook, [this](const dpp::confirmation_callback_t &create_callback) {
                if (create_callback.is_error()) {
                    owner->warning_logger_("Failed to create Discord webhook: " + create_callback.get_error().message);
                    return;
                }

                const auto webhook = create_callback.get<dpp::webhook>();
                if (webhook.url.empty()) {
                    owner->warning_logger_("Discord returned a webhook without a usable URL.");
                    return;
                }

                try {
                    configureWebhook(webhook.url);
                    owner->info_logger_("Created and bound Discord webhook '" + webhook.name + "'.");
                }
                catch (const std::exception &ex) {
                    owner->warning_logger_("Failed to configure new webhook: " + std::string(ex.what()));
                }
            });
        });
    }

    void selectFallbackChannelFromGuild(std::uint64_t guild_id)
    {
        if (!cluster || guild_id == 0) {
            return;
        }

        cluster->channels_get(guild_id, [this, guild_id](const dpp::confirmation_callback_t &callback) {
            if (callback.is_error()) {
                owner->warning_logger_("Failed to list channels for guild " + std::to_string(guild_id) + ": " +
                                       callback.get_error().message);
                return;
            }

            std::vector<dpp::channel> candidates;
            for (const auto &[id, channel] : callback.get<dpp::channel_map>()) {
                (void)id;
                if (isWebhookCandidateChannel(channel)) {
                    candidates.push_back(channel);
                }
            }

            if (candidates.empty()) {
                owner->warning_logger_("Could not auto-select an outbound Discord channel because guild " +
                                       std::to_string(guild_id) + " has no text-compatible channels.");
                return;
            }

            std::sort(candidates.begin(), candidates.end(), [](const dpp::channel &lhs, const dpp::channel &rhs) {
                if (lhs.position != rhs.position) {
                    return lhs.position < rhs.position;
                }
                if (lhs.name != rhs.name) {
                    return lhs.name < rhs.name;
                }
                return lhs.id < rhs.id;
            });

            const auto &chosen = candidates.front();
            setResolvedOutboundChannelId(chosen.id);
            owner->info_logger_("Auto-selected outbound Discord channel '" + chosen.name + "' (" +
                                std::to_string(chosen.id) + ") for Endcord.");
            ensureWebhookBindingForChannel(chosen.id);
        });
    }

    void deriveGuildFromAnyAvailableSource()
    {
        if (!cluster) {
            return;
        }

        cluster->current_user_get_guilds([this](const dpp::confirmation_callback_t &callback) {
            if (callback.is_error()) {
                owner->warning_logger_("Failed to resolve a default guild for Endcord: " + callback.get_error().message);
                return;
            }

            std::vector<dpp::guild> guilds;
            for (const auto &[id, guild] : callback.get<dpp::guild_map>()) {
                (void)id;
                guilds.push_back(guild);
            }

            if (guilds.empty()) {
                owner->warning_logger_("Could not auto-resolve a Discord guild because the bot is not in any guilds.");
                return;
            }

            std::sort(guilds.begin(), guilds.end(), [](const dpp::guild &lhs, const dpp::guild &rhs) {
                if (lhs.name != rhs.name) {
                    return lhs.name < rhs.name;
                }
                return lhs.id < rhs.id;
            });

            const auto &chosen = guilds.front();
            setResolvedGuildId(chosen.id);
            owner->info_logger_("Auto-derived discord.guild_id=" + std::to_string(chosen.id) + " from guild '" +
                                chosen.name + "'.");
            syncCommandsIfNeeded();
            selectFallbackChannelFromGuild(chosen.id);
        });
    }

    void ensureResolvedOutboundChannel()
    {
        if (getOutboundChannelId() != 0) {
            return;
        }

        const auto guild_id = resolvePreferredGuildId();
        if (guild_id != 0) {
            selectFallbackChannelFromGuild(guild_id);
            return;
        }

        deriveGuildFromAnyAvailableSource();
    }

    bool isAuthorized(const dpp::slashcommand_t &event, const SlashCommandRuleConfig &rule) const
    {
        try {
            if (event.command.get_resolved_permission(event.command.usr.id).has(dpp::p_administrator)) {
                return true;
            }
        }
        catch (const std::exception &) {
        }

        if (rule.admin_only) {
            return false;
        }
        if (rule.role_ids.empty()) {
            return true;
        }

        for (const auto role_id : event.command.member.get_roles()) {
            if (std::find(rule.role_ids.begin(), rule.role_ids.end(), static_cast<std::uint64_t>(role_id)) !=
                rule.role_ids.end()) {
                return true;
            }
        }
        return false;
    }

    void syncCommandsIfNeeded()
    {
        if (!cluster || !config.slash_commands.enabled || commands_synced) {
            return;
        }

        auto buildCommands = [this]() {
            std::vector<dpp::slashcommand> commands;
            const auto application_id = cluster->me.id;

            if (config.slash_commands.status.enabled) {
                dpp::slashcommand command("status", "Show the current Endcord bridge status.", application_id);
                if (config.slash_commands.status.admin_only) {
                    command.set_default_permissions(dpp::p_administrator);
                }
                commands.push_back(command);
            }
            if (config.slash_commands.players.enabled) {
                dpp::slashcommand command("players", "Show the players currently online.", application_id);
                if (config.slash_commands.players.admin_only) {
                    command.set_default_permissions(dpp::p_administrator);
                }
                commands.push_back(command);
            }
            if (config.slash_commands.ping.enabled) {
                dpp::slashcommand command("ping", "Check the Discord bot and bridge response time.", application_id);
                if (config.slash_commands.ping.admin_only) {
                    command.set_default_permissions(dpp::p_administrator);
                }
                commands.push_back(command);
            }
            if (config.slash_commands.command.enabled) {
                dpp::slashcommand command("command", "Execute a server command through Endcord.", application_id);
                command.add_option(
                    dpp::command_option(dpp::co_string, "command", "Command to execute without the leading slash.", true));
                if (config.slash_commands.command.admin_only) {
                    command.set_default_permissions(dpp::p_administrator);
                }
                commands.push_back(command);
            }
            if (config.slash_commands.configreload.enabled) {
                dpp::slashcommand command("configreload", "Reload the Endcord plugin configuration.", application_id);
                if (config.slash_commands.configreload.admin_only) {
                    command.set_default_permissions(dpp::p_administrator);
                }
                commands.push_back(command);
            }

            return commands;
        };

        if (config.discord.sync_commands_globally) {
            cluster->global_bulk_command_create(buildCommands(), [this](const dpp::confirmation_callback_t &callback) {
                if (callback.is_error()) {
                    owner->warning_logger_("Failed to sync global slash commands: " + callback.get_error().message);
                    return;
                }
                commands_synced = true;
                owner->info_logger_("Synced integrated slash commands globally.");
            });
            return;
        }

        const auto guild_id = resolvedGuildId();
        if (guild_id == 0) {
            owner->warning_logger_("Skipping integrated slash-command sync because guild id is not resolved yet.");
            return;
        }

        cluster->guild_bulk_command_create(buildCommands(), guild_id, [this, guild_id](const dpp::confirmation_callback_t &callback) {
            if (callback.is_error()) {
                owner->warning_logger_("Failed to sync guild slash commands: " + callback.get_error().message);
                return;
            }
            commands_synced = true;
            owner->info_logger_("Synced integrated slash commands to guild " + std::to_string(guild_id) + ".");
        });
    }

    void ensureRuntimeContext()
    {
        if (!cluster) {
            return;
        }
        if (config.discord.guild_id != 0) {
            setResolvedGuildId(config.discord.guild_id);
            syncCommandsIfNeeded();
            return;
        }

        const auto channel_id = getOutboundChannelId();
        if (channel_id == 0) {
            deriveGuildFromAnyAvailableSource();
            return;
        }

        cluster->channel_get(channel_id, [this, channel_id](const dpp::confirmation_callback_t &callback) {
            if (callback.is_error()) {
                owner->warning_logger_("Failed to resolve guild from channel " + std::to_string(channel_id) + ": " +
                                       callback.get_error().message);
                return;
            }
            const auto channel = callback.get<dpp::channel>();
            setResolvedGuildId(channel.guild_id);
            owner->info_logger_("Derived discord.guild_id=" + std::to_string(channel.guild_id) + " from channel " +
                                std::to_string(channel_id) + ".");
            syncCommandsIfNeeded();
        });
    }

    void ensureWebhookBinding()
    {
        if (!cluster || !config.discord.auto_create_webhook) {
            return;
        }

        const auto channel_id = getOutboundChannelId();
        if (channel_id == 0) {
            owner->warning_logger_("No outbound Discord channel is configured; attempting automatic channel selection.");
            ensureResolvedOutboundChannel();
            return;
        }

        ensureWebhookBindingForChannel(channel_id);
    }

    void startThreads()
    {
        stop_threads = false;

        if (config.presence.enabled && !presence_thread.joinable()) {
            try {
                presence_thread = std::thread([this] {
                    while (!stop_threads) {
                        try {
                            refreshPresence();
                        }
                        catch (const std::exception &ex) {
                            owner->warning_logger_("Failed to refresh integrated bot presence: " + std::string(ex.what()));
                        }

                        if (config.presence.update_interval_seconds <= 0) {
                            break;
                        }
                        std::unique_lock<std::mutex> lock(stop_cv_mutex);
                        stop_cv.wait_for(lock, std::chrono::seconds(config.presence.update_interval_seconds),
                                         [this] { return stop_threads.load(); });
                    }
                });
            }
            catch (const std::system_error &ex) {
                owner->warning_logger_("Failed to start presence update thread: " + std::string(ex.what()));
            }
        }

        if (config.system_messages.enabled && !system_thread.joinable()) {
            try {
                system_thread = std::thread([this] {
                    auto delay = std::max(config.system_messages.poll_interval_seconds, 1);
                    while (!stop_threads) {
                        try {
                            drainAndSendSystemMessages();
                            delay = std::max(config.system_messages.poll_interval_seconds, 1);
                        }
                        catch (const std::exception &ex) {
                            owner->warning_logger_("Failed to relay integrated system messages: " + std::string(ex.what()));
                            delay = std::min(std::max(delay * 2, config.system_messages.failure_backoff_seconds),
                                             config.system_messages.max_backoff_seconds);
                        }

                        std::unique_lock<std::mutex> lock(stop_cv_mutex);
                        stop_cv.wait_for(lock, std::chrono::seconds(delay),
                                         [this] { return stop_threads.load(); });
                    }
                });
            }
            catch (const std::system_error &ex) {
                owner->warning_logger_("Failed to start system message thread: " + std::string(ex.what()));
            }
        }
    }

    void stopThreads()
    {
        {
            std::lock_guard<std::mutex> lock(stop_cv_mutex);
            stop_threads = true;
        }
        stop_cv.notify_all();
        if (presence_thread.joinable()) {
            presence_thread.join();
        }
        if (system_thread.joinable()) {
            system_thread.join();
        }
    }

    ReplacementList buildPresenceReplacements(const json &status_payload, int gateway_latency_ms,
                                              int bridge_latency_ms) const
    {
        std::string online_player_names;
        if (status_payload.contains("online_player_names") && status_payload["online_player_names"].is_array()) {
            std::vector<std::string> names;
            for (const auto &item : status_payload["online_player_names"]) {
                if (item.is_string()) {
                    names.push_back(item.get<std::string>());
                }
            }
            online_player_names = joinStrings(names, ", ");
        }

        return {
            {"{server_name}", status_payload.value("server_name", "Unknown Server")},
            {"{server_version}", status_payload.value("server_version", "unknown")},
            {"{minecraft_version}", status_payload.value("minecraft_version", "unknown")},
            {"{protocol_version}", std::to_string(status_payload.value("protocol_version", 0))},
            {"{online_players}", std::to_string(status_payload.value("online_players", 0))},
            {"{max_players}", std::to_string(status_payload.value("max_players", 0))},
            {"{player_slots_available}", std::to_string(status_payload.value("player_slots_available", 0))},
            {"{player_utilization_percent}", status_payload.value("player_utilization_percent", std::string("0.0"))},
            {"{online_player_names}", online_player_names},
            {"{game_port}", std::to_string(status_payload.value("game_port", 0))},
            {"{game_port_v6}", std::to_string(status_payload.value("game_port_v6", 0))},
            {"{online_mode}", boolText(status_payload.value("online_mode", false))},
            {"{gateway_latency_ms}", std::to_string(gateway_latency_ms)},
            {"{bridge_latency_ms}", std::to_string(bridge_latency_ms)},
            {"{webhook_queue_depth}", std::to_string(status_payload.value("webhook_queue_depth", 0))},
            {"{system_message_queue_depth}", std::to_string(status_payload.value("system_message_queue_depth", 0))},
            {"{system_message_queue_max}", std::to_string(status_payload.value("system_message_queue_max", 0))},
            {"{webhook_configured}", boolText(status_payload.value("webhook_configured", false))},
            {"{runtime_webhook_override_active}", boolText(status_payload.value("runtime_webhook_override_active", false))},
            {"{minecraft_to_discord_enabled}", boolText(status_payload.value("minecraft_to_discord_enabled", false))},
            {"{discord_to_minecraft_enabled}", boolText(status_payload.value("discord_to_minecraft_enabled", false))},
            {"{bot_system_messages_enabled}", boolText(status_payload.value("bot_system_messages_enabled", false))},
            {"{avatar_enabled}", boolText(status_payload.value("avatar_enabled", false))},
            {"{avatar_provider}", status_payload.value("avatar_provider", std::string())},
            {"{bot_bridge_enabled}", boolText(status_payload.value("bot_bridge_enabled", false))},
            {"{integrated_bot_enabled}", boolText(status_payload.value("integrated_bot_enabled", false))},
            {"{integrated_bot_running}", boolText(status_payload.value("integrated_bot_running", false))},
            {"{guild_name}", resolvedGuildId() != 0 ? std::to_string(resolvedGuildId()) : std::string()},
            {"{guild_id}", resolvedGuildId() != 0 ? std::to_string(resolvedGuildId()) : std::string()},
        };
    }

    void refreshPresence()
    {
        if (!cluster) {
            return;
        }

        std::string template_text = config.presence.fallback_text;
        json status_payload = json::object();
        int callback_latency_ms = 0;

        try {
            const auto start = std::chrono::steady_clock::now();
            status_payload = getStatus();
            callback_latency_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
            if (config.discord.auto_create_webhook && !status_payload.value("webhook_configured", false)) {
                ensureWebhookBinding();
                const auto retry_start = std::chrono::steady_clock::now();
                status_payload = getStatus();
                callback_latency_ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - retry_start)
                        .count());
            }
            template_text = config.presence.activity_text;
        }
        catch (const std::exception &ex) {
            if (config.logging.log_presence_updates) {
                owner->warning_logger_("Presence refresh fell back to fallback_text: " + std::string(ex.what()));
            }
        }

        int gateway_latency_ms = 0;
        if (auto *shard = cluster->get_shard(0); shard != nullptr) {
            gateway_latency_ms = static_cast<int>(shard->websocket_ping * 1000.0);
        }

        auto activity_text =
            bridge_support::applyTemplate(template_text, buildPresenceReplacements(status_payload, gateway_latency_ms, callback_latency_ms));
        if (activity_text.empty()) {
            activity_text = config.presence.fallback_text;
        }
        if (activity_text.empty()) {
            activity_text = "Endcord online";
        }

        cluster->set_presence(dpp::presence(toPresenceStatus(config.presence.status), buildActivity(config, activity_text)));
        if (config.logging.log_presence_updates) {
            owner->info_logger_("Updated integrated bot presence to '" + activity_text + "'.");
        }
    }

    void drainAndSendSystemMessages()
    {
        if (!cluster) {
            return;
        }

        const auto channel_id = getSystemChannelId();
        if (channel_id == 0) {
            owner->warning_logger_("System message relay is enabled but no channel_id is configured.");
            return;
        }

        const auto messages = drainSystemMessages();
        int sent = 0;
        for (const auto &payload : messages) {
            if (sent >= config.system_messages.max_messages_per_poll) {
                break;
            }

            const auto event_name = payload.value("event", std::string());
            const auto content_value = payload.value("content", std::string());
            const auto content = bridge_support::applyTemplate(
                config.system_messages.message_template,
                {{"{content}", content_value},
                 {"{message}", content_value},
                 {"{content_length}", std::to_string(content_value.size())},
                 {"{event}", event_name},
                 {"{event_name}", event_name},
                 {"{event_upper}", toUpper(event_name)},
                 {"{event_title}", toTitle(event_name)},
                 {"{player}", payload.value("player_name", std::string())},
                 {"{player_name}", payload.value("player_name", std::string())}});

            if (content.empty()) {
                continue;
            }

            cluster->message_create(dpp::message(channel_id, content));
            ++sent;
        }
    }

    IntegratedBot *owner;
    BotConfig config{};
    IntegratedBotCallbacks callbacks{};
    std::unique_ptr<dpp::cluster> cluster;
    mutable std::mutex state_mutex;
    std::mutex stop_cv_mutex;
    std::condition_variable stop_cv;
    std::atomic_bool stop_threads{false};
    std::thread presence_thread;
    std::thread system_thread;
    std::uint64_t resolved_guild_id = 0;
    std::uint64_t resolved_outbound_channel_id = 0;
    std::atomic<bool> commands_synced{false};
    std::atomic<bool> running{false};
};

IntegratedBot::IntegratedBot(LogCallback info_logger, LogCallback warning_logger)
    : info_logger_(std::move(info_logger)), warning_logger_(std::move(warning_logger)), impl_(std::make_unique<Impl>(this))
{
}

IntegratedBot::~IntegratedBot()
{
    stop();
}

bool IntegratedBot::start(const BotConfig &config, IntegratedBotCallbacks callbacks)
{
    stop();

    if (config.discord.token.empty() || config.discord.token == "replace-me") {
        warning_logger_("Skipping integrated Discord bot startup because discord.token is not configured.");
        return false;
    }
    if (!callbacks.get_status || !callbacks.relay_chat || !callbacks.execute_command || !callbacks.drain_system_messages ||
        !callbacks.configure_webhook) {
        warning_logger_("Skipping integrated Discord bot startup because required callbacks are missing.");
        return false;
    }

    // Non-fatal warnings — bot starts but operator should be aware.
    if (config.discord.relay_to_game_enabled) {
        const bool has_relay_channel = !config.discord.relay_channel_ids.empty() ||
                                       config.discord.outbound_channel_id != 0 ||
                                       config.system_messages.channel_id != 0;
        if (!has_relay_channel) {
            warning_logger_("No relay channel configured (discord.relay_channel_ids, discord.outbound_channel_id, "
                            "or system_messages.channel_id). Discord messages will not be relayed to Minecraft.");
        }
    }
    if (config.relay.max_message_length < 50) {
        warning_logger_("relay.max_message_length=" + std::to_string(config.relay.max_message_length) +
                        " is very low and will truncate most messages. Consider raising it to at least 50.");
    }

    impl_->config = config;
    impl_->callbacks = std::move(callbacks);
    impl_->resolved_guild_id = 0;
    impl_->resolved_outbound_channel_id = 0;
    impl_->commands_synced = false;
    impl_->cluster = std::make_unique<dpp::cluster>(config.discord.token, dpp::i_default_intents | dpp::i_message_content);

    impl_->cluster->on_log([this](const dpp::log_t &event) {
        if (event.severity >= dpp::ll_warning) {
            warning_logger_("Discord runtime: " + event.message);
        }
    });

    impl_->cluster->on_ready([this](const dpp::ready_t &) {
        info_logger_("Integrated Discord bot connected to Discord.");
        impl_->ensureRuntimeContext();
        impl_->ensureWebhookBinding();
        impl_->syncCommandsIfNeeded();
        impl_->startThreads();
    });

    impl_->cluster->on_message_create([this](const dpp::message_create_t &event) {
        const auto &config = impl_->config;
        const auto &message = event.msg;

        if (!config.discord.relay_to_game_enabled || message.guild_id == 0) {
            return;
        }
        if (config.relay.ignore_bot_messages && message.author.is_bot()) {
            return;
        }
        if (config.relay.ignore_webhook_messages && message.webhook_id != 0) {
            return;
        }
        if (impl_->resolvedGuildId() != 0 && message.guild_id != impl_->resolvedGuildId()) {
            return;
        }
        if (!impl_->isRelayChannel(static_cast<std::uint64_t>(message.channel_id))) {
            return;
        }

        std::vector<std::string> attachment_lines;
        if (config.relay.include_attachment_urls) {
            for (const auto &attachment : message.attachments) {
                attachment_lines.push_back(bridge_support::applyTemplate(
                    config.relay.attachment_template,
                    {{"{filename}", attachment.filename},
                     {"{url}", attachment.url},
                     {"{content_type}", attachment.content_type}}));
            }
        }

        const auto attachments_text = joinStrings(attachment_lines, config.relay.join_separator);
        const auto jump_url_text =
            config.relay.include_jump_url
                ? bridge_support::applyTemplate(config.relay.jump_url_template, {{"{url}", message.get_url()}})
                : std::string();
        auto relay_content = bridge_support::applyTemplate(
            config.relay.message_template, buildRelayReplacements(message, message.content, attachments_text, jump_url_text));
        relay_content = truncateText(relay_content, static_cast<std::size_t>(config.relay.max_message_length));
        if (relay_content.empty()) {
            return;
        }

        try {
            impl_->postChat(getDisplayName(message), relay_content, std::to_string(message.channel_id),
                            std::to_string(message.guild_id), message.get_url(), std::to_string(message.author.id),
                            std::to_string(message.channel_id), std::to_string(message.guild_id),
                            std::to_string(message.id));
            if (config.logging.log_relay_successes) {
                info_logger_("Relayed Discord chat from '" + getDisplayName(message) + "' into Minecraft.");
            }
        }
        catch (const std::exception &ex) {
            warning_logger_("Failed to relay Discord message into Minecraft: " + std::string(ex.what()));
        }
    });

    impl_->cluster->on_slashcommand([this](const dpp::slashcommand_t &event) {
        const auto command_name = event.command.get_command_name();
        const auto ephemeral = impl_->config.slash_commands.ephemeral_responses;
        auto reply = [&event](const std::string &text, bool ephemeral_value) {
            event.reply(makeReplyMessage(text, ephemeral_value));
        };

        try {
            if (command_name == "status" && impl_->config.slash_commands.status.enabled) {
                if (!impl_->isAuthorized(event, impl_->config.slash_commands.status)) {
                    reply("You are not allowed to use this command.", ephemeral);
                    return;
                }

                const auto status = impl_->getStatus();
                reply(joinLines({"Server: `" + status.value("server_name", std::string("unknown")) + "`",
                                 "Server version: `" + status.value("server_version", std::string("unknown")) + "`",
                                 "Minecraft: `" + status.value("minecraft_version", std::string("unknown")) + "`",
                                 "Protocol: `" + std::to_string(status.value("protocol_version", 0)) + "`",
                                 "Online players: `" + std::to_string(status.value("online_players", 0)) + "/" +
                                     std::to_string(status.value("max_players", 0)) + "`",
                                 "Webhook queue depth: `" + std::to_string(status.value("webhook_queue_depth", 0)) + "`",
                                 "Avatar provider: `" + status.value("avatar_provider", std::string("<disabled>")) + "`"}),
                      ephemeral);
                return;
            }

            if (command_name == "players" && impl_->config.slash_commands.players.enabled) {
                if (!impl_->isAuthorized(event, impl_->config.slash_commands.players)) {
                    reply("You are not allowed to use this command.", ephemeral);
                    return;
                }

                const auto status = impl_->getStatus();
                std::vector<std::string> lines = {"Online players: `" + std::to_string(status.value("online_players", 0)) + "`"};
                if (status.contains("online_player_names") && status["online_player_names"].is_array() &&
                    !status["online_player_names"].empty()) {
                    lines.push_back("Players:");
                    for (const auto &item : status["online_player_names"]) {
                        if (item.is_string()) {
                            lines.push_back("- " + item.get<std::string>());
                        }
                    }
                }
                else {
                    lines.push_back("Nobody is online right now.");
                }

                reply(truncateText(joinLines(lines), kDiscordSafeReplyLength), ephemeral);
                return;
            }

            if (command_name == "ping" && impl_->config.slash_commands.ping.enabled) {
                if (!impl_->isAuthorized(event, impl_->config.slash_commands.ping)) {
                    reply("You are not allowed to use this command.", ephemeral);
                    return;
                }

                const auto start = std::chrono::steady_clock::now();
                const auto status = impl_->getStatus();
                const auto bridge_latency_ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
                int gateway_latency_ms = 0;
                if (auto *shard = impl_->cluster->get_shard(0); shard != nullptr) {
                    gateway_latency_ms = static_cast<int>(shard->websocket_ping * 1000.0);
                }

                reply(joinLines({"Gateway latency: `" + std::to_string(gateway_latency_ms) + " ms`",
                                 "Bridge latency: `" + std::to_string(bridge_latency_ms) + " ms`",
                                 "Server: `" + status.value("server_name", std::string("unknown")) + "`",
                                 "Players: `" + std::to_string(status.value("online_players", 0)) + "/" +
                                     std::to_string(status.value("max_players", 0)) + "`"}),
                      ephemeral);
                return;
            }

            if (command_name == "command" && impl_->config.slash_commands.command.enabled) {
                if (!impl_->isAuthorized(event, impl_->config.slash_commands.command)) {
                    reply("You are not allowed to use this command.", ephemeral);
                    return;
                }

                const auto command = std::get<std::string>(event.get_parameter("command"));
                const auto result = impl_->postCommand(getDisplayName(event.command.member, event.command.usr), command);
                std::vector<std::string> lines = {"Dispatched: `" + boolText(result.value("dispatched", false)) + "`"};
                if (result.contains("output") && result["output"].is_array() && !result["output"].empty()) {
                    lines.push_back("Output:");
                    for (const auto &item : result["output"]) {
                        if (item.is_string()) {
                            lines.push_back("- " + item.get<std::string>());
                        }
                    }
                }
                if (result.contains("errors") && result["errors"].is_array() && !result["errors"].empty()) {
                    lines.push_back("Errors:");
                    for (const auto &item : result["errors"]) {
                        if (item.is_string()) {
                            lines.push_back("- " + item.get<std::string>());
                        }
                    }
                }

                reply(truncateText(joinLines(lines), kDiscordSafeReplyLength), ephemeral);
                return;
            }

            if (command_name == "configreload" && impl_->config.slash_commands.configreload.enabled) {
                if (!impl_->isAuthorized(event, impl_->config.slash_commands.configreload)) {
                    reply("You are not allowed to use this command.", ephemeral);
                    return;
                }

                impl_->postCommand(getDisplayName(event.command.member, event.command.usr), "endcord reload");
                reply("Bridge reload command sent.", ephemeral);
                return;
            }
        }
        catch (const std::exception &ex) {
            reply("Command failed: " + std::string(ex.what()), ephemeral);
        }
    });

    impl_->cluster->start(dpp::st_return);
    impl_->running = true;
    info_logger_("Started integrated Discord bot runtime.");
    return true;
}

void IntegratedBot::stop()
{
    if (!impl_->running) {
        return;
    }

    impl_->stopThreads();
    if (impl_->cluster) {
        impl_->cluster->shutdown();
        impl_->cluster.reset();
    }
    impl_->commands_synced = false;
    impl_->running = false;
    info_logger_("Stopped integrated Discord bot runtime.");
}

bool IntegratedBot::isRunning() const
{
    return impl_->running;
}

}  // namespace endcord
