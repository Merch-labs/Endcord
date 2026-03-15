# Security Policy

## Supported scope

Security issues in the Endstone plugin, the companion bot, and the local bridge API are in scope.

## Reporting

If you discover a security issue, report it privately to the maintainer before opening a public issue.

## Deployment guidance

- Keep `bot_bridge.allow_local_requests_only` enabled unless you fully control a trusted reverse proxy in front of the plugin API.
- Use a strong `bot_bridge.shared_secret` and rotate it if you suspect exposure.
- Do not publish the bridge API directly to the public internet.
- Treat `/command` access as privileged because it can execute server commands.
- Prefer running the bot on the same host as the Bedrock server.
