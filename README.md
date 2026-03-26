# Endcord

Two-way Minecraft Bedrock <-> Discord bridge for Endstone.

This project gives you:

- a Linux Endstone plugin (`.so`)
- a companion Discord bot
- JSON config files for both sides

The normal setup is:

1. put the plugin in your server `plugins/` folder
2. start the server once
3. bootstrap the bot into `plugins/endcord/bot/`
4. paste in your bot token and channel id
5. let the server start and stop the bot with the plugin

You do not need Docker-specific setup, tunnels, proxies, or self-hosted avatar hosting for the default path.

## If You Just Want It Working

Use this path if you are not trying to build or customize anything deeply.

### What you need

- an Endstone Bedrock server on Linux
- the plugin `.so`
- a Discord bot token
- a Discord channel id
- a shared secret you choose

### 1. Put the plugin in `plugins/`

Copy:

```text
endstone_endcord.so
```

into:

```text
path/to/bedrock_server/plugins/
```

If you are building from source, the build section is near the bottom of this README.

### 2. Start the server once

On first load, the plugin creates:

```text
path/to/bedrock_server/plugins/endcord/config.json
```

### 3. Edit the plugin config

You can start from:

```bash
cp config/config.json.example path/to/bedrock_server/plugins/endcord/config.json
```

The most important plugin settings are:

- `bot_bridge.shared_secret`
- `bot_bridge.allow_local_requests_only`
- `avatar.provider`
- `bot_bridge.inbound_chat_template`
- `integrated_bot.enabled`

For most users:

- leave `bot_bridge.allow_local_requests_only` as `true`
- leave `avatar.provider` as `tabavatars`
- leave `discord.allow_runtime_webhook_override` as `true`

Plugin schema:
- [config/config.schema.json](config/config.schema.json)

### 4. Set up the integrated bot config

Start the server once and Endcord will create:

- `plugins/endcord/bot/config.json`

Then edit:

- `discord.token`
- `discord.relay_channel_ids`

Usually you can leave these as-is:

- `discord.guild_id = 0`
- `discord.auto_create_webhook = true`

Bot schema:
- [bot/config.schema.json](bot/config.schema.json)

### 5. Start the server

Start or restart Endstone after filling in `plugins/endcord/bot/config.json`.
Endcord will start and stop the integrated Discord runtime with the plugin.

If you want to run the bot outside the server-managed path, you can still use:
- [run-local-runtime.sh](bot/scripts/run-local-runtime.sh)
- [endcord-bot.service.example](bot/systemd/endcord-bot.service.example)

### 6. What should happen

Once both pieces are running:

- Minecraft chat should appear in Discord
- Discord messages from your configured channel should appear in Minecraft
- player avatars should come from the configured avatar provider
- the bot should auto-create or reuse the outbound webhook if allowed

## What This Project Does

- Minecraft -> Discord relay through a webhook
- Discord -> Minecraft relay through a local bot API
- per-message Discord `username` and `avatar_url` for Minecraft-originated messages
- join, quit, and death relays
- slash commands:
  - `/status`
  - `/players`
  - `/ping`
  - `/command`
  - `/configreload`

## Common Things People Change

### Change how messages look

Plugin-side formatting:

- `discord.username_template`
- `discord.content_template`
- `discord.system_username_template`
- `discord.join_content_template`
- `discord.quit_content_template`
- `discord.death_content_template`
- `bot_bridge.inbound_chat_template`
- `managed_bot.enabled`
- `managed_bot.log_path`

Common plugin placeholders:

- Outbound Minecraft -> Discord: `{player}`, `{player_name}`, `{username}`, `{message}`, `{content}`, `{event_message}`, `{event}`, `{event_name}`, `{skin_id}`, `{xuid}`, `{uuid}`, `{uuid_nodashes}`, `{message_length}`, `{server}`, `{server_name}`, `{server_version}`, `{minecraft_version}`, `{protocol_version}`, `{online_players}`, `{max_players}`, `{player_slots_available}`, `{player_utilization_percent}`, `{game_port}`, `{game_port_v6}`, `{online_mode}`
- Inbound Discord -> Minecraft: `{author}`, `{author_name}`, `{author_id}`, `{content}`, `{message}`, `{content_length}`, `{channel}`, `{channel_name}`, `{channel_id}`, `{guild}`, `{guild_name}`, `{guild_id}`, `{message_url}`, `{jump_url}`, `{message_id}`, `{server}`, `{server_name}`, `{server_version}`, `{minecraft_version}`, `{protocol_version}`, `{online_players}`, `{max_players}`, `{player_slots_available}`, `{player_utilization_percent}`, `{game_port}`, `{game_port_v6}`, `{online_mode}`

Bot-side formatting:

- `relay.message_template`
- `relay.attachment_template`
- `relay.jump_url_template`

Common bot placeholders:

- Relay message templates: `{author}`, `{author_name}`, `{author_username}`, `{author_global_name}`, `{author_id}`, `{content}`, `{content_raw}`, `{content_length}`, `{attachments}`, `{attachments_count}`, `{jump_url}`, `{channel}`, `{channel_name}`, `{channel_id}`, `{channel_mention}`, `{guild}`, `{guild_name}`, `{guild_id}`, `{message_url}`, `{message_id}`
- System message templates: `{content}`, `{message}`, `{content_length}`, `{event}`, `{event_name}`, `{event_upper}`, `{event_title}`, `{player}`, `{player_name}`

### Change avatar provider

Supported providers:

- `tabavatars`
- `mcheads`
- `custom`

Relevant plugin settings:

- `avatar.provider`
- `avatar.provider_url_template`
- `avatar.provider_prefer_xuid`
- `avatar.provider_render_type`
- `avatar.provider_bedrock_username_prefix`
- `avatar.size`

### Turn relays on or off

Plugin relay toggles:

- `relay.minecraft_to_discord_enabled`
- `relay.chat_enabled`
- `relay.join_enabled`
- `relay.quit_enabled`
- `relay.death_enabled`

Bot-side message filters:

- `discord.relay_to_game_enabled`
- `relay.ignore_bot_messages`
- `relay.ignore_webhook_messages`

### Bot presence and bot-owned system messages

Presence:

- `presence.enabled`
- `presence.status`
- `presence.activity_type`
- `presence.activity_text`

Useful presence placeholders:

- `{server_name}`, `{server_version}`, `{minecraft_version}`, `{protocol_version}`, `{online_players}`, `{max_players}`, `{player_slots_available}`, `{player_utilization_percent}`, `{online_player_names}`, `{game_port}`, `{game_port_v6}`, `{online_mode}`, `{gateway_latency_ms}`, `{bridge_latency_ms}`, `{webhook_queue_depth}`, `{system_message_queue_depth}`, `{system_message_queue_max}`, `{webhook_configured}`, `{runtime_webhook_override_active}`, `{minecraft_to_discord_enabled}`, `{discord_to_minecraft_enabled}`, `{bot_system_messages_enabled}`, `{avatar_enabled}`, `{avatar_provider}`, `{bot_bridge_enabled}`, `{managed_bot_enabled}`, `{managed_bot_running}`, `{guild_name}`, `{guild_id}`

Optional bot-owned lifecycle delivery:

- `system_messages.enabled`
- `system_messages.channel_id`
- `system_messages.message_template`

### Turn slash commands on or off, or restrict them

Bot slash command settings:

- `slash_commands.enabled`
- `slash_commands.status.enabled`
- `slash_commands.players.enabled`
- `slash_commands.ping.enabled`
- `slash_commands.command.enabled`
- `slash_commands.configreload.enabled`
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

## Troubleshooting

### Minecraft -> Discord works, but Discord -> Minecraft does not

Check:

- the bot is running
- the bot config is at `plugins/endcord/bot/config.json`
- the managed bot executable exists at `plugins/endcord/bot/.venv/bin/endcord-bot`
- `plugin_bridge.base_url` points at `127.0.0.1`
- `discord.relay_channel_ids` contains the right channel

### Avatars are not showing correctly

Check:

- `avatar.enabled` is `true`
- `avatar.provider` is valid
- the provider you chose can resolve that Bedrock player identity

### The bot does not create the webhook

Check:

- `discord.auto_create_webhook = true`
- `plugin_bridge.configure_webhook_on_startup = true`
- the bot has permission to manage webhooks in the outbound channel
- `discord.webhook_name` does not contain the word `Discord`

## Recommended Runtime Model

Keep the setup simple:

- Endstone loads the plugin inside the Bedrock server process
- the companion bot runs beside it in the same local runtime environment
- the bot talks to the plugin over `http://127.0.0.1:<port>/endcord/api`

That gives you:

- one plugin
- one bot
- one shared secret
- one local bridge URL

## Optional Advanced Settings

These are useful, but most people do not need to touch them on day one.

- `queue.max_size`
- `queue.max_attempts`
- `queue.retry_delay_ms`
- `bot_bridge.allowed_remote_addresses`
- `plugin_bridge.request_max_retries`
- `plugin_bridge.request_retry_base_seconds`
- `plugin_bridge.request_retry_max_seconds`
- legacy fallback role lists:
  - `discord.command_role_ids`
  - `discord.status_role_ids`
- `discord.outbound_channel_id`

## Build From Source

### Local Linux build

```bash
./scripts/setup-local-libcxx.sh
cmake --preset linux-clang-local-libcxx
cmake --build --preset build-local
```

Expected artifact:

```text
build-local/endstone_endcord.so
```

### Debian 12-compatible build

If your runtime matches the official `endstone/endstone` Debian 12 image:

```bash
./scripts/build-plugin-debian12.sh
```

Artifact:

```text
build-debian12/endstone_endcord.so
```

### Build notes

- Endstone requires Clang on Linux
- Endstone forces `-stdlib=libc++`
- plain `g++` builds are expected to fail
- current Endstone pin: `v0.11.2`

## Developer Checks

Run these from the repository root:

```bash
cmake --preset linux-clang-local-libcxx
cmake --build --preset build-local
ctest --test-dir build-local --output-on-failure
python3 -m unittest discover -s bot/tests
python3 -m compileall bot/src
```

## Project Files

- plugin config example: [config/config.json.example](config/config.json.example)
- plugin config schema: [config/config.schema.json](config/config.schema.json)
- bot README: [bot/README.md](bot/README.md)
- bot config schema: [bot/config.schema.json](bot/config.schema.json)
- service example: [bot/systemd/endcord-bot.service.example](bot/systemd/endcord-bot.service.example)

## Publishing

- License: [LICENSE](LICENSE)
- Changelog: [CHANGELOG.md](CHANGELOG.md)
- Third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
- Contribution guide: [CONTRIBUTING.md](CONTRIBUTING.md)
- Security guidance: [SECURITY.md](SECURITY.md)

Release checklist:

1. Build the plugin.
2. Run `ctest`.
3. Run bot unit tests.
4. Run `python3 -m compileall bot/src`.
5. Review config examples and schemas.
6. Update [CHANGELOG.md](CHANGELOG.md).
7. Tag only after CI passes on `master`.
