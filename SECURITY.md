# Security Policy

## Supported scope

Security issues in the Endcord plugin and its integrated Discord runtime are in scope.

## Reporting

Report security issues privately to the maintainer before opening a public issue.

## Deployment guidance

- Treat `/command` access as privileged because it can execute server commands.
- Keep `bot.discord.token` private.
- Use Discord role restrictions for sensitive slash commands.
- Do not share full `config.json` files publicly.
