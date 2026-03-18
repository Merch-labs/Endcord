#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BOT_DIR=$(cd "${SCRIPT_DIR}/.." && pwd)
CONFIG_PATH="${1:-${BOT_DIR}/config.json}"
CONFIG_DIR=$(dirname "${CONFIG_PATH}")
VENV_DIR="${BOT_VENV_DIR:-${CONFIG_DIR}/.venv}"

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required to bootstrap the companion bot." >&2
    exit 1
fi

python3 -m venv "${VENV_DIR}"
"${VENV_DIR}/bin/python" -m pip install --upgrade pip
"${VENV_DIR}/bin/python" -m pip install "${BOT_DIR}"

if [[ ! -f "${CONFIG_PATH}" ]]; then
    mkdir -p "$(dirname "${CONFIG_PATH}")"
    cat > "${CONFIG_PATH}" <<'EOF'
{
  "discord": {
    "token": "replace-me",
    "guild_id": 0,
    "relay_channel_ids": [0],
    "outbound_channel_id": 0,
    "command_role_ids": [],
    "status_role_ids": [],
    "relay_to_game_enabled": true,
    "sync_commands_globally": false,
    "auto_create_webhook": true,
    "webhook_name": "Endcord"
  },
  "plugin_bridge": {
    "base_url": "http://127.0.0.1:8089/endcord/api",
    "request_timeout_seconds": 10,
    "configure_webhook_on_startup": true,
    "request_max_retries": 3,
    "request_retry_base_seconds": 1.5,
    "request_retry_max_seconds": 15.0
  },
  "relay": {
    "include_attachment_urls": true,
    "include_jump_url": false,
    "ignore_bot_messages": true,
    "ignore_webhook_messages": true,
    "message_template": "{content}{attachments}{jump_url}",
    "attachment_template": "\n[attachment] {url}",
    "jump_url_template": "\n[jump] {url}",
    "join_separator": "\n",
    "max_message_length": 1800
  },
  "slash_commands": {
    "enabled": true,
    "ephemeral_responses": true,
    "status": {
      "enabled": true,
      "admin_only": false,
      "role_ids": []
    },
    "command": {
      "enabled": true,
      "admin_only": true,
      "role_ids": []
    },
    "players": {
      "enabled": true,
      "admin_only": false,
      "role_ids": []
    },
    "ping": {
      "enabled": true,
      "admin_only": false,
      "role_ids": []
    },
    "configreload": {
      "enabled": true,
      "admin_only": true,
      "role_ids": []
    }
  },
  "presence": {
    "enabled": true,
    "status": "online",
    "activity_type": "watching",
    "activity_text": "{server_name} | {online_players} online",
    "fallback_text": "Endcord online",
    "streaming_url": "",
    "update_interval_seconds": 120
  },
  "logging": {
    "level": "INFO",
    "log_ignored_messages": false,
    "log_relay_successes": false,
    "log_presence_updates": false
  },
  "system_messages": {
    "enabled": false,
    "channel_id": 0,
    "poll_interval_seconds": 2,
    "failure_backoff_seconds": 5,
    "max_backoff_seconds": 60,
    "message_template": "{content}",
    "max_messages_per_poll": 20
  }
}
EOF
    echo "Wrote default config to ${CONFIG_PATH}"
fi

echo "Companion bot installed in ${VENV_DIR}"
echo "Run it with: ${SCRIPT_DIR}/run-local-runtime.sh ${CONFIG_PATH}"
