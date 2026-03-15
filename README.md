# Bedrock Discord Bridge

Linux-ready Endstone C++ plugin skeleton for Minecraft Bedrock chat forwarding to Discord.

## What is included

- Endstone C++ plugin scaffold using `FetchContent` and `endstone_add_plugin(...)`
- Verified `PlayerChatEvent` listener stub
- Verified `Player::getSkin()` and `Skin::getImage()` usage for future avatar/head rendering
- Simple `discord.env` config format to avoid adding an unverified parser dependency
- Placeholders for webhook delivery, skin head extraction, and avatar caching

## Build on Linux

The current CMake project is pinned to Endstone `v0.11.2`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

If you want to force Clang explicitly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j
```

Expected Linux artifact name:

```text
build/endstone_bedrock_discord_bridge.so
```

## Config

Copy the example file into the plugin data folder created by Endstone at runtime:

```bash
cp config/discord.env.example path/to/bedrock_server/plugins/bedrock_discord_bridge/discord.env
```

Then set:

```text
DISCORD_WEBHOOK_URL=https://discord.com/api/webhooks/...
```

## Recommended Discord architecture

Use a hybrid model:

- Bot token connection for Discord-to-Minecraft chat, slash commands, admin workflows, and channel event handling
- Discord webhook for Minecraft-to-Discord chat so each message can set a player-specific `username` and `avatar_url`

Practical avatar path:

1. Read Bedrock skin RGBA data from `player.getSkin().getImage()`.
2. Extract the face region and hat/overlay region into a square head render.
3. Upload the rendered PNG to a stable public URL.
4. Cache the URL by `skin.getId()`.
5. Reuse the cached `avatar_url` for subsequent webhook messages until the skin id changes.

Rate-limit strategy:

- Queue outbound webhook posts instead of sending inline on the chat event thread
- Respect Discord `429` responses and `Retry-After`
- Group local throttling by `X-RateLimit-Bucket`
- Cache avatar URLs aggressively so skin uploads are much rarer than chat sends

## Next implementation steps

- Add an async HTTP client for webhook POSTs
- Add PNG encoding for extracted head icons
- Add a storage target for avatar images
- Add a Discord bot component for inbound relay and admin commands
