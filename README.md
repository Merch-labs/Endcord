# Endcord

Endcord is a single Endstone plugin for Minecraft Bedrock on Linux that relays chat between Minecraft and Discord.

It now runs everything inside the plugin:

- Minecraft -> Discord webhook relay
- Discord -> Minecraft relay
- Discord slash commands
- embedded bot presence updates
- webhook auto-provisioning

You do not need Python, Docker, a separate bot service, or any extra helper container for the normal setup.

The current integrated setup is tested on Endstone `0.11.2` and Minecraft Bedrock `1.26.3.1`.

## Quick Start

### What you need

- an Endstone server on Linux
- `endstone_endcord.so`
- a Discord bot token
- a Discord channel id

### 1. Install the plugin

Put the built plugin in your server `plugins/` folder:

```text
endstone_endcord.so
```

### 2. Start the server once

On first start, Endcord creates:

```text
plugins/endcord/config.json
```

### 3. Edit the config

You can start from:

```bash
cp config/config.json.example /path/to/bedrock_server/plugins/endcord/config.json
```

The minimum fields most people need to change are:

- `bot.discord.token`
- `bot.discord.relay_channel_ids`

Good defaults to keep:

- `avatar.provider = "tabavatars"`
- `bot.discord.auto_create_webhook = true`
- `bot.discord.guild_id = 0`
- `bot_bridge.inbound_chat_template = "[Discord] <{author}> {content}"`

Optional:

- `discord.webhook_url`
  Use this only if you want to pin a specific webhook manually. In the normal setup, Endcord can create or reuse one automatically.

### 4. Restart the server

After restart, Endcord should:

- send Minecraft chat to Discord
- send Discord channel messages into Minecraft
- register slash commands in Discord
- update the bot presence
- auto-create or reuse the outbound webhook if the bot can manage webhooks

## Commands

### Minecraft

- `/endcord`
- `/endcord status`
- `/endcord reload`

### Discord slash commands

- `/status`
- `/players`
- `/ping`
- `/command`
- `/configreload`

## Main Config Areas

### Minecraft -> Discord

- `discord.username_template`
- `discord.content_template`
- `discord.system_username_template`
- `discord.join_content_template`
- `discord.quit_content_template`
- `discord.death_content_template`

### Discord -> Minecraft

- `bot_bridge.inbound_chat_enabled`
- `bot_bridge.inbound_chat_template`
- `bot_bridge.command_enabled`
- `bot_bridge.outbound_system_messages_enabled`

### Avatar provider

- `avatar.enabled`
- `avatar.provider`
- `avatar.provider_url_template`
- `avatar.provider_prefer_xuid`
- `avatar.provider_render_type`
- `avatar.provider_bedrock_username_prefix`
- `avatar.size`

Supported providers:

- `tabavatars`
- `mcheads`
- `custom`

### Integrated Discord runtime

- `bot.discord.*`
- `bot.integration.*`
- `bot.relay.*`
- `bot.slash_commands.*`
- `bot.presence.*`
- `bot.logging.*`
- `bot.system_messages.*`

## Placeholder Highlights

### Minecraft -> Discord

- `{player}`
- `{message}`
- `{event_message}`
- `{server_name}`
- `{server_version}`
- `{minecraft_version}`
- `{online_players}`
- `{max_players}`
- `{xuid}`
- `{uuid}`

### Discord -> Minecraft

- `{author}`
- `{content}`
- `{channel}`
- `{guild}`
- `{message_url}`
- `{jump_url}`
- `{server_name}`
- `{online_players}`

### Discord relay / presence

- `{author}`
- `{content}`
- `{attachments}`
- `{jump_url}`
- `{online_player_names}`
- `{gateway_latency_ms}`
- `{bridge_latency_ms}`
- `{webhook_queue_depth}`
- `{system_message_queue_depth}`

## Troubleshooting

### Minecraft -> Discord works, but Discord -> Minecraft does not

Check:

- `bot.discord.token` is set correctly
- `bot.discord.relay_channel_ids` contains the correct channel
- the bot has Message Content intent enabled
- `bot_bridge.inbound_chat_enabled` is `true`

### The webhook is not being created

Check:

- the bot is in the target server
- the bot can manage webhooks in the channel
- `bot.discord.auto_create_webhook` is `true`
- `discord.allow_runtime_webhook_override` is `true`

### Avatars are not showing correctly

Check:

- `avatar.enabled` is `true`
- `avatar.provider` is valid
- the provider you selected supports the Bedrock identity data available for that player

## Build From Source

For local Linux development:

```bash
cmake --preset linux-clang-local-libcxx
cmake --build --preset build-local -j4
ctest --test-dir build-local --output-on-failure
```

Expected local artifact:

```text
build-local/endstone_endcord.so
```

For deployment to Dockerized Endstone servers or other hosts that need a Debian 12 compatible build:

```bash
./scripts/build-plugin-debian12.sh
```

Expected deployment artifact:

```text
build-debian12/endstone_endcord.so
```

## Files

- plugin config example: [config/config.json.example](config/config.json.example)
- plugin config schema: [config/config.schema.json](config/config.schema.json)
- CI workflow: [.github/workflows/ci.yml](.github/workflows/ci.yml)
