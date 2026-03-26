# Endcord Bot

Legacy standalone Discord bot for the Endstone Endcord plugin.

The `integrate-bot-into-plugin` branch now embeds the Discord runtime directly inside the C++ plugin, so normal users on this branch do not need the standalone Python bot. This folder is kept as a reference/runtime fallback while the integrated path settles.

This bot is responsible for:

- Discord -> Minecraft relay
- slash commands
- webhook creation/reuse for Minecraft -> Discord
- optional bot-owned join/quit/death delivery

The normal runtime model is simple:

- bootstrap the bot into `plugins/endcord/bot/`
- let Endcord start and stop it with the server
- keep `plugin_bridge.base_url` on `127.0.0.1`
- let the bot auto-create the outbound webhook

## Quick Start

```bash
cd bot
./scripts/bootstrap-local-runtime.sh /path/to/bedrock_server/plugins/endcord/bot/config.json
```

Before running, fill in:

- `discord.token`
- `discord.relay_channel_ids`

Optional but common:

- `discord.outbound_channel_id`
- `discord.guild_id`
- `slash_commands.command.role_ids`
- `slash_commands.status.role_ids`

Schema:
- [config.schema.json](config.schema.json)

## Most Important Settings

- `plugin_bridge.base_url`
  - usually `http://127.0.0.1:8089/endcord/api`
- `bot_bridge.shared_secret` in the plugin config
  - the bot reads it automatically from `plugins/endcord/config.json`
- `managed_bot.enabled` in the plugin config
  - lets the plugin start and stop the bot automatically
- `discord.auto_create_webhook`
  - lets the bot provision the webhook automatically
- `plugin_bridge.configure_webhook_on_startup`
  - pushes the webhook into the plugin so users do not need to paste one manually
- `discord.guild_id`
  - can stay `0` if you want the bot to derive it
- `discord.outbound_channel_id`
  - defaults to the first relay channel if left at `0`

Webhook naming note:

- keep `discord.webhook_name` free of the word `Discord`, because Discord rejects it in webhook names

## Relay Customization

Discord -> Minecraft formatting:

- `relay.message_template`
- `relay.attachment_template`
- `relay.jump_url_template`
- `relay.join_separator`
- `relay.max_message_length`

Supported placeholders in relay templates:

- Message templates: `{author}`, `{author_name}`, `{author_username}`, `{author_global_name}`, `{author_id}`, `{content}`, `{content_raw}`, `{content_length}`, `{attachments}`, `{attachments_count}`, `{jump_url}`, `{channel}`, `{channel_name}`, `{channel_id}`, `{channel_mention}`, `{guild}`, `{guild_name}`, `{guild_id}`, `{message_url}`, `{message_id}`
- Attachment templates: `{filename}`, `{url}`, `{content_type}`
- Jump URL templates: `{url}`

Filtering:

- `relay.ignore_bot_messages`
- `relay.ignore_webhook_messages`
- `discord.relay_to_game_enabled`

## Slash Commands

- `/status`
- `/players`
- `/ping`
- `/command command:<text>`
- `/configreload`

Role restrictions:

- `slash_commands.status.admin_only`
- `slash_commands.players.admin_only`
- `slash_commands.ping.admin_only`
- `slash_commands.command.admin_only`
- `slash_commands.configreload.admin_only`
- `slash_commands.status.role_ids`
- `slash_commands.players.role_ids`
- `slash_commands.ping.role_ids`
- `slash_commands.command.role_ids`
- `slash_commands.configreload.role_ids`

Per-command toggles:

- `slash_commands.status.enabled`
- `slash_commands.players.enabled`
- `slash_commands.ping.enabled`
- `slash_commands.command.enabled`
- `slash_commands.configreload.enabled`

Legacy fallback role lists still load if you already use them:

- `discord.command_role_ids`
- `discord.status_role_ids`

Default policy:

- `/players`, `/status`, and `/ping` are open by default
- `/command` and `/configreload` are admin-only by default

## Presence

Useful presence settings:

- `presence.enabled`
- `presence.status`
- `presence.activity_type`
- `presence.activity_text`
- `presence.fallback_text`
- `presence.update_interval_seconds`

Supported placeholders in `presence.activity_text`:

- `{server_name}`
- `{server_version}`
- `{minecraft_version}`
- `{protocol_version}`
- `{online_players}`
- `{max_players}`
- `{player_slots_available}`
- `{player_utilization_percent}`
- `{online_player_names}`
- `{game_port}`
- `{game_port_v6}`
- `{online_mode}`
- `{gateway_latency_ms}`
- `{bridge_latency_ms}`
- `{webhook_queue_depth}`
- `{system_message_queue_depth}`
- `{system_message_queue_max}`
- `{webhook_configured}`
- `{runtime_webhook_override_active}`
- `{minecraft_to_discord_enabled}`
- `{discord_to_minecraft_enabled}`
- `{bot_system_messages_enabled}`
- `{avatar_enabled}`
- `{avatar_provider}`
- `{bot_bridge_enabled}`
- `{managed_bot_enabled}`
- `{managed_bot_running}`
- `{guild_name}`
- `{guild_id}`

## System Messages

Optional bot-owned lifecycle delivery:

- `system_messages.enabled`
- `system_messages.channel_id`
- `system_messages.message_template`
- `system_messages.max_messages_per_poll`
- `system_messages.failure_backoff_seconds`
- `system_messages.max_backoff_seconds`

Supported placeholders in `system_messages.message_template`:

- `{content}`
- `{message}`
- `{content_length}`
- `{event}`
- `{event_name}`
- `{event_upper}`
- `{event_title}`
- `{player}`
- `{player_name}`

## Logging

- `logging.level`
- `logging.log_ignored_messages`
- `logging.log_relay_successes`
- `logging.log_presence_updates`

## Manual Or Service-Based Runtime

If you do not want the plugin to manage the bot process:

- run [run-local-runtime.sh](scripts/run-local-runtime.sh) manually
- or adapt [endcord-bot.service.example](systemd/endcord-bot.service.example)

## Local Checks

```bash
python3 -m unittest discover -s bot/tests
python3 -m compileall bot/src
endcord-bot --help
```
