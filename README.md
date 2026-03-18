# Bedrock Discord Bridge

Two-way Minecraft Bedrock <-> Discord bridge for Endstone.

It ships as:

- a Linux Endstone C++ plugin (`.so`)
- a colocated companion Discord bot
- JSON config files for both sides

The default setup is meant to feel simple:

1. build or download the plugin
2. drop the `.so` into `plugins/`
3. start the server once
4. fill in the bot token, channel id, and shared secret
5. run the bot in the same local runtime as Endstone

No Docker-specific setup, tunnel, proxy, or self-hosted avatar service is required for the normal path.

## What it does

- Minecraft -> Discord relay via webhook with per-message `username` and `avatar_url`
- Discord -> Minecraft relay through a secure local bot API
- Provider-based Bedrock avatar support with `tabavatars`, `mcheads`, or a custom provider URL
- Join, quit, and death relays with configurable templates
- Slash commands:
  - `/mcstatus`
  - `/mccommand`
  - `/mcreloadbridge`
- Runtime webhook provisioning so the bot can create and inject the webhook automatically

## Quick Start

### 1. Build the plugin

For a normal local Linux build:

```bash
./scripts/setup-local-libcxx.sh
cmake --preset linux-clang-local-libcxx
cmake --build --preset build-local
```

Expected artifact:

```text
build-local/endstone_bedrock_discord_bridge.so
```

If you want a build that matches the official `endstone/endstone` Debian 12 container image:

```bash
./scripts/build-plugin-debian12.sh
```

Artifact:

```text
build-debian12/endstone_bedrock_discord_bridge.so
```

### 2. Install the plugin

Copy the plugin into your Bedrock server `plugins/` directory:

```bash
cp build-local/endstone_bedrock_discord_bridge.so path/to/bedrock_server/plugins/
```

Or use the Debian 12 build if that matches your server runtime:

```bash
cp build-debian12/endstone_bedrock_discord_bridge.so path/to/bedrock_server/plugins/
```

### 3. Start the server once

On first load, the plugin creates:

```text
path/to/bedrock_server/plugins/bedrock_discord_bridge/config.json
```

You can also pre-seed it from:

```bash
cp config/config.json.example path/to/bedrock_server/plugins/bedrock_discord_bridge/config.json
```

### 4. Set the plugin config

For the normal setup, the most important fields are:

- `bot_bridge.shared_secret`
- `bot_bridge.allow_local_requests_only`
- optional formatting changes such as `bot_bridge.inbound_chat_template`
- optional avatar provider changes under `avatar`

Plugin schema:
- [config/config.schema.json](config/config.schema.json)

### 5. Set up the bot

```bash
cd bot
./scripts/bootstrap-local-runtime.sh
cp config.json.example /path/to/bedrock_server/plugins/bedrock_discord_bridge/bot/config.json
```

Then fill in:

- `discord.token`
- `discord.relay_channel_ids`
- `plugin_bridge.shared_secret`

The default bot config can auto-derive the guild and auto-create the outbound webhook.

Bot schema:
- [bot/config.schema.json](bot/config.schema.json)

### 6. Run the bot

Run the bot in the same local runtime environment as Endstone so the bridge stays on `127.0.0.1`:

```bash
cd bot
./scripts/run-local-runtime.sh /path/to/bedrock_server/plugins/bedrock_discord_bridge/bot/config.json
```

For long-running installs, use:
- [bedrock-discord-bridge-bot.service.example](bot/systemd/bedrock-discord-bridge-bot.service.example)

## Normal Runtime Model

The intended runtime model is:

- Endstone loads the plugin inside the Bedrock server process
- the companion bot runs beside it in the same local runtime environment
- the bot talks to the plugin over `http://127.0.0.1:<port>/bedrock-discord-bridge/api`

That keeps the product model simple:

- one plugin
- one bot
- one shared secret
- one local bridge URL

## Config Basics

### Plugin config

Important plugin settings:

- `discord.webhook_url`
  - optional if the bot is allowed to provision the webhook at runtime
- `discord.allow_runtime_webhook_override`
  - lets the bot inject the webhook automatically
- `relay.minecraft_to_discord_enabled`
  - master outbound toggle
- `relay.chat_enabled`
- `relay.join_enabled`
- `relay.quit_enabled`
- `relay.death_enabled`
- `avatar.enabled`
- `avatar.provider`
  - `tabavatars`, `mcheads`, or `custom`
- `avatar.provider_url_template`
  - used only when `provider = "custom"`
- `bot_bridge.shared_secret`
- `bot_bridge.allow_local_requests_only`
- `bot_bridge.inbound_chat_template`

Admin commands:

- `/discordbridge status`
- `/discordbridge reload`

### Bot config

Important bot settings:

- `discord.token`
- `discord.relay_channel_ids`
- `discord.outbound_channel_id`
  - optional if the first relay channel should also be used for outbound messages
- `discord.auto_create_webhook`
- `plugin_bridge.base_url`
  - normally `http://127.0.0.1:8089/bedrock-discord-bridge/api`
- `plugin_bridge.shared_secret`
- `relay.message_template`
- `presence.activity_text`

## Avatar Providers

The project now uses provider-based avatars only.

Supported modes:

- `tabavatars`
- `mcheads`
- `custom`

Relevant settings:

- `avatar.provider`
- `avatar.provider_url_template`
- `avatar.provider_prefer_xuid`
- `avatar.provider_render_type`
- `avatar.provider_bedrock_username_prefix`
- `avatar.size`

Practical provider flow:

1. read Bedrock player identity such as gamertag, XUID, UUID, and skin id
2. build a provider URL from the configured avatar provider
3. send that URL as the Discord webhook `avatar_url`

## Common Customization

Useful formatting settings:

- `discord.username_template`
- `discord.content_template`
- `discord.system_username_template`
- `discord.join_content_template`
- `discord.quit_content_template`
- `discord.death_content_template`
- `bot_bridge.inbound_chat_template`
- `relay.message_template`
- `relay.attachment_template`
- `relay.jump_url_template`

Useful behavior toggles:

- `discord.allow_mentions`
- `discord.use_player_avatar_for_system_messages`
- `relay.ignore_bot_messages`
- `relay.ignore_webhook_messages`
- `system_messages.enabled`

Useful reliability settings:

- `queue.max_size`
- `queue.max_attempts`
- `queue.retry_delay_ms`
- `plugin_bridge.request_max_retries`
- `plugin_bridge.request_retry_base_seconds`
- `plugin_bridge.request_retry_max_seconds`

## Build Notes

Endstone requires Clang on Linux and forces `-stdlib=libc++`, so plain `g++` builds are expected to fail.

Current Endstone pin:

- `v0.11.2`

Local test/build commands:

```bash
cmake --preset linux-clang-local-libcxx
cmake --build --preset build-local
ctest --test-dir build-local --output-on-failure
python3 -m unittest discover -s bot/tests
python3 -m compileall bot/src
```

## Recommended Architecture

Use a hybrid Discord setup:

- webhook for Minecraft-originated outbound messages
- bot connection for Discord-originated messages, slash commands, and webhook provisioning

This keeps the best part of each path:

- webhook gives per-message avatar/name overrides
- bot gives Discord gateway features without bloating the plugin runtime

## Project Layout

- plugin build/config:
  - [CMakeLists.txt](CMakeLists.txt)
  - [config/config.json.example](config/config.json.example)
  - [config/config.schema.json](config/config.schema.json)
- bot:
  - [bot/README.md](bot/README.md)
  - [plugins/endcord/bot/config.json](plugins/endcord/bot/config.json)
  - [bot/config.schema.json](bot/config.schema.json)

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
