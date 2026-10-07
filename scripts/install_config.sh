#!/usr/bin/env bash
# Write GKrellM user-config for GB10 dock (plugins + layout prefs).
set -euo pipefail

CFG="$HOME/.gkrellm2/user-config"
PE="$HOME/.gkrellm2/plugin_enable"
NETDATA="$HOME/.gkrellm2/data/net"
mkdir -p "$HOME/.gkrellm2/plugins" "$HOME/.gkrellm2/themes" "$NETDATA"

# Plugin load order: nvidia (GB10 header+GPU, incl. UMA %), LLM NIM, CPU, Net, Board
# (uma_dram.so is built/installed but not enabled: its chart + readout duplicated the
#  UMA % already shown under the GPU charts and cost ~120 px of dock height)
cat >"$PE" <<'EOF'
nvidia.so
llm_nim.so
cpu_clusters.so
net_clusters.so
board_acpi.so
EOF

POS=""
if [[ -f "$HOME/.gkrellm2/data/startup_position" ]]; then
  POS="$(cat "$HOME/.gkrellm2/data/startup_position")"
fi

# Defaults for LLM NIM — preserve GUI/user choices across restart re-apply
LLM_URL="http://127.0.0.1:8000"
LLM_FEATURES="1908735"
LLM_TPS="50"
LLM_PREFILL="1000"
LLM_TIMEOUT="500"
LLM_DISPLAY_NAME=""
LLM_DOCS_RELEASE="2.0.10"
LLM_AIRGAP="0"
LLM_DOCK_SLOTS="1"
LLM_SLOT1_URL=""
LLM_SLOT1_NAME=""
LLM_SLOT2_URL=""
LLM_SLOT2_NAME=""
LLM_SLOT3_URL=""
LLM_SLOT3_NAME=""

# Net Clusters: interfaces folded into one chart (container veth by default)
NETC_PATTERN="^veth"
NETC_LABEL="Docker"

if [[ -f "$CFG" ]]; then
  v="$(sed -n 's/^net_clusters pattern //p' "$CFG" | head -n1 || true)"
  [[ -n "${v:-}" ]] && NETC_PATTERN="$v"
  v="$(sed -n 's/^net_clusters label //p' "$CFG" | head -n1 || true)"
  [[ -n "${v:-}" ]] && NETC_LABEL="$v"
  v="$(awk '/^llm_nim url / { print $3; exit }' "$CFG" || true)"
  [[ -n "${v:-}" ]] && LLM_URL="$v"
  v="$(awk '/^llm_nim features / { print $3; exit }' "$CFG" || true)"
  [[ -n "${v:-}" ]] && LLM_FEATURES="$v"
  v="$(awk '/^llm_nim chart_max_tps / { print $3; exit }' "$CFG" || true)"
  [[ -n "${v:-}" ]] && LLM_TPS="$v"
  v="$(awk '/^llm_nim chart_max_prefill / { print $3; exit }' "$CFG" || true)"
  [[ -n "${v:-}" ]] && LLM_PREFILL="$v"
  v="$(awk '/^llm_nim timeout_ms / { print $3; exit }' "$CFG" || true)"
  [[ -n "${v:-}" ]] && LLM_TIMEOUT="$v"
  v="$(sed -n 's/^llm_nim display_name //p' "$CFG" | head -n1 || true)"
  [[ -n "${v:-}" ]] && LLM_DISPLAY_NAME="$v"
  v="$(awk '/^llm_nim docs_release / { print $3; exit }' "$CFG" || true)"
  [[ -n "${v:-}" ]] && LLM_DOCS_RELEASE="$v"
  v="$(awk '/^llm_nim airgap / { print $3; exit }' "$CFG" || true)"
  [[ -n "${v:-}" ]] && LLM_AIRGAP="$v"
  v="$(awk '/^llm_nim dock_slots / { print $3; exit }' "$CFG" || true)"
  [[ -n "${v:-}" ]] && LLM_DOCK_SLOTS="$v"
  v="$(awk '/^llm_nim slot1_url / { print $3; exit }' "$CFG" || true)"
  [[ -n "${v:-}" ]] && LLM_SLOT1_URL="$v"
  v="$(sed -n 's/^llm_nim slot1_name //p' "$CFG" | head -n1 || true)"
  [[ -n "${v:-}" ]] && LLM_SLOT1_NAME="$v"
  v="$(awk '/^llm_nim slot2_url / { print $3; exit }' "$CFG" || true)"
  [[ -n "${v:-}" ]] && LLM_SLOT2_URL="$v"
  v="$(sed -n 's/^llm_nim slot2_name //p' "$CFG" | head -n1 || true)"
  [[ -n "${v:-}" ]] && LLM_SLOT2_NAME="$v"
  v="$(awk '/^llm_nim slot3_url / { print $3; exit }' "$CFG" || true)"
  [[ -n "${v:-}" ]] && LLM_SLOT3_URL="$v"
  v="$(sed -n 's/^llm_nim slot3_name //p' "$CFG" | head -n1 || true)"
  [[ -n "${v:-}" ]] && LLM_SLOT3_NAME="$v"
fi

cat >"$CFG" <<EOF
### GKrellM user config.  Managed by gkrellm-dock / gkrellm-dock project ###
### Version 2.3.11 ###
enable_hostname 1
hostname_short 0
enable_sysname 0
mbmon_port 0
sticky_state 1
dock_type 0
decorated 0
skip_taskbar 1
skip_pager 1
above 1
below 0
track_gtk_theme_name 0
default_track_theme "gb10-blue"
save_position 1
chart_width 120
update_HZ 10
allow_multiple_instances 0
float_factor 1000
# Stock composite CPU off — plugins/cpu_clusters.so provides X925/A725
cpu enabled cpu 0
cpu enabled cpu0 0
cpu enabled cpu1 0
cpu enabled cpu2 0
cpu enabled cpu3 0
cpu enabled cpu4 0
cpu enabled cpu5 0
cpu enabled cpu6 0
cpu enabled cpu7 0
cpu enabled cpu8 0
cpu enabled cpu9 0
cpu enabled cpu10 0
cpu enabled cpu11 0
cpu enabled cpu12 0
cpu enabled cpu13 0
cpu enabled cpu14 0
cpu enabled cpu15 0
cpu enabled cpu16 0
cpu enabled cpu17 0
cpu enabled cpu18 0
cpu enabled cpu19 0
cpu show_panel_labels 1
cpu omit_nice_mode 0
cpu config_tracking 1
# Memory: UMA % is shown in the GPU block (nvidia.so); the optional uma_dram.so
# readout is not enabled by default. Stock Mem meter stays off — gb10-blue krells
# still crush its label.
# Swap: stock meter ON with name label (label_is_data=0).
meminfo mem_meter 0 0 0
meminfo swap_meter 1 0
meminfo swap_chart 0 0
uptime enable 1
# Hide unused mail to free vertical space
mail enable 0 0 0 0
# Net: stock monitor only for physical NICs + docker0. Container veth* are
# folded into ONE chart by plugins/net_clusters.so (no per-container charts,
# no restart needed when containers are recreated). PPP/bridges/tunnels ignored.
net timer_enabled 0
net timer_iface none
net net_enabled_as_default 0
net ignore_patterns ^ppp[0-9]+\$|^br-|^virbr|^tun|^tap|^veth
net enables docker0 1 1 0
net enables wlP9s9 1 1 0
net enables enP7s7 1 1 0
net_clusters pattern $NETC_PATTERN
net_clusters label $NETC_LABEL
EOF

cat >>"$CFG" <<EOF
# NVML: Temp + UMA% (mask 272); path for GB10
nvidia NVML 272 abcdefghijkl /usr/lib/aarch64-linux-gnu/libnvidia-ml.so.1
# LLM NIM/vLLM (nemotron-nim :8000) — prefs preserved if already in user-config
# features default: bits 0–3 + TTFT/SPEC/PREFIX/ITL/E2E/TPOT/PREEMPT/REQ/RSS/PCACHE/CPU/QREASON/ENGINE
llm_nim url $LLM_URL
llm_nim display_name $LLM_DISPLAY_NAME
llm_nim dock_slots $LLM_DOCK_SLOTS
llm_nim slot1_url $LLM_SLOT1_URL
llm_nim slot1_name $LLM_SLOT1_NAME
llm_nim slot2_url $LLM_SLOT2_URL
llm_nim slot2_name $LLM_SLOT2_NAME
llm_nim slot3_url $LLM_SLOT3_URL
llm_nim slot3_name $LLM_SLOT3_NAME
llm_nim docs_release $LLM_DOCS_RELEASE
llm_nim airgap $LLM_AIRGAP
llm_nim features $LLM_FEATURES
llm_nim chart_max_tps $LLM_TPS
llm_nim chart_max_prefill $LLM_PREFILL
llm_nim timeout_ms $LLM_TIMEOUT
# Disk: composite only, fixed 5×40 MB/s = 200 MB/s full scale
disk assign_method 2
disk device Disk 0 0 0 1 1 0 0 Disk
disk chart_config Disk 40 40000000 5 0 0 1 : 0 0 0 0 500 : 0 0 0 0 500
disk device nvme0n1 0 0 28 0 1 -1 0 nvme0n1
disk device sda 0 0 45 0 1 -1 0 sda
EOF

# Drop stale PPP cookies and every veth cookie (veth is handled by net_clusters)
rm -f "$NETDATA/ppp0" "$NETDATA/ppp0_disabled"
rm -f "$NETDATA"/veth*

echo "wrote $CFG and $PE"
echo "llm_nim features $LLM_FEATURES (url $LLM_URL)"
echo "net_clusters: pattern '$NETC_PATTERN' label '$NETC_LABEL' (replaces per-veth charts)"
[[ -n "$POS" ]] && echo "startup_position preserved: $POS"
