#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BOT_DIR=$(cd "${SCRIPT_DIR}/.." && pwd)
VENV_DIR="${BOT_VENV_DIR:-${BOT_DIR}/.venv}"

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required to bootstrap the companion bot." >&2
    exit 1
fi

python3 -m venv "${VENV_DIR}"
"${VENV_DIR}/bin/python" -m pip install --upgrade pip
"${VENV_DIR}/bin/python" -m pip install -e "${BOT_DIR}"

if [[ ! -f "${BOT_DIR}/config.json" && -f "${BOT_DIR}/config.json.example" ]]; then
    cp "${BOT_DIR}/config.json.example" "${BOT_DIR}/config.json"
    echo "Wrote default config to ${BOT_DIR}/config.json"
fi

echo "Companion bot installed in ${VENV_DIR}"
echo "Run it with: ${SCRIPT_DIR}/run-local-runtime.sh ${BOT_DIR}/config.json"
