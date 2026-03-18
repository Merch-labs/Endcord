# Bedrock Discord Bridge Bot

Companion Discord bot for the Endstone Bedrock Discord Bridge plugin.

## What it does

- Relays Discord channel messages into Minecraft through the plugin's secure local HTTP API
- Exposes slash commands for bridge status and remote server command execution
- Uses Discord gateway intents for inbound relay and keeps the plugin focused on the game-side runtime
- Reads a JSON config so relay scope, permissions, and plugin API details can be adjusted without code changes

## Setup

The intended runtime model is simple: keep the bot in the same local runtime environment as Endstone so `plugin_bridge.base_url` can stay on `127.0.0.1`.

```bash
cd bot
./scripts/bootstrap-local-runtime.sh
```

Fill in:

- `discord.token`
- `discord.relay_channel_ids`
- optionally `discord.guild_id` if you do not want it auto-derived from the first configured channel
- optionally `discord.outbound_channel_id` if Minecraft-to-Discord should use a different channel from inbound relay
- `discord.command_role_ids` if you want to restrict remote command usage
- `discord.status_role_ids` if you want to restrict `/mcstatus`
- `plugin_bridge.base_url`
- `plugin_bridge.shared_secret`

Then run:

```bash
./scripts/run-local-runtime.sh
```

Schema for editor validation is included at [config.schema.json](bot/config.schema.json).

## Config notes

- `plugin_bridge.base_url` should point at the plugin API root, usually `http://127.0.0.1:8089/bedrock-discord-bridge/api`.
- `plugin_bridge.shared_secret` must match `bot_bridge.shared_secret` in the plugin config.
- `discord.guild_id` can stay `0` and the bot will derive it from `discord.outbound_channel_id`, the first relay channel, or the system-message channel.
- `discord.auto_create_webhook` lets the bot create or reuse a webhook in the outbound channel, then push that URL into the plugin automatically. Keep `discord.webhook_name` free of the word `Discord`, because Discord rejects it in webhook names.
- `discord.outbound_channel_id` defaults to the first `discord.relay_channel_ids` entry when left at `0`.
- `discord.relay_channel_ids` can be left empty only if you set `discord.outbound_channel_id` and do not want Discord-to-Minecraft chat relay.
- `discord.sync_commands_globally` should usually stay `false` so command updates sync quickly to one guild.
- `plugin_bridge.configure_webhook_on_startup` is the easiest-distribution path: the bot provisions the webhook for the plugin so server owners do not need to paste a webhook URL by hand.
- `plugin_bridge.request_max_retries`, `plugin_bridge.request_retry_base_seconds`, and `plugin_bridge.request_retry_max_seconds` tune retry/backoff for idempotent bridge calls such as status checks, webhook binding, and draining queued system messages.
- `relay.message_template` supports `{author}`, `{content}`, `{attachments}`, `{jump_url}`, `{channel}`, `{guild}`, and `{message_url}` so you can decide exactly what the plugin receives.
- `relay.attachment_template` supports `{filename}`, `{url}`, and `{content_type}`.
- `relay.include_attachment_urls` and `relay.include_jump_url` decide whether those placeholders get populated.
- `relay.ignore_bot_messages` and `relay.ignore_webhook_messages` are useful if you run multiple Discord automations in the same relay channel.
- `presence.activity_text` supports `{server_name}`, `{minecraft_version}`, `{online_players}`, `{webhook_queue_depth}`, and `{guild_name}`.
- `presence.activity_type` supports `playing`, `streaming`, `listening`, `watching`, `competing`, and `custom`.
- `logging.level`, `logging.log_ignored_messages`, `logging.log_relay_successes`, and `logging.log_presence_updates` tune runtime verbosity.
- `system_messages.enabled` turns on bot-owned join/quit/death delivery from the plugin queue.
- `system_messages.channel_id` can target a dedicated channel, or you can leave it at `0` to fall back to the first `discord.relay_channel_ids` entry.
- `system_messages.failure_backoff_seconds` and `system_messages.max_backoff_seconds` keep the bot from hammering the plugin during restart windows.
- `system_messages.message_template` supports `{content}`, `{event}`, and `{player_name}`.
- `slash_commands.ephemeral_responses` controls whether command replies stay visible only to the caller.

## Slash commands

- `/mcstatus`
- `/mccommand command:<text>`
- `/mcreloadbridge`

`/mccommand` and `/mcreloadbridge` can be restricted by `discord.command_role_ids`.

## Colocated runtime

Run the bot in the same local runtime environment as the Endstone server so the bridge stays on `127.0.0.1` and there is no environment-specific decision to make in normal use.

- Bootstrap the venv and local editable install with [bootstrap-local-runtime.sh](bot/scripts/bootstrap-local-runtime.sh).
- Start it with [run-local-runtime.sh](bot/scripts/run-local-runtime.sh).
- The older [bootstrap-host.sh](bot/scripts/bootstrap-host.sh) and [run-host.sh](bot/scripts/run-host.sh) names are kept as compatibility wrappers.
- If you want it to start automatically on boot, adapt [bedrock-discord-bridge-bot.service.example](bot/systemd/bedrock-discord-bridge-bot.service.example) for your install path and enable it with `systemctl`.
