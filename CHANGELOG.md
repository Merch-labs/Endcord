# Changelog

All notable changes to this project will be documented in this file.

## 0.7.0 - 2026-03-26

- Integrated the Discord runtime directly into the Endstone plugin in C++.
- Removed the separate Python companion bot runtime from the default architecture.
- Moved Discord runtime settings into the main plugin config under `bot`.
- Removed the external bridge secret and local bot API requirements for the normal setup.
- Simplified installation to one plugin and one config file.

## 0.4.0 - 2026-03-15

- Added configurable Minecraft-to-Discord relay toggles for chat, join, quit, and death events.
- Added separate Discord webhook templates for chat posts and lifecycle/system posts, including optional player avatars on join/quit/death messages.
- Added an optional plugin-to-bot queue so join/quit/death messages can be posted by the bot identity instead of the webhook identity.
- Added plugin-side logging controls for filtered events, webhook successes, HTTP requests, avatar cache activity, inbound Discord chat, and remote commands.
- Added Discord-to-Minecraft formatting controls for attachment lines, jump-link lines, and final relay content templating.
- Added bot-side polling, routing, and templates for queued system messages.
- Added companion-bot presence customization with live server placeholders and periodic status refresh from the plugin API.
- Added companion-bot logging controls for ignored messages, successful relays, presence refreshes, and log level selection.

## 0.3.0 - 2026-03-15

- Added a Linux-ready Endstone C++ plugin build for `endstone_endcord.so`.
- Added async Discord webhook delivery with queueing, retry handling, and per-message identity support.
- Added Bedrock skin head rendering, avatar caching, and built-in HTTP avatar serving.
- Added a secure local plugin API for Discord-to-Minecraft relay and remote command execution.
- Added a companion Discord bot with slash commands for status, reload, and remote command dispatch.
- Added JSON example configs and JSON schema files for both the plugin and companion bot.
