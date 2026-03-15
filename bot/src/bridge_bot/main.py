from __future__ import annotations

import argparse
import asyncio
import json
import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import aiohttp
import discord
from discord import app_commands


LOGGER = logging.getLogger("bedrock_discord_bridge_bot")


@dataclass(slots=True)
class DiscordConfig:
    token: str
    guild_id: int
    relay_channel_ids: list[int]
    command_role_ids: list[int]
    status_role_ids: list[int]
    relay_to_game_enabled: bool
    sync_commands_globally: bool


@dataclass(slots=True)
class PluginBridgeConfig:
    base_url: str
    shared_secret: str
    request_timeout_seconds: int


@dataclass(slots=True)
class RelayConfig:
    include_attachment_urls: bool
    include_jump_url: bool
    max_message_length: int


@dataclass(slots=True)
class SlashCommandConfig:
    enabled: bool
    ephemeral_responses: bool


@dataclass(slots=True)
class BotConfig:
    discord: DiscordConfig
    plugin_bridge: PluginBridgeConfig
    relay: RelayConfig
    slash_commands: SlashCommandConfig

    @staticmethod
    def load(path: Path) -> "BotConfig":
        data = json.loads(path.read_text(encoding="utf-8"))

        discord_cfg = data["discord"]
        plugin_cfg = data["plugin_bridge"]
        relay_cfg = data.get("relay", {})
        slash_cfg = data.get("slash_commands", {})

        config = BotConfig(
            discord=DiscordConfig(
                token=discord_cfg["token"],
                guild_id=int(discord_cfg["guild_id"]),
                relay_channel_ids=[int(x) for x in discord_cfg.get("relay_channel_ids", [])],
                command_role_ids=[int(x) for x in discord_cfg.get("command_role_ids", [])],
                status_role_ids=[int(x) for x in discord_cfg.get("status_role_ids", [])],
                relay_to_game_enabled=bool(discord_cfg.get("relay_to_game_enabled", True)),
                sync_commands_globally=bool(discord_cfg.get("sync_commands_globally", False)),
            ),
            plugin_bridge=PluginBridgeConfig(
                base_url=str(plugin_cfg["base_url"]).rstrip("/"),
                shared_secret=str(plugin_cfg["shared_secret"]).strip(),
                request_timeout_seconds=max(int(plugin_cfg.get("request_timeout_seconds", 10)), 1),
            ),
            relay=RelayConfig(
                include_attachment_urls=bool(relay_cfg.get("include_attachment_urls", True)),
                include_jump_url=bool(relay_cfg.get("include_jump_url", False)),
                max_message_length=max(int(relay_cfg.get("max_message_length", 1800)), 32),
            ),
            slash_commands=SlashCommandConfig(
                enabled=bool(slash_cfg.get("enabled", True)),
                ephemeral_responses=bool(slash_cfg.get("ephemeral_responses", True)),
            ),
        )

        if not config.discord.token or config.discord.token == "replace-me":
            raise ValueError("discord.token must be configured")
        if config.discord.guild_id <= 0:
            raise ValueError("discord.guild_id must be configured")
        if not config.plugin_bridge.base_url.startswith(("http://", "https://")):
            raise ValueError("plugin_bridge.base_url must start with http:// or https://")
        if not config.plugin_bridge.shared_secret or config.plugin_bridge.shared_secret == "change-me":
            raise ValueError("plugin_bridge.shared_secret must be configured")

        return config


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
        return await self._request("GET", "/status", None)

    async def _request(self, method: str, suffix: str, payload: dict[str, Any] | None) -> dict[str, Any]:
        url = f"{self._config.plugin_bridge.base_url}{suffix}"
        headers = {
            "Authorization": f"Bearer {self._config.plugin_bridge.shared_secret}",
            "Content-Type": "application/json",
        }
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


class BedrockDiscordBridgeBot(discord.Client):
    def __init__(self, config: BotConfig) -> None:
        intents = discord.Intents.default()
        intents.guilds = True
        intents.guild_messages = True
        intents.message_content = True

        super().__init__(intents=intents)
        self.config = config
        self.bridge = PluginBridgeClient(config)
        self.tree = app_commands.CommandTree(self)
        self._register_commands()

    async def close(self) -> None:
        await self.bridge.close()
        await super().close()

    async def setup_hook(self) -> None:
        guild_object = discord.Object(id=self.config.discord.guild_id)
        if self.config.slash_commands.enabled:
            if self.config.discord.sync_commands_globally:
                await self.tree.sync()
                LOGGER.info("Synced slash commands globally.")
            else:
                self.tree.copy_global_to(guild=guild_object)
                await self.tree.sync(guild=guild_object)
                LOGGER.info("Synced slash commands to guild %s.", self.config.discord.guild_id)

    async def on_ready(self) -> None:
        LOGGER.info("Logged in as %s (%s)", self.user, getattr(self.user, "id", "unknown"))

    async def on_message(self, message: discord.Message) -> None:
        if not self.config.discord.relay_to_game_enabled:
            return
        if message.author.bot or message.webhook_id is not None:
            return
        if message.guild is None or message.guild.id != self.config.discord.guild_id:
            return
        if self.config.discord.relay_channel_ids and message.channel.id not in self.config.discord.relay_channel_ids:
            return

        content = message.content.strip()
        parts: list[str] = [content] if content else []
        if self.config.relay.include_attachment_urls and message.attachments:
            parts.extend(f"[attachment] {attachment.url}" for attachment in message.attachments)
        if self.config.relay.include_jump_url:
            parts.append(f"[jump] {message.jump_url}")

        relay_content = "\n".join(part for part in parts if part).strip()
        if not relay_content:
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
        except Exception as exc:  # noqa: BLE001
            LOGGER.exception("Failed to relay Discord message %s into Minecraft: %s", message.id, exc)

    def _register_commands(self) -> None:
        if not self.config.slash_commands.enabled:
            return

        @self.tree.command(name="mcstatus", description="Show the current Bedrock Discord bridge status.")
        async def mcstatus(interaction: discord.Interaction) -> None:
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
                f"Avatar base URL: `{status.get('avatar_base_url') or '<not configured>'}`",
            ]
            await interaction.followup.send("\n".join(lines), ephemeral=self.config.slash_commands.ephemeral_responses)

        @self.tree.command(name="mccommand", description="Execute a server command through the bridge.")
        @app_commands.describe(command="Command to execute without the leading slash.")
        async def mccommand(interaction: discord.Interaction, command: str) -> None:
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

        @self.tree.command(name="mcreloadbridge", description="Reload the Endstone bridge plugin configuration.")
        async def mcreloadbridge(interaction: discord.Interaction) -> None:
            if not self._is_authorized(interaction, self.config.discord.command_role_ids):
                await interaction.response.send_message("You are not allowed to use this command.", ephemeral=True)
                return

            await interaction.response.defer(ephemeral=self.config.slash_commands.ephemeral_responses)
            try:
                await self.bridge.post_command(actor_name=interaction.user.display_name, command="discordbridge reload")
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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the Bedrock Discord Bridge companion bot.")
    parser.add_argument("config", nargs="?", default="config.json", help="Path to the bot JSON config file.")
    return parser.parse_args()


async def async_main() -> None:
    args = parse_args()
    config = BotConfig.load(Path(args.config))

    logging.basicConfig(level=logging.INFO, format="[%(asctime)s] %(levelname)s %(name)s: %(message)s")

    client = BedrockDiscordBridgeBot(config)
    await client.start(config.discord.token)


def main() -> None:
    try:
        asyncio.run(async_main())
    except KeyboardInterrupt:
        LOGGER.info("Bot interrupted, shutting down.")


if __name__ == "__main__":
    main()
