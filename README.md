# Bedrock Discord Bridge

Linux-ready Endstone C++ plugin plus companion Discord bot for a two-way Minecraft Bedrock bridge with JSON configuration, per-player webhook identity, Bedrock-aware avatar providers, and a secure local bot API.

## What is included

- Endstone C++ plugin build pinned to `v0.11.2`
- Linux build flow that works without sudo by bootstrapping `libc++` locally
- Async Discord webhook worker so chat events do not block on network I/O
- Rate-limit aware retry path for `429` and transient webhook failures
- Runtime webhook override API so the companion bot can provision the webhook for the plugin
- Bedrock-aware external avatar provider mode for easy installs without extra web hosting
- Secure local bot bridge API for Discord-to-Minecraft relay and remote command execution
- JSON config with nested sections for Discord, relay toggles, logging, queue, avatar options, and bot bridge settings
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
ctest --test-dir build-local --output-on-failure
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

- `discord.webhook_url` can be set manually, or the companion bot can inject a runtime webhook when `discord.allow_runtime_webhook_override` is `true`.
- `discord.username_template`, `discord.content_template`, `discord.system_username_template`, and the join/quit/death templates support `{player}`, `{message}`, `{event_message}`, `{event}`, `{skin_id}`, and `{server}`.
- `relay.minecraft_to_discord_enabled` is the master outbound switch, and `relay.chat_enabled`, `relay.join_enabled`, `relay.quit_enabled`, and `relay.death_enabled` let you tune which Minecraft events reach Discord.
- `discord.use_player_avatar_for_system_messages` controls whether join/quit/death webhook posts reuse the player head avatar.
- `avatar.provider` supports `tabavatars`, `mcheads`, and `custom`.
- `avatar.provider_url_template` lets server owners bring their own provider with placeholders like `{player}`, `{xuid}`, `{uuid}`, `{uuid_nodashes}`, `{skin_id}`, `{size}`, and `{render_type}`.
- `avatar.provider_prefer_xuid` makes Bedrock avatar provider lookups use `Player::getXuid()` when possible, which is more stable than gamertags.
- `queue.max_size`, `queue.max_attempts`, and timeout values control webhook behavior under load.
- `logging.log_filtered_events`, `logging.log_webhook_successes`, `logging.log_http_requests`, and the bot logging flags let you dial the plugin between quiet production mode and detailed troubleshooting.
- `bot_bridge.enabled` turns on the local HTTP API used by the companion Discord bot.
- `bot_bridge.shared_secret` must match the bot config exactly.
- `bot_bridge.api_route_prefix` must line up with the bot's `plugin_bridge.base_url`.
- `bot_bridge.allow_local_requests_only` should usually stay `true`, and `bot_bridge.allowed_remote_addresses` is only for advanced cases where the bot cannot stay on the same local runtime path.
- `bot_bridge.outbound_system_messages_enabled` moves join/quit/death delivery to the companion bot so those lifecycle posts come from the bot identity instead of the webhook identity.
- `bot_bridge.outbound_system_message_max_batch` controls how many queued lifecycle events the bot can drain per poll.
- `bot_bridge.outbound_system_message_queue_max_size` bounds that queue so a restart loop cannot grow memory indefinitely.
- `bot_bridge.inbound_chat_template` supports `{author}`, `{content}`, `{channel}`, `{guild}`, `{message_url}`, and `{server}`.
- `bot_bridge.inbound_chat_max_length` clamps how much Discord content the plugin will project into Minecraft chat.

## Avatar providers

The plugin now uses provider-based avatars only. That keeps installs simple and avoids shipping any self-hosted avatar rendering, cache serving, tunnel, or proxy logic.

Use one of:

- `tabavatars`
- `mcheads`
- `custom` through `avatar.provider_url_template`

## Recommended Discord architecture

Use a hybrid model:

- Bot token connection for Discord-to-Minecraft chat, slash commands, admin workflows, and channel event handling
- Discord webhook for Minecraft-to-Discord chat so each message can set a player-specific `username` and `avatar_url`

Practical avatar path:

1. Read Bedrock player identity data such as gamertag, XUID, UUID, and skin id.
2. Build a provider URL for the configured avatar service.
3. Reuse that provider URL in the webhook `avatar_url` field for Minecraft-originated messages.

Rate-limit strategy:

- Queue outbound webhook posts instead of sending inline on the chat event thread
- Respect Discord `429` responses and `Retry-After`
- Group local throttling by `X-RateLimit-Bucket`
- Cache provider resolution decisions in-process where useful, but keep the runtime focused on fast webhook delivery
- Retry transient webhook failures with bounded backoff instead of blocking the server thread

## Companion bot

The repo now includes a companion bot under [bot/README.md](bot/README.md).

Quick start:

```bash
cd bot
./scripts/bootstrap-local-runtime.sh
./scripts/run-local-runtime.sh
```

Bot-only test command:

```bash
python3 -m unittest discover -s bot/tests
```

Bot config highlights:

- `discord.relay_channel_ids` limits which Discord channels relay into Minecraft.
- `discord.guild_id` can stay `0` if you want the bot to derive the guild from the first configured channel.
- `discord.auto_create_webhook` and `plugin_bridge.configure_webhook_on_startup` are the easiest-distribution path: the bot creates or reuses the outbound webhook and injects it into the plugin automatically. Keep the configured webhook name free of the word `Discord`, because Discord rejects it in webhook names.
- `discord.command_role_ids` and `discord.status_role_ids` control who can use slash commands.
- `plugin_bridge.base_url` should normally stay on `http://127.0.0.1:<port><api_route_prefix>` because the intended runtime model is to keep the bot in the same local runtime environment as Endstone.
- `plugin_bridge.request_max_retries`, `plugin_bridge.request_retry_base_seconds`, and `plugin_bridge.request_retry_max_seconds` control retry/backoff for safe bridge operations.
- `relay.message_template`, `relay.attachment_template`, `relay.jump_url_template`, and `relay.join_separator` control how Discord messages are projected into Minecraft before the plugin wraps them with `bot_bridge.inbound_chat_template`.
- `relay.ignore_bot_messages` and `relay.ignore_webhook_messages` let you decide whether other automation can talk back into Minecraft.
- `presence.activity_text` supports `{server_name}`, `{minecraft_version}`, `{online_players}`, `{webhook_queue_depth}`, and `{guild_name}`.
- `logging.level`, `logging.log_ignored_messages`, `logging.log_relay_successes`, and `logging.log_presence_updates` control bot verbosity.
- `system_messages.enabled` turns on bot-owned join/quit/death delivery, `system_messages.channel_id` selects the target channel, and `system_messages.message_template` supports `{content}`, `{event}`, and `{player_name}`.
- Slash commands provided today: `/mcstatus`, `/mccommand`, `/mcreloadbridge`.
- Colocated-runtime launcher scripts live under [bot/scripts/bootstrap-local-runtime.sh](bot/scripts/bootstrap-local-runtime.sh) and [bot/scripts/run-local-runtime.sh](bot/scripts/run-local-runtime.sh).
- A sample service file for non-Docker deployments is included at [bedrock-discord-bridge-bot.service.example](bot/systemd/bedrock-discord-bridge-bot.service.example).

## Deployment order

1. Build the plugin with `cmake --preset linux-clang-local-libcxx` and `cmake --build --preset build-local`.
2. Copy `build-local/endstone_bedrock_discord_bridge.so` into your Bedrock server `plugins/` directory.
3. Copy [config/config.json.example](config/config.json.example) to the plugin data folder as `config.json` and fill in the bot bridge settings. A manual webhook URL is optional if the bot is allowed to auto-create one.
4. Start the server once so the plugin creates its data directory and bridge runtime files.
5. Set up the companion bot from [plugins/endcord/bot/config.json](plugins/endcord/bot/config.json), making sure `plugin_bridge.shared_secret` matches `bot_bridge.shared_secret`.
6. Fill in `discord.token` and at least one channel id. The default bot config can auto-derive the guild and auto-create the webhook.
7. Run the bot in the same local runtime environment as Endstone while `bot_bridge.allow_local_requests_only` is enabled, ideally through [bot/scripts/run-local-runtime.sh](bot/scripts/run-local-runtime.sh) or a system service based on [bedrock-discord-bridge-bot.service.example](bot/systemd/bedrock-discord-bridge-bot.service.example).

## Implemented scope

- Implemented: Minecraft-to-Discord relay via webhook with per-message username and avatar URL support
- Implemented: Configurable relay templates and event toggles for chat, join, quit, and death messages
- Implemented: Provider-based Bedrock avatar URLs for webhook identity rendering
- Implemented: Discord-to-Minecraft relay through the companion bot and secure plugin API
- Implemented: Optional bot-owned delivery path for lifecycle/system messages while player chat stays on the webhook
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
2. Confirm the plugin support tests pass with `ctest --test-dir build-local --output-on-failure`.
3. Confirm the bot still installs and its sources compile with `python3 -m compileall bot/src`.
4. Confirm the bot config tests pass with `python3 -m unittest discover -s bot/tests`.
5. Review `config/config.json.example`, `config/config.schema.json`, `plugins/endcord/bot/config.json`, and `bot/config.schema.json` for any new settings.
6. Update [CHANGELOG.md](CHANGELOG.md) with release notes.
7. Tag the release only after CI passes on `master`.
