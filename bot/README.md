# Bedrock Discord Bridge Bot

Companion Discord bot for the Endstone Bedrock Discord Bridge plugin.

## What it does

- Relays Discord channel messages into Minecraft through the plugin's secure local HTTP API
- Exposes slash commands for bridge status and remote server command execution
- Uses Discord gateway intents for inbound relay and keeps the plugin focused on the game-side runtime

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
- `plugin_bridge.base_url`
- `plugin_bridge.shared_secret`

Then run:

```bash
bedrock-discord-bridge-bot config.json
```

## Slash commands

- `/mcstatus`
- `/mccommand command:<text>`
- `/mcreloadbridge`

`/mccommand` and `/mcreloadbridge` can be restricted by `discord.command_role_ids`.
