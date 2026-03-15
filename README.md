# Bedrock Discord Bridge

Linux-ready Endstone C++ plugin plus companion Discord bot for a two-way Minecraft Bedrock bridge with JSON configuration, per-player webhook identity, rendered skin-head avatars, built-in avatar hosting, and a secure local bot API.

## What is included

- Endstone C++ plugin build pinned to `v0.11.2`
- Linux build flow that works without sudo by bootstrapping `libc++` locally
- Async Discord webhook worker so chat events do not block on network I/O
- Rate-limit aware retry path for `429` and transient webhook failures
- Bedrock skin face plus hat-overlay rendering to PNG
- Local avatar cache under the plugin data folder
- Built-in HTTP server for serving avatar PNGs
- Secure local bot bridge API for Discord-to-Minecraft relay and remote command execution
- JSON config with nested sections for Discord, relay toggles, logging, queue, avatar hosting, and bot bridge settings
- Companion Discord bot with its own JSON config and slash commands
- Customizable bot presence plus configurable Discord-to-Minecraft message formatting
- `/discordbridge status` and `/discordbridge reload`

## Build on Linux

The current CMake project is pinned to Endstone `v0.11.2`.

On Ubuntu hosts where you do not have sudo, bootstrap the required `libc++` toolchain locally first:

```bash
./scripts/setup-local-libcxx.sh
```

Then configure and build with the checked-in preset:

```bash
cmake --preset linux-clang-local-libcxx
cmake --build --preset build-local
```

To build a plugin binary that matches the official `endstone/endstone` Debian 12 Docker image and its `glibc 2.36`, use:

```bash
./scripts/build-plugin-debian12.sh
```

That produces:

```text
build-debian12/endstone_bedrock_discord_bridge.so
```

Note: Endstone requires Clang on Linux and forces `-stdlib=libc++`, so plain `g++` builds are expected to fail.

Expected Linux artifact name:

```text
build-local/endstone_bedrock_discord_bridge.so
```

## Config

On first run, the plugin writes a default JSON config to:

```bash
path/to/bedrock_server/plugins/bedrock_discord_bridge/config.json
```

You can also start from:

```bash
cp config/config.json.example path/to/bedrock_server/plugins/bedrock_discord_bridge/config.json
```

Schema for editor validation is included at [config/config.schema.json](config/config.schema.json).

Admin commands:

- `/discordbridge status`
- `/discordbridge reload`

Config notes:

- `discord.webhook_url` is required for Minecraft-to-Discord relay.
- `discord.username_template`, `discord.content_template`, `discord.system_username_template`, and the join/quit/death templates support `{player}`, `{message}`, `{event_message}`, `{event}`, `{skin_id}`, and `{server}`.
- `relay.minecraft_to_discord_enabled` is the master outbound switch, and `relay.chat_enabled`, `relay.join_enabled`, `relay.quit_enabled`, and `relay.death_enabled` let you tune which Minecraft events reach Discord.
- `discord.use_player_avatar_for_system_messages` controls whether join/quit/death webhook posts reuse the player head avatar.
- `avatar.enabled` controls skin-head rendering.
- `avatar.public_base_url` is a direct override if you already host the avatar cache elsewhere.
- `avatar.http_server` can serve the avatar cache directly from the plugin.
- If `avatar.http_server.public_base_url` is set, the plugin uses it to build Discord `avatar_url` values.
- If `avatar.public_base_url` and `avatar.http_server.public_base_url` are both empty, the plugin derives a local base URL only when `bind_host` is not a wildcard address.
- `queue.max_size`, `queue.max_attempts`, and timeout values control webhook behavior under load.
- `logging.log_filtered_events`, `logging.log_webhook_successes`, `logging.log_http_requests`, and the avatar/bot logging flags let you dial the plugin between quiet production mode and detailed troubleshooting.
- `bot_bridge.enabled` turns on the local HTTP API used by the companion Discord bot.
- `bot_bridge.shared_secret` must match the bot config exactly.
- `bot_bridge.api_route_prefix` must line up with the bot's `plugin_bridge.base_url`.
- `bot_bridge.allow_local_requests_only` should stay `true` unless you intentionally expose the API behind another trusted hop.
- `bot_bridge.inbound_chat_template` supports `{author}`, `{content}`, `{channel}`, `{guild}`, `{message_url}`, and `{server}`.
- `bot_bridge.inbound_chat_max_length` clamps how much Discord content the plugin will project into Minecraft chat.

## Avatar hosting

Discord webhook `avatar_url` must be a URL Discord can fetch. This plugin generates and caches PNGs locally, then maps them to:

```text
<effective-avatar-base-url>/<skin-hash>.png
```

Practical production path:

1. Enable `avatar.http_server`.
2. Expose its route publicly through a reverse proxy or direct port forward.
3. Set `avatar.http_server.public_base_url` to the public URL for that route.
4. Let the plugin render and reuse cached head icons by skin hash.

This keeps the plugin simple and fast while still matching the mature webhook-avatar pattern used by established Discord bridges.

## Recommended Discord architecture

Use a hybrid model:

- Bot token connection for Discord-to-Minecraft chat, slash commands, admin workflows, and channel event handling
- Discord webhook for Minecraft-to-Discord chat so each message can set a player-specific `username` and `avatar_url`

Practical avatar path:

1. Read Bedrock skin RGBA data from `player.getSkin().getImage()`.
2. Extract the face region and hat/overlay region into a square head render.
3. Write the PNG to the local avatar cache.
4. Publish that cache directory behind a stable public URL or the built-in avatar HTTP server.
5. Reuse the resulting `avatar_url` for subsequent webhook messages until the skin hash changes.

Rate-limit strategy:

- Queue outbound webhook posts instead of sending inline on the chat event thread
- Respect Discord `429` responses and `Retry-After`
- Group local throttling by `X-RateLimit-Bucket`
- Cache avatar URLs aggressively so avatar renders are much rarer than chat sends
- Retry transient webhook failures with bounded backoff instead of blocking the server thread

## Companion bot

The repo now includes a companion bot under [bot/README.md](bot/README.md).

Quick start:

```bash
cd bot
python3 -m venv .venv
. .venv/bin/activate
pip install -e .
cp config.json.example config.json
bedrock-discord-bridge-bot config.json
```

Bot config highlights:

- `discord.relay_channel_ids` limits which Discord channels relay into Minecraft.
- `discord.command_role_ids` and `discord.status_role_ids` control who can use slash commands.
- `plugin_bridge.base_url` should match `http://127.0.0.1:<port><api_route_prefix>`.
- `relay.message_template`, `relay.attachment_template`, `relay.jump_url_template`, and `relay.join_separator` control how Discord messages are projected into Minecraft before the plugin wraps them with `bot_bridge.inbound_chat_template`.
- `relay.ignore_bot_messages` and `relay.ignore_webhook_messages` let you decide whether other automation can talk back into Minecraft.
- `presence.activity_text` supports `{server_name}`, `{minecraft_version}`, `{online_players}`, `{webhook_queue_depth}`, and `{guild_name}`.
- `logging.level`, `logging.log_ignored_messages`, `logging.log_relay_successes`, and `logging.log_presence_updates` control bot verbosity.
- Slash commands provided today: `/mcstatus`, `/mccommand`, `/mcreloadbridge`.

## Deployment order

1. Build the plugin with `cmake --preset linux-clang-local-libcxx` and `cmake --build --preset build-local`.
2. Copy `build-local/endstone_bedrock_discord_bridge.so` into your Bedrock server `plugins/` directory.
3. Copy [config/config.json.example](config/config.json.example) to the plugin data folder as `config.json` and fill in the webhook plus bot bridge settings.
4. Start the server once so the plugin creates its data directories and avatar cache path.
5. Set up the companion bot from [plugins/endcord/bot/config.json](plugins/endcord/bot/config.json), making sure `plugin_bridge.shared_secret` matches `bot_bridge.shared_secret`.
6. Run the bot on the same host while `bot_bridge.allow_local_requests_only` is enabled.

## Implemented scope

- Implemented: Minecraft-to-Discord relay via webhook with per-message username and avatar URL support
- Implemented: Configurable relay templates and event toggles for chat, join, quit, and death messages
- Implemented: Bedrock skin head rendering, caching, and built-in avatar HTTP serving
- Implemented: Discord-to-Minecraft relay through the companion bot and secure plugin API
- Implemented: Customizable companion-bot presence and relay/logging controls
- Implemented: Slash-command driven status, reload, and remote command execution
- Implemented: Linux `.so` build path for Endstone and JSON-driven runtime configuration

## Publishing notes

- License: [LICENSE](LICENSE)
- Changelog: [CHANGELOG.md](CHANGELOG.md)
- Third-party dependency summary: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
- Contribution guide: [CONTRIBUTING.md](CONTRIBUTING.md)
- Security guidance: [SECURITY.md](SECURITY.md)

## Release checklist

1. Confirm the plugin still builds with `cmake --build --preset build-local`.
2. Confirm the bot still installs and its sources compile with `python3 -m compileall bot/src`.
3. Review `config/config.json.example`, `config/config.schema.json`, `plugins/endcord/bot/config.json`, and `bot/config.schema.json` for any new settings.
4. Update [CHANGELOG.md](CHANGELOG.md) with release notes.
5. Tag the release only after CI passes on `master`.
