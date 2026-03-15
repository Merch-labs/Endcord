# Bedrock Discord Bridge Bot

Companion Discord bot for the Endstone Bedrock Discord Bridge plugin.

## What it does

- Relays Discord channel messages into Minecraft through the plugin's secure local HTTP API
- Exposes slash commands for bridge status and remote server command execution
- Uses Discord gateway intents for inbound relay and keeps the plugin focused on the game-side runtime
- Reads a JSON config so relay scope, permissions, and plugin API details can be adjusted without code changes

## Setup

```bash
cd bot
python3 -m venv .venv
. .venv/bin/activate
pip install -e .
cp config.json.example config.json
```

Fill in:

- `discord.token`
- `discord.guild_id`
- `discord.relay_channel_ids`
- `discord.command_role_ids` if you want to restrict remote command usage
- `discord.status_role_ids` if you want to restrict `/mcstatus`
- `plugin_bridge.base_url`
- `plugin_bridge.shared_secret`

Then run:

```bash
bedrock-discord-bridge-bot config.json
```

Schema for editor validation is included at [config.schema.json](bot/config.schema.json).

## Config notes

- `plugin_bridge.base_url` should point at the plugin API root, usually `http://127.0.0.1:8089/bedrock-discord-bridge/api`.
- `plugin_bridge.shared_secret` must match `bot_bridge.shared_secret` in the plugin config.
- `discord.relay_channel_ids` can be left empty to allow all text channels in the configured guild.
- `discord.sync_commands_globally` should usually stay `false` so command updates sync quickly to one guild.
- `relay.include_attachment_urls` appends uploaded file URLs into the Minecraft relay message.
- `relay.include_jump_url` appends the Discord jump link into the Minecraft relay message.
- `slash_commands.ephemeral_responses` controls whether command replies stay visible only to the caller.

## Slash commands

- `/mcstatus`
- `/mccommand command:<text>`
- `/mcreloadbridge`

`/mccommand` and `/mcreloadbridge` can be restricted by `discord.command_role_ids`.

## Recommended runtime

Run the bot on the same host as the Bedrock server when the plugin keeps `bot_bridge.allow_local_requests_only` enabled. That gives you a simple and safer default: Discord reaches the bot, and only the local bot reaches the plugin API.
