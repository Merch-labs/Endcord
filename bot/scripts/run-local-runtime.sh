#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BOT_DIR=$(cd "${SCRIPT_DIR}/.." && pwd)
VENV_DIR="${BOT_VENV_DIR:-${BOT_DIR}/.venv}"
CONFIG_PATH="${1:-${BOT_DIR}/config.json}"

if [[ ! -x "${VENV_DIR}/bin/endcord-bot" ]]; then
    echo "Companion bot is not installed in ${VENV_DIR}." >&2
    echo "Run ${SCRIPT_DIR}/bootstrap-local-runtime.sh first." >&2
    exit 1
fi

if [[ ! -f "${CONFIG_PATH}" ]]; then
    echo "Config file not found: ${CONFIG_PATH}" >&2
    exit 1
fi

exec "${VENV_DIR}/bin/endcord-bot" "${CONFIG_PATH}"
