#!/usr/bin/env bash
# Installer for GKrellM Dock plugins + gb10-blue theme on DGX Spark (GB10).
# Builds the six GKrellM plugins, installs the theme, and writes the managed
# ~/.gkrellm2 config. Does NOT start gkrellm and does NOT manage autostart —
# launch and autostart of gkrellm are handled by the host environment.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SKIP_DEPS=0

usage() {
  cat <<'EOF'
Usage: ./scripts/install.sh [options]

  --skip-deps   Do not apt-install missing packages
  -h, --help    Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-deps) SKIP_DEPS=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage; exit 1 ;;
  esac
  shift
done

need_cmd() {
  command -v "$1" >/dev/null 2>&1
}

echo "== GKrellM Dock installer (DGX Spark GB10) =="
echo "source: $ROOT"

ARCH="$(uname -m)"
if [[ "$ARCH" != "aarch64" ]]; then
  echo "WARNING: expected aarch64 (GB10); found $ARCH — continuing anyway." >&2
fi

PKGS=(gkrellm libgtk2.0-dev libcurl4-openssl-dev pkg-config build-essential)
MISSING=()
for p in "${PKGS[@]}"; do
  if ! dpkg -s "$p" >/dev/null 2>&1; then
    MISSING+=("$p")
  fi
done

# Deduplicate MISSING
if ((${#MISSING[@]})); then
  mapfile -t MISSING < <(printf '%s\n' "${MISSING[@]}" | sort -u)
fi

if ((${#MISSING[@]})) && [[ "$SKIP_DEPS" -eq 0 ]]; then
  echo "== installing packages: ${MISSING[*]}"
  if need_cmd sudo; then
    sudo apt-get update -qq
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y "${MISSING[@]}"
  else
    echo "Missing packages and sudo unavailable: ${MISSING[*]}" >&2
    exit 1
  fi
elif ((${#MISSING[@]})); then
  echo "Missing packages (--skip-deps): ${MISSING[*]}" >&2
  exit 1
fi

if ! need_cmd gkrellm; then
  echo "gkrellm binary not found after package install" >&2
  exit 1
fi

echo "== build + install plugins / theme / config"
make -C "$ROOT" clean >/dev/null || true
make -C "$ROOT" install

cat <<'EOF'

Install complete.

  Plugins:  ~/.gkrellm2/plugins/  (nvidia, llm_nim, cpu_clusters, net_clusters, board_acpi; uma_dram.so installed but not enabled)
  Theme:    ~/.gkrellm2/themes/gb10-blue/
  Config:   ~/.gkrellm2/user-config  (managed by scripts/install_config.sh)

Start the dock:  gkrellm -t ~/.gkrellm2/themes/gb10-blue
Docs: README.md and docs/
EOF
