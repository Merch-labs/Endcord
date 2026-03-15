# Bedrock Discord Bridge

Linux-ready Endstone C++ Endstone plugin for Minecraft Bedrock chat forwarding to Discord with JSON config, per-player webhook identity, rendered skin-head avatars, and built-in avatar hosting.

## What is included

- Endstone C++ plugin build pinned to `v0.11.2`
- Linux build flow that works without sudo by bootstrapping `libc++` locally
- Async Discord webhook worker so chat events do not block on network I/O
- Rate-limit aware retry path for `429` and transient webhook failures
- Bedrock skin face plus hat-overlay rendering to PNG
- Local avatar cache under the plugin data folder
- Built-in HTTP server for serving avatar PNGs
- JSON config with nested sections for Discord, queue, and avatar hosting
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

Admin commands:

- `/discordbridge status`
- `/discordbridge reload`

Config notes:

- `discord.webhook_url` is required for Minecraft-to-Discord relay.
- `discord.username_template` and `discord.content_template` support `{player}`, `{message}`, `{skin_id}`, and `{server}`.
- `avatar.enabled` controls skin-head rendering.
- `avatar.public_base_url` is a direct override if you already host the avatar cache elsewhere.
- `avatar.http_server` can serve the avatar cache directly from the plugin.
- If `avatar.http_server.public_base_url` is set, the plugin uses it to build Discord `avatar_url` values.
- If `avatar.public_base_url` and `avatar.http_server.public_base_url` are both empty, the plugin derives a local base URL only when `bind_host` is not a wildcard address.
- `queue.max_size`, `queue.max_attempts`, and timeout values control webhook behavior under load.

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

## Current scope

- Implemented: Minecraft-to-Discord relay via webhook
- Implemented: local avatar rendering/cache pipeline for Bedrock skins
- Implemented: built-in avatar HTTP server and JSON config
- Implemented: Linux `.so` build path for Endstone
- Not implemented by design: Discord bot/gateway side. Mature bridges typically keep that as a separate bot process or service from the game plugin.
