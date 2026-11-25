#!/usr/bin/env bash
set -euo pipefail
umask 077

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

if ! command -v uv >/dev/null 2>&1; then
  echo "uv is required. Install it from https://docs.astral.sh/uv/" >&2
  exit 1
fi

uv venv --python '>=3.12,<3.16' .venv
uv pip install --python .venv/bin/python -r requirements.txt
echo "Environment ready. Activate with: source .venv/bin/activate"
