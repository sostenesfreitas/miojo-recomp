#!/bin/bash
# ngx-sdk.sh: NVIDIA's DLSS SDK files for a local Linux build with NVIDIA DLSS (HOOKSHOT_NGX_SDK): scripts/ngx_sdk.py, which
# says what it fetches and asks for NVIDIA's license (--accept-nvidia-license: accepted without the question).
#
# Usage: scripts/linux/ngx-sdk.sh [--accept-nvidia-license] [<sdk folder>] [<runtime folder>]
#   <sdk folder>: default build-linux/ngx-sdk; <runtime folder>: libnvidia-ngx-dlss.so and libnvidia-ngx-dlssg.so copied
#   there too (a game folder's ngx/, or $XDG_DATA_HOME/hookshot/ngx; RX_VK_DLSS_DIR names another)
# Then build with HOOKSHOT_NGX_SDK=<sdk folder>.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
accept=()
if [ "${1:-}" = --accept-nvidia-license ]; then accept=(--accept-nvidia-license); shift; fi
args=(--platform linux "${accept[@]}")
[ -n "${2:-}" ] && args+=(--runtime-dir "$2")
exec "${PYTHON:-python3}" -I "$ROOT/scripts/ngx_sdk.py" "${args[@]}" ${1:+"$1"}
