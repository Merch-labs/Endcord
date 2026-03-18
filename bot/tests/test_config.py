from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from bridge_bot.config import BotConfig  # noqa: E402


class BotConfigTests(unittest.TestCase):
    def write_config(self, payload: dict) -> Path:
        temp_dir = Path(tempfile.mkdtemp(prefix="bridge-bot-config-"))
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
                        "shared_secret": "secret",
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
                    "shared_secret": "secret",
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
                    "shared_secret": "secret",
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


if __name__ == "__main__":
    unittest.main()
