# Endcord Bot

Companion Discord bot for the Endstone Endcord plugin.

This bot is responsible for:

- Discord -> Minecraft relay
- slash commands
- webhook creation/reuse for Minecraft -> Discord
- optional bot-owned join/quit/death delivery

The normal runtime model is simple:

- keep the bot in the same local runtime environment as Endstone
- keep `plugin_bridge.base_url` on `127.0.0.1`
- let the bot auto-create the outbound webhook

## Quick Start

```bash
cd bot
./scripts/bootstrap-local-runtime.sh
cp config.json.example /path/to/bedrock_server/plugins/endcord/bot/config.json
./scripts/run-local-runtime.sh /path/to/bedrock_server/plugins/endcord/bot/config.json
```

Before running, fill in:

- `discord.token`
- `discord.relay_channel_ids`
- `plugin_bridge.shared_secret`

Optional but common:

- `discord.outbound_channel_id`
- `discord.guild_id`
- `discord.command_role_ids`
- `discord.status_role_ids`

Schema:
- [config.schema.json](config.schema.json)

## Most Important Settings

- `plugin_bridge.base_url`
  - usually `http://127.0.0.1:8089/endcord/api`
- `plugin_bridge.shared_secret`
  - must match `bot_bridge.shared_secret` in the plugin config
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

Filtering:

- `relay.ignore_bot_messages`
- `relay.ignore_webhook_messages`
- `discord.relay_to_game_enabled`

## Slash Commands

- `/status`
- `/command command:<text>`
- `/reloadbridge`

Role restrictions:

- `discord.command_role_ids`
- `discord.status_role_ids`

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
- `{minecraft_version}`
- `{online_players}`
- `{webhook_queue_depth}`
- `{guild_name}`

## System Messages

Optional bot-owned lifecycle delivery:

- `system_messages.enabled`
- `system_messages.channel_id`
- `system_messages.message_template`
- `system_messages.max_messages_per_poll`
- `system_messages.failure_backoff_seconds`
- `system_messages.max_backoff_seconds`

## Logging

- `logging.level`
- `logging.log_ignored_messages`
- `logging.log_relay_successes`
- `logging.log_presence_updates`

## Service Setup

If you want the bot to start automatically:

- adapt [endcord-bot.service.example](systemd/endcord-bot.service.example)
- enable it with `systemctl`

## Local Checks

```bash
python3 -m unittest discover -s bot/tests
python3 -m compileall bot/src
endcord-bot --help
```
