from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path


@dataclass(slots=True)
class DiscordConfig:
    token: str
    guild_id: int
    relay_channel_ids: list[int]
    outbound_channel_id: int
    command_role_ids: list[int]
    status_role_ids: list[int]
    relay_to_game_enabled: bool
    sync_commands_globally: bool
    auto_create_webhook: bool
    webhook_name: str


@dataclass(slots=True)
class PluginBridgeConfig:
    base_url: str
    shared_secret: str
    request_timeout_seconds: int
    configure_webhook_on_startup: bool
    request_max_retries: int
    request_retry_base_seconds: float
    request_retry_max_seconds: float


@dataclass(slots=True)
class RelayConfig:
    include_attachment_urls: bool
    include_jump_url: bool
    ignore_bot_messages: bool
    ignore_webhook_messages: bool
    message_template: str
    attachment_template: str
    jump_url_template: str
    join_separator: str
    max_message_length: int


@dataclass(slots=True)
class SlashCommandConfig:
    enabled: bool
    ephemeral_responses: bool


@dataclass(slots=True)
class PresenceConfig:
    enabled: bool
    status: str
    activity_type: str
    activity_text: str
    fallback_text: str
    streaming_url: str
    update_interval_seconds: int


@dataclass(slots=True)
class LoggingConfig:
    level: str
    log_ignored_messages: bool
    log_relay_successes: bool
    log_presence_updates: bool


@dataclass(slots=True)
class SystemMessageConfig:
    enabled: bool
    channel_id: int
    poll_interval_seconds: int
    failure_backoff_seconds: int
    max_backoff_seconds: int
    message_template: str
    max_messages_per_poll: int


@dataclass(slots=True)
class BotConfig:
    discord: DiscordConfig
    plugin_bridge: PluginBridgeConfig
    relay: RelayConfig
    slash_commands: SlashCommandConfig
    presence: PresenceConfig
    logging: LoggingConfig
    system_messages: SystemMessageConfig

    @staticmethod
    def load(path: Path) -> "BotConfig":
        data = json.loads(path.read_text(encoding="utf-8"))

        discord_cfg = data["discord"]
        plugin_cfg = data["plugin_bridge"]
        relay_cfg = data.get("relay", {})
        slash_cfg = data.get("slash_commands", {})
        presence_cfg = data.get("presence", {})
        logging_cfg = data.get("logging", {})
        system_cfg = data.get("system_messages", {})

        config = BotConfig(
            discord=DiscordConfig(
                token=discord_cfg["token"],
                guild_id=int(discord_cfg["guild_id"]),
                relay_channel_ids=[int(x) for x in discord_cfg.get("relay_channel_ids", [])],
                outbound_channel_id=int(discord_cfg.get("outbound_channel_id", 0)),
                command_role_ids=[int(x) for x in discord_cfg.get("command_role_ids", [])],
                status_role_ids=[int(x) for x in discord_cfg.get("status_role_ids", [])],
                relay_to_game_enabled=bool(discord_cfg.get("relay_to_game_enabled", True)),
                sync_commands_globally=bool(discord_cfg.get("sync_commands_globally", False)),
                auto_create_webhook=bool(discord_cfg.get("auto_create_webhook", True)),
                webhook_name=str(discord_cfg.get("webhook_name", "Bedrock Discord Bridge")).strip()
                or "Bedrock Discord Bridge",
            ),
            plugin_bridge=PluginBridgeConfig(
                base_url=str(plugin_cfg["base_url"]).rstrip("/"),
                shared_secret=str(plugin_cfg["shared_secret"]).strip(),
                request_timeout_seconds=max(int(plugin_cfg.get("request_timeout_seconds", 10)), 1),
                configure_webhook_on_startup=bool(plugin_cfg.get("configure_webhook_on_startup", True)),
                request_max_retries=max(int(plugin_cfg.get("request_max_retries", 3)), 0),
                request_retry_base_seconds=max(float(plugin_cfg.get("request_retry_base_seconds", 1.5)), 0.1),
                request_retry_max_seconds=max(float(plugin_cfg.get("request_retry_max_seconds", 15.0)), 0.1),
            ),
            relay=RelayConfig(
                include_attachment_urls=bool(relay_cfg.get("include_attachment_urls", True)),
                include_jump_url=bool(relay_cfg.get("include_jump_url", False)),
                ignore_bot_messages=bool(relay_cfg.get("ignore_bot_messages", True)),
                ignore_webhook_messages=bool(relay_cfg.get("ignore_webhook_messages", True)),
                message_template=str(relay_cfg.get("message_template", "{content}")),
                attachment_template=str(relay_cfg.get("attachment_template", "[attachment] {url}")),
                jump_url_template=str(relay_cfg.get("jump_url_template", "[jump] {url}")),
                join_separator=str(relay_cfg.get("join_separator", "\n")),
                max_message_length=max(int(relay_cfg.get("max_message_length", 1800)), 32),
            ),
            slash_commands=SlashCommandConfig(
                enabled=bool(slash_cfg.get("enabled", True)),
                ephemeral_responses=bool(slash_cfg.get("ephemeral_responses", True)),
            ),
            presence=PresenceConfig(
                enabled=bool(presence_cfg.get("enabled", True)),
                status=str(presence_cfg.get("status", "online")).lower(),
                activity_type=str(presence_cfg.get("activity_type", "watching")).lower(),
                activity_text=str(presence_cfg.get("activity_text", "{server_name} | {online_players} online")),
                fallback_text=str(presence_cfg.get("fallback_text", "Bridge online")),
                streaming_url=str(presence_cfg.get("streaming_url", "")),
                update_interval_seconds=max(int(presence_cfg.get("update_interval_seconds", 120)), 0),
            ),
            logging=LoggingConfig(
                level=str(logging_cfg.get("level", "INFO")).upper(),
                log_ignored_messages=bool(logging_cfg.get("log_ignored_messages", False)),
                log_relay_successes=bool(logging_cfg.get("log_relay_successes", False)),
                log_presence_updates=bool(logging_cfg.get("log_presence_updates", False)),
            ),
            system_messages=SystemMessageConfig(
                enabled=bool(system_cfg.get("enabled", False)),
                channel_id=int(system_cfg.get("channel_id", 0)),
                poll_interval_seconds=max(int(system_cfg.get("poll_interval_seconds", 2)), 1),
                failure_backoff_seconds=max(int(system_cfg.get("failure_backoff_seconds", 5)), 1),
                max_backoff_seconds=max(int(system_cfg.get("max_backoff_seconds", 60)), 1),
                message_template=str(system_cfg.get("message_template", "{content}")),
                max_messages_per_poll=max(int(system_cfg.get("max_messages_per_poll", 20)), 1),
            ),
        )

        if not config.discord.token or config.discord.token == "replace-me":
            raise ValueError("discord.token must be configured")
        if config.discord.guild_id < 0:
            raise ValueError("discord.guild_id must be 0 or a positive guild id")
        if not config.discord.relay_channel_ids and config.discord.outbound_channel_id <= 0:
            raise ValueError("configure at least one discord.relay_channel_ids entry or discord.outbound_channel_id")
        if not config.plugin_bridge.base_url.startswith(("http://", "https://")):
            raise ValueError("plugin_bridge.base_url must start with http:// or https://")
        if not config.plugin_bridge.shared_secret or config.plugin_bridge.shared_secret == "change-me":
            raise ValueError("plugin_bridge.shared_secret must be configured")
        if config.presence.status not in {"online", "idle", "dnd", "invisible"}:
            raise ValueError("presence.status must be one of online, idle, dnd, invisible")
        if config.presence.activity_type not in {"playing", "streaming", "listening", "watching", "competing", "custom"}:
            raise ValueError(
                "presence.activity_type must be one of playing, streaming, listening, watching, competing, custom"
            )
        if config.logging.level not in {"CRITICAL", "ERROR", "WARNING", "INFO", "DEBUG"}:
            raise ValueError("logging.level must be one of CRITICAL, ERROR, WARNING, INFO, DEBUG")

        return config
