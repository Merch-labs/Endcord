from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from bridge_bot.config import BotConfig  # noqa: E402


class BotConfigTests(unittest.TestCase):
    def write_config(self, payload: dict, plugin_secret: str | None = "secret") -> Path:
        temp_root = Path(tempfile.mkdtemp(prefix="bridge-bot-config-"))
        temp_dir = temp_root / "endcord" / "bot"
        temp_dir.mkdir(parents=True, exist_ok=True)
        if plugin_secret is not None:
            plugin_config_path = temp_root / "endcord" / "config.json"
            plugin_config_path.write_text(
                json.dumps({"bot_bridge": {"shared_secret": plugin_secret}}),
                encoding="utf-8",
            )
        path = temp_dir / "config.json"
        path.write_text(json.dumps(payload), encoding="utf-8")
        return path

    def test_loads_minimal_valid_config(self) -> None:
        config = BotConfig.load(
            self.write_config(
                {
                    "discord": {
                        "token": "token",
                        "guild_id": 0,
                        "relay_channel_ids": [123],
                        "outbound_channel_id": 0,
                        "command_role_ids": [],
                        "status_role_ids": [],
                        "relay_to_game_enabled": True,
                        "sync_commands_globally": False,
                        "auto_create_webhook": True,
                        "webhook_name": "Bridge",
                    },
                    "plugin_bridge": {
                        "base_url": "http://127.0.0.1:8089/endcord/api",
                        "request_timeout_seconds": 10,
                        "configure_webhook_on_startup": True,
                        "request_max_retries": 3,
                        "request_retry_base_seconds": 1.5,
                        "request_retry_max_seconds": 15.0,
                    },
                    "relay": {},
                    "slash_commands": {},
                    "presence": {},
                    "logging": {},
                    "system_messages": {},
                }
            )
        )

        self.assertEqual(config.discord.guild_id, 0)
        self.assertEqual(config.discord.relay_channel_ids, [123])
        self.assertTrue(config.discord.auto_create_webhook)
        self.assertEqual(config.plugin_bridge.request_max_retries, 3)
        self.assertTrue(config.slash_commands.status.enabled)
        self.assertFalse(config.slash_commands.status.admin_only)
        self.assertFalse(config.slash_commands.players.admin_only)
        self.assertFalse(config.slash_commands.ping.admin_only)
        self.assertTrue(config.slash_commands.command.admin_only)
        self.assertTrue(config.slash_commands.configreload.admin_only)
        self.assertEqual(config.slash_commands.players.role_ids, [])
        self.assertEqual(config.slash_commands.command.role_ids, [])

    def test_requires_channel_when_guild_is_derived(self) -> None:
        path = self.write_config(
            {
                "discord": {
                    "token": "token",
                    "guild_id": 0,
                    "relay_channel_ids": [],
                    "outbound_channel_id": 0,
                    "command_role_ids": [],
                    "status_role_ids": [],
                    "relay_to_game_enabled": True,
                    "sync_commands_globally": False,
                    "auto_create_webhook": True,
                    "webhook_name": "Bridge",
                },
                "plugin_bridge": {
                    "base_url": "http://127.0.0.1:8089/endcord/api",
                    "request_timeout_seconds": 10,
                    "configure_webhook_on_startup": True,
                    "request_max_retries": 3,
                    "request_retry_base_seconds": 1.5,
                    "request_retry_max_seconds": 15.0,
                },
            }
        )

        with self.assertRaisesRegex(ValueError, "configure at least one"):
            BotConfig.load(path)

    def test_validates_presence_status(self) -> None:
        path = self.write_config(
            {
                "discord": {
                    "token": "token",
                    "guild_id": 1,
                    "relay_channel_ids": [123],
                    "outbound_channel_id": 0,
                    "command_role_ids": [],
                    "status_role_ids": [],
                    "relay_to_game_enabled": True,
                    "sync_commands_globally": False,
                    "auto_create_webhook": True,
                    "webhook_name": "Bridge",
                },
                "plugin_bridge": {
                    "base_url": "http://127.0.0.1:8089/endcord/api",
                    "request_timeout_seconds": 10,
                    "configure_webhook_on_startup": True,
                    "request_max_retries": 3,
                    "request_retry_base_seconds": 1.5,
                    "request_retry_max_seconds": 15.0,
                },
                "presence": {"status": "busy"},
            }
        )

        with self.assertRaisesRegex(ValueError, "presence.status"):
            BotConfig.load(path)

    def test_loads_per_command_slash_command_settings(self) -> None:
        config = BotConfig.load(
            self.write_config(
                {
                    "discord": {
                        "token": "token",
                        "guild_id": 1,
                        "relay_channel_ids": [123],
                        "outbound_channel_id": 0,
                        "command_role_ids": [1001],
                        "status_role_ids": [1002],
                        "relay_to_game_enabled": True,
                        "sync_commands_globally": False,
                        "auto_create_webhook": True,
                        "webhook_name": "Endcord",
                    },
                    "plugin_bridge": {
                        "base_url": "http://127.0.0.1:8089/endcord/api",
                        "request_timeout_seconds": 10,
                        "configure_webhook_on_startup": True,
                        "request_max_retries": 3,
                        "request_retry_base_seconds": 1.5,
                        "request_retry_max_seconds": 15.0,
                    },
                    "slash_commands": {
                        "enabled": True,
                        "ephemeral_responses": False,
                        "status": {"enabled": True, "admin_only": False, "role_ids": [2001]},
                        "command": {"enabled": False, "admin_only": True, "role_ids": [2002]},
                        "players": {"enabled": True, "admin_only": False, "role_ids": [2003]},
                        "ping": {"enabled": True, "admin_only": False, "role_ids": [2004]},
                        "configreload": {"enabled": True, "admin_only": True, "role_ids": [2005]},
                    },
                }
            )
        )

        self.assertFalse(config.slash_commands.ephemeral_responses)
        self.assertFalse(config.slash_commands.status.admin_only)
        self.assertEqual(config.slash_commands.status.role_ids, [2001])
        self.assertFalse(config.slash_commands.command.enabled)
        self.assertTrue(config.slash_commands.command.admin_only)
        self.assertEqual(config.slash_commands.players.role_ids, [2003])
        self.assertFalse(config.slash_commands.players.admin_only)
        self.assertEqual(config.slash_commands.ping.role_ids, [2004])
        self.assertFalse(config.slash_commands.ping.admin_only)
        self.assertEqual(config.slash_commands.configreload.role_ids, [2005])
        self.assertTrue(config.slash_commands.configreload.admin_only)

    def test_slash_command_roles_fall_back_to_legacy_lists(self) -> None:
        config = BotConfig.load(
            self.write_config(
                {
                    "discord": {
                        "token": "token",
                        "guild_id": 1,
                        "relay_channel_ids": [123],
                        "outbound_channel_id": 0,
                        "command_role_ids": [3001],
                        "status_role_ids": [3002],
                        "relay_to_game_enabled": True,
                        "sync_commands_globally": False,
                        "auto_create_webhook": True,
                        "webhook_name": "Endcord",
                    },
                    "plugin_bridge": {
                        "base_url": "http://127.0.0.1:8089/endcord/api",
                        "request_timeout_seconds": 10,
                        "configure_webhook_on_startup": True,
                        "request_max_retries": 3,
                        "request_retry_base_seconds": 1.5,
                        "request_retry_max_seconds": 15.0,
                    },
                    "slash_commands": {
                        "enabled": True,
                        "ephemeral_responses": True
                    },
                }
            )
        )

        self.assertEqual(config.slash_commands.status.role_ids, [3002])
        self.assertEqual(config.slash_commands.command.role_ids, [3001])
        self.assertEqual(config.slash_commands.ping.role_ids, [3002])
        self.assertEqual(config.slash_commands.configreload.role_ids, [3001])
        self.assertFalse(config.slash_commands.status.admin_only)
        self.assertTrue(config.slash_commands.command.admin_only)
        self.assertFalse(config.slash_commands.players.admin_only)
        self.assertFalse(config.slash_commands.ping.admin_only)
        self.assertTrue(config.slash_commands.configreload.admin_only)

    def test_uses_explicit_shared_secret_when_present(self) -> None:
        config = BotConfig.load(
            self.write_config(
                {
                    "discord": {
                        "token": "token",
                        "guild_id": 1,
                        "relay_channel_ids": [123],
                        "outbound_channel_id": 0,
                        "command_role_ids": [],
                        "status_role_ids": [],
                        "relay_to_game_enabled": True,
                        "sync_commands_globally": False,
                        "auto_create_webhook": True,
                        "webhook_name": "Endcord",
                    },
                    "plugin_bridge": {
                        "base_url": "http://127.0.0.1:8089/endcord/api",
                        "shared_secret": "explicit-secret",
                        "request_timeout_seconds": 10,
                        "configure_webhook_on_startup": True,
                        "request_max_retries": 3,
                        "request_retry_base_seconds": 1.5,
                        "request_retry_max_seconds": 15.0,
                    },
                },
                plugin_secret="other-secret",
            )
        )

        self.assertEqual(config.plugin_bridge.shared_secret, "explicit-secret")

    def test_requires_discoverable_shared_secret_when_not_explicit(self) -> None:
        path = self.write_config(
            {
                "discord": {
                    "token": "token",
                    "guild_id": 1,
                    "relay_channel_ids": [123],
                    "outbound_channel_id": 0,
                    "command_role_ids": [],
                    "status_role_ids": [],
                    "relay_to_game_enabled": True,
                    "sync_commands_globally": False,
                    "auto_create_webhook": True,
                    "webhook_name": "Endcord",
                },
                "plugin_bridge": {
                    "base_url": "http://127.0.0.1:8089/endcord/api",
                    "request_timeout_seconds": 10,
                    "configure_webhook_on_startup": True,
                    "request_max_retries": 3,
                    "request_retry_base_seconds": 1.5,
                    "request_retry_max_seconds": 15.0,
                },
            },
            plugin_secret=None,
        )

        with self.assertRaisesRegex(ValueError, "shared_secret"):
            BotConfig.load(path)


if __name__ == "__main__":
    unittest.main()
