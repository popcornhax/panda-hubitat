#!/usr/bin/env bash
set -euo pipefail
umask 077

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
esphome_bin="${ESPHOME_BIN:-$repo_root/.venv/bin/esphome}"
secrets_file="$repo_root/firmware/secrets.yaml"
config="firmware/panda-touch.yaml"

if [[ "${1:-}" == "--test-config" ]]; then
  config="firmware/display-touch-test.yaml"
  shift
fi
if [[ $# -gt 0 ]]; then
  echo "Usage: $0 [--test-config]" >&2
  exit 2
fi
if [[ ! -x "$esphome_bin" ]]; then
  echo "ESPHome not found at $esphome_bin; run ./scripts/bootstrap.sh" >&2
  exit 1
fi
if [[ ! -f "$secrets_file" ]]; then
  echo "Secrets file not found: $secrets_file (copy secrets.example.yaml there)" >&2
  exit 1
fi

if [[ "$config" == "firmware/panda-touch.yaml" ]]; then
  "$repo_root/.venv/bin/python" "$repo_root/scripts/generate_dashboard_config.py"
fi

exec "$esphome_bin" compile "$config"
