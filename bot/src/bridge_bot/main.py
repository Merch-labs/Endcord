from __future__ import annotations

import argparse
import asyncio
import json
import logging
from pathlib import Path
from typing import Any

import aiohttp
import discord
from discord import app_commands

from bridge_bot.config import BotConfig


LOGGER = logging.getLogger("endcord_bot")


class PluginBridgeClient:
    def __init__(self, config: BotConfig) -> None:
        self._config = config
        timeout = aiohttp.ClientTimeout(total=config.plugin_bridge.request_timeout_seconds)
        self._session = aiohttp.ClientSession(timeout=timeout)

    async def close(self) -> None:
        await self._session.close()

    async def post_chat(
        self,
        *,
        author: str,
        content: str,
        channel_name: str,
        guild_name: str,
        message_url: str,
    ) -> dict[str, Any]:
        payload = {
            "author": author,
            "content": content,
            "channel_name": channel_name,
            "guild_name": guild_name,
            "message_url": message_url,
        }
        return await self._request("POST", "/chat", payload)

    async def post_command(self, *, actor_name: str, command: str) -> dict[str, Any]:
        return await self._request("POST", "/command", {"actor_name": actor_name, "command": command})

    async def get_status(self) -> dict[str, Any]:
        return await self._request("GET", "/status", None, retryable=True)

    async def drain_system_messages(self) -> list[dict[str, Any]]:
        data = await self._request("POST", "/system-messages/drain", {}, retryable=True)
        messages = data.get("messages", [])
        if not isinstance(messages, list):
            raise RuntimeError("bridge returned invalid system message payload")
        return [message for message in messages if isinstance(message, dict)]

    async def configure_webhook(self, webhook_url: str) -> dict[str, Any]:
        return await self._request("POST", "/webhook", {"webhook_url": webhook_url}, retryable=True)

    async def _request(
        self,
        method: str,
        suffix: str,
        payload: dict[str, Any] | None,
        *,
        retryable: bool = False,
    ) -> dict[str, Any]:
        url = f"{self._config.plugin_bridge.base_url}{suffix}"
        headers = {
            "Authorization": f"Bearer {self._config.plugin_bridge.shared_secret}",
            "Content-Type": "application/json",
        }
        attempts = self._config.plugin_bridge.request_max_retries + 1 if retryable else 1
        delay = self._config.plugin_bridge.request_retry_base_seconds
        last_error: Exception | None = None

        for attempt in range(1, attempts + 1):
            try:
                async with self._session.request(method, url, headers=headers, json=payload) as response:
                    text = await response.text()
                    if not text:
                        raise RuntimeError(f"bridge returned empty response with HTTP {response.status}")

                    try:
                        data = json.loads(text)
                    except json.JSONDecodeError as exc:
                        raise RuntimeError(f"bridge returned invalid JSON with HTTP {response.status}: {text}") from exc

                    if response.status >= 400 or not data.get("ok", False):
                        error = data.get("error", f"HTTP {response.status}")
                        raise RuntimeError(str(error))

                    return data
            except asyncio.CancelledError:
                raise
            except Exception as exc:  # noqa: BLE001
                last_error = exc
                if attempt >= attempts:
                    break
                await asyncio.sleep(delay)
                delay = min(delay * 2.0, self._config.plugin_bridge.request_retry_max_seconds)

        raise RuntimeError(str(last_error) if last_error is not None else "bridge request failed")


class EndcordBot(discord.Client):
    def __init__(self, config: BotConfig) -> None:
        intents = discord.Intents.default()
        intents.guilds = True
        intents.guild_messages = True
        intents.message_content = True

        super().__init__(intents=intents)
        self.config = config
        self.bridge = PluginBridgeClient(config)
        self.tree = app_commands.CommandTree(self)
        self._presence_task: asyncio.Task[None] | None = None
        self._system_message_task: asyncio.Task[None] | None = None
        self._commands_synced = False
        self._register_commands()

    async def close(self) -> None:
        if self._presence_task is not None:
            self._presence_task.cancel()
            try:
                await self._presence_task
            except asyncio.CancelledError:
                pass
        if self._system_message_task is not None:
            self._system_message_task.cancel()
            try:
                await self._system_message_task
            except asyncio.CancelledError:
                pass
        await self.bridge.close()
        await super().close()

    async def setup_hook(self) -> None:
        if self.config.slash_commands.enabled and self.config.discord.sync_commands_globally:
            await self.tree.sync()
            self._commands_synced = True
            LOGGER.info("Synced slash commands globally.")

    async def on_ready(self) -> None:
        LOGGER.info("Logged in as %s (%s)", self.user, getattr(self.user, "id", "unknown"))
        await self._ensure_runtime_context()
        await self._ensure_webhook_binding()
        await self._sync_commands_if_needed()
        if self.config.presence.enabled and (self._presence_task is None or self._presence_task.done()):
            self._presence_task = asyncio.create_task(self._presence_loop())
        if self.config.system_messages.enabled and (
            self._system_message_task is None or self._system_message_task.done()
        ):
            self._system_message_task = asyncio.create_task(self._system_message_loop())

    async def on_message(self, message: discord.Message) -> None:
        if not self.config.discord.relay_to_game_enabled:
            return
        if self.config.relay.ignore_bot_messages and message.author.bot:
            self._log_ignored_message(message, "author is a bot")
            return
        if self.config.relay.ignore_webhook_messages and message.webhook_id is not None:
            self._log_ignored_message(message, "message came from a webhook")
            return
        if message.guild is None:
            self._log_ignored_message(message, "message is outside the configured guild")
            return
        if self.config.discord.guild_id > 0 and message.guild.id != self.config.discord.guild_id:
            self._log_ignored_message(message, "message is outside the configured guild")
            return
        if self.config.discord.relay_channel_ids and message.channel.id not in self.config.discord.relay_channel_ids:
            self._log_ignored_message(message, "channel is not in discord.relay_channel_ids")
            return

        content = message.content.strip()
        attachment_lines: list[str] = []
        if self.config.relay.include_attachment_urls and message.attachments:
            attachment_lines.extend(
                self._apply_template(
                    self.config.relay.attachment_template,
                    {
                        "{filename}": attachment.filename,
                        "{url}": attachment.url,
                        "{content_type}": attachment.content_type or "",
                    },
                )
                for attachment in message.attachments
            )
        attachments_text = self.config.relay.join_separator.join(line for line in attachment_lines if line).strip()

        jump_url_text = ""
        if self.config.relay.include_jump_url:
            jump_url_text = self._apply_template(
                self.config.relay.jump_url_template,
                {
                    "{url}": message.jump_url,
                },
            )

        relay_content = self._apply_template(
            self.config.relay.message_template,
            self._build_relay_replacements(
                author=message.author.display_name,
                content=content,
                attachments=attachments_text,
                jump_url=jump_url_text,
                channel=message.channel.name,
                guild=message.guild.name,
                message_url=message.jump_url,
            ),
        ).strip()
        if not relay_content:
            self._log_ignored_message(message, "formatted relay content is empty")
            return

        relay_content = relay_content[: self.config.relay.max_message_length]

        try:
            await self.bridge.post_chat(
                author=message.author.display_name,
                content=relay_content,
                channel_name=message.channel.name,
                guild_name=message.guild.name,
                message_url=message.jump_url,
            )
            if self.config.logging.log_relay_successes:
                LOGGER.info("Relayed Discord message %s from #%s into Minecraft.", message.id, message.channel.name)
        except Exception as exc:  # noqa: BLE001
            LOGGER.exception("Failed to relay Discord message %s into Minecraft: %s", message.id, exc)

    def _register_commands(self) -> None:
        if not self.config.slash_commands.enabled:
            return

        @self.tree.command(name="status", description="Show the current Endcord bridge status.")
        async def status(interaction: discord.Interaction) -> None:
            if not self._is_authorized(interaction, self.config.discord.status_role_ids):
                await interaction.response.send_message("You are not allowed to use this command.", ephemeral=True)
                return

            await interaction.response.defer(ephemeral=self.config.slash_commands.ephemeral_responses)
            try:
                status = await self.bridge.get_status()
            except Exception as exc:  # noqa: BLE001
                await interaction.followup.send(f"Status request failed: {exc}", ephemeral=True)
                return

            lines = [
                f"Server: `{status.get('server_name', 'unknown')}`",
                f"Minecraft: `{status.get('minecraft_version', 'unknown')}`",
                f"Online players: `{status.get('online_players', 0)}`",
                f"Webhook queue depth: `{status.get('webhook_queue_depth', 0)}`",
                f"Avatar provider: `{status.get('avatar_provider') or '<disabled>'}`",
            ]
            await interaction.followup.send("\n".join(lines), ephemeral=self.config.slash_commands.ephemeral_responses)

        @self.tree.command(name="command", description="Execute a server command through the bridge.")
        @app_commands.describe(command="Command to execute without the leading slash.")
        async def command(interaction: discord.Interaction, command: str) -> None:
            if not self._is_authorized(interaction, self.config.discord.command_role_ids):
                await interaction.response.send_message("You are not allowed to use this command.", ephemeral=True)
                return

            await interaction.response.defer(ephemeral=self.config.slash_commands.ephemeral_responses)
            try:
                result = await self.bridge.post_command(actor_name=interaction.user.display_name, command=command)
            except Exception as exc:  # noqa: BLE001
                await interaction.followup.send(f"Command failed: {exc}", ephemeral=True)
                return

            output = result.get("output", [])
            errors = result.get("errors", [])
            lines = [f"Dispatched: `{result.get('dispatched', False)}`"]
            if output:
                lines.append("Output:")
                lines.extend(f"- {line}" for line in output[:10])
            if errors:
                lines.append("Errors:")
                lines.extend(f"- {line}" for line in errors[:10])

            response_text = "\n".join(lines)
            if len(response_text) > 1800:
                response_text = response_text[:1797] + "..."
            await interaction.followup.send(response_text, ephemeral=self.config.slash_commands.ephemeral_responses)

        @self.tree.command(name="reloadbridge", description="Reload the Endcord plugin configuration.")
        async def reloadbridge(interaction: discord.Interaction) -> None:
            if not self._is_authorized(interaction, self.config.discord.command_role_ids):
                await interaction.response.send_message("You are not allowed to use this command.", ephemeral=True)
                return

            await interaction.response.defer(ephemeral=self.config.slash_commands.ephemeral_responses)
            try:
                await self.bridge.post_command(actor_name=interaction.user.display_name, command="endcord reload")
            except Exception as exc:  # noqa: BLE001
                await interaction.followup.send(f"Reload failed: {exc}", ephemeral=True)
                return

            await interaction.followup.send("Bridge reload command sent.", ephemeral=self.config.slash_commands.ephemeral_responses)

    def _is_authorized(self, interaction: discord.Interaction, role_ids: list[int]) -> bool:
        if interaction.user.guild_permissions.administrator:
            return True
        if not role_ids:
            return True
        user = interaction.user
        if not isinstance(user, discord.Member):
            return False
        user_role_ids = {role.id for role in user.roles}
        return bool(user_role_ids.intersection(role_ids))

    async def _presence_loop(self) -> None:
        interval = self.config.presence.update_interval_seconds
        while not self.is_closed():
            try:
                await self._refresh_presence()
            except asyncio.CancelledError:
                raise
            except Exception as exc:  # noqa: BLE001
                LOGGER.warning("Failed to refresh bot presence: %s", exc)

            if interval <= 0:
                return
            await asyncio.sleep(interval)

    async def _refresh_presence(self) -> None:
        template_text = self.config.presence.fallback_text
        status_payload: dict[str, Any] = {}

        try:
            status_payload = await self.bridge.get_status()
            if self.config.plugin_bridge.configure_webhook_on_startup and not status_payload.get("webhook_configured", False):
                await self._ensure_webhook_binding()
                status_payload = await self.bridge.get_status()
            template_text = self.config.presence.activity_text
        except Exception as exc:  # noqa: BLE001
            if self.config.logging.log_presence_updates:
                LOGGER.warning("Presence status refresh fell back to fallback_text: %s", exc)

        guild = self.get_guild(self.config.discord.guild_id)
        activity_text = self._apply_template(
            template_text,
            {
                "{server_name}": str(status_payload.get("server_name", "Unknown Server")),
                "{minecraft_version}": str(status_payload.get("minecraft_version", "unknown")),
                "{online_players}": str(status_payload.get("online_players", 0)),
                "{webhook_queue_depth}": str(status_payload.get("webhook_queue_depth", 0)),
                "{guild_name}": guild.name if guild is not None else "",
            },
        ).strip()
        if not activity_text:
            activity_text = self.config.presence.fallback_text or "Endcord online"

        await self.change_presence(
            status=self._discord_status(self.config.presence.status),
            activity=self._build_activity(activity_text),
        )
        if self.config.logging.log_presence_updates:
            LOGGER.info("Updated presence to '%s' (%s).", activity_text, self.config.presence.activity_type)

    def _build_activity(self, activity_text: str) -> discord.BaseActivity | None:
        activity_type = self.config.presence.activity_type
        if activity_type == "custom":
            return discord.CustomActivity(name=activity_text)
        if activity_type == "streaming":
            return discord.Streaming(name=activity_text, url=self.config.presence.streaming_url or "https://twitch.tv/")

        type_map = {
            "playing": discord.ActivityType.playing,
            "listening": discord.ActivityType.listening,
            "watching": discord.ActivityType.watching,
            "competing": discord.ActivityType.competing,
        }
        return discord.Activity(type=type_map[activity_type], name=activity_text)

    @staticmethod
    def _discord_status(value: str) -> discord.Status:
        return {
            "online": discord.Status.online,
            "idle": discord.Status.idle,
            "dnd": discord.Status.dnd,
            "invisible": discord.Status.invisible,
        }[value]

    def _log_ignored_message(self, message: discord.Message, reason: str) -> None:
        if self.config.logging.log_ignored_messages:
            LOGGER.debug("Ignoring Discord message %s: %s.", message.id, reason)

    async def _system_message_loop(self) -> None:
        delay = self.config.system_messages.poll_interval_seconds
        while not self.is_closed():
            try:
                await self._drain_and_send_system_messages()
                delay = self.config.system_messages.poll_interval_seconds
            except asyncio.CancelledError:
                raise
            except Exception as exc:  # noqa: BLE001
                LOGGER.warning("Failed to relay queued system messages: %s", exc)
                delay = min(max(delay * 2, self.config.system_messages.failure_backoff_seconds),
                            self.config.system_messages.max_backoff_seconds)

            await asyncio.sleep(delay)

    async def _drain_and_send_system_messages(self) -> None:
        channel = await self._resolve_system_channel()
        if channel is None:
            return

        messages = await self.bridge.drain_system_messages()
        for payload in messages[: self.config.system_messages.max_messages_per_poll]:
            content = self._apply_template(
                self.config.system_messages.message_template,
                {
                    "{content}": str(payload.get("content", "")),
                    "{event}": str(payload.get("event", "")),
                    "{player_name}": str(payload.get("player_name", "")),
                },
            ).strip()
            if not content:
                continue
            await channel.send(content)
            if self.config.logging.log_relay_successes:
                LOGGER.info("Sent bot-owned system message for event '%s'.", payload.get("event", "unknown"))

    async def _resolve_system_channel(self):
        channel_id = self.config.system_messages.channel_id
        if channel_id <= 0 and self.config.discord.relay_channel_ids:
            channel_id = self.config.discord.relay_channel_ids[0]
        if channel_id <= 0:
            LOGGER.warning("System message relay is enabled but no channel_id is configured.")
            return None

        return await self._resolve_channel(channel_id)

    async def _sync_commands_if_needed(self) -> None:
        if self._commands_synced or not self.config.slash_commands.enabled or self.config.discord.sync_commands_globally:
            return
        if self.config.discord.guild_id <= 0:
            LOGGER.warning("Skipping guild slash-command sync because discord.guild_id is not resolved yet.")
            return

        guild_object = discord.Object(id=self.config.discord.guild_id)
        self.tree.copy_global_to(guild=guild_object)
        await self.tree.sync(guild=guild_object)
        self._commands_synced = True
        LOGGER.info("Synced slash commands to guild %s.", self.config.discord.guild_id)

    async def _ensure_runtime_context(self) -> None:
        if self.config.discord.guild_id > 0:
            return

        channel_id = self.config.discord.outbound_channel_id
        if channel_id <= 0 and self.config.discord.relay_channel_ids:
            channel_id = self.config.discord.relay_channel_ids[0]
        if channel_id <= 0 and self.config.system_messages.channel_id > 0:
            channel_id = self.config.system_messages.channel_id
        if channel_id <= 0:
            return

        channel = await self._resolve_channel(channel_id)
        if channel is None or getattr(channel, "guild", None) is None:
            return

        self.config.discord.guild_id = channel.guild.id
        LOGGER.info("Derived discord.guild_id=%s from channel %s.", self.config.discord.guild_id, channel_id)

    async def _ensure_webhook_binding(self) -> None:
        if not self.config.plugin_bridge.configure_webhook_on_startup or not self.config.discord.auto_create_webhook:
            return

        channel_id = self.config.discord.outbound_channel_id
        if channel_id <= 0 and self.config.discord.relay_channel_ids:
            channel_id = self.config.discord.relay_channel_ids[0]
        if channel_id <= 0:
            LOGGER.warning("Skipping webhook provisioning because no outbound Discord channel is configured.")
            return

        channel = await self._resolve_channel(channel_id)
        if channel is None:
            return
        if not isinstance(channel, discord.TextChannel):
            LOGGER.warning("Configured outbound channel %s does not support webhooks.", channel_id)
            return

        try:
            webhooks = await channel.webhooks()
        except Exception as exc:  # noqa: BLE001
            LOGGER.warning("Failed to list channel webhooks for %s: %s", channel_id, exc)
            return

        webhook = next((hook for hook in webhooks if hook.name == self.config.discord.webhook_name and hook.token), None)
        if webhook is None:
            try:
                webhook = await channel.create_webhook(name=self.config.discord.webhook_name, reason="Endcord setup")
                LOGGER.info("Created webhook '%s' in #%s.", self.config.discord.webhook_name, channel.name)
            except Exception as exc:  # noqa: BLE001
                LOGGER.warning("Failed to create webhook in #%s: %s", channel.name, exc)
                return

        try:
            await self.bridge.configure_webhook(webhook.url)
        except Exception as exc:  # noqa: BLE001
            LOGGER.warning("Failed to push webhook configuration into the plugin bridge: %s", exc)
            return

        if self.config.logging.log_relay_successes:
            LOGGER.info("Configured plugin webhook target from channel #%s.", channel.name)

    async def _resolve_channel(self, channel_id: int):
        channel = self.get_channel(channel_id)
        if channel is not None:
            return channel
        try:
            return await self.fetch_channel(channel_id)
        except Exception as exc:  # noqa: BLE001
            LOGGER.warning("Failed to resolve channel %s: %s", channel_id, exc)
            return None

    @staticmethod
    def _build_relay_replacements(
        *,
        author: str,
        content: str,
        attachments: str,
        jump_url: str,
        channel: str,
        guild: str,
        message_url: str,
    ) -> dict[str, str]:
        return {
            "{author}": author,
            "{content}": content,
            "{attachments}": attachments,
            "{jump_url}": jump_url,
            "{channel}": channel,
            "{guild}": guild,
            "{message_url}": message_url,
        }

    @staticmethod
    def _apply_template(template: str, replacements: dict[str, str]) -> str:
        value = template
        for needle, replacement in replacements.items():
            value = value.replace(needle, replacement)
        return value


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the Endcord companion bot.")
    parser.add_argument("config", nargs="?", default="config.json", help="Path to the bot JSON config file.")
    return parser.parse_args()


async def async_main() -> None:
    args = parse_args()
    config = BotConfig.load(Path(args.config))

    logging.basicConfig(
        level=getattr(logging, config.logging.level, logging.INFO),
        format="[%(asctime)s] %(levelname)s %(name)s: %(message)s",
    )

    client = EndcordBot(config)
    await client.start(config.discord.token)


def main() -> None:
    try:
        asyncio.run(async_main())
    except KeyboardInterrupt:
        LOGGER.info("Bot interrupted, shutting down.")


if __name__ == "__main__":
    main()
