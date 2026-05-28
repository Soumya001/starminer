#!/bin/bash
# Puzzle #135 Pool — Universal Installer
#
# Interactive (Linux/macOS):
#   curl -fsSL https://starnetlive.space/install-135.sh | bash
#
# Vast.ai / headless (set WORKER_NAME, no prompts):
#   WORKER_NAME=bc1qYOURBTCADDRESS bash -c \
#     "curl -fsSL https://starnetlive.space/install-135.sh | bash"
#
# Vast.ai on-start field (paste as-is, replace BTC address):
#   WORKER_NAME=bc1qYOURBTCADDRESS bash -c "curl -fsSL https://starnetlive.space/install-135.sh | bash"

set -e

C_RED='\033[0;31m'; C_GREEN='\033[0;32m'
C_YELLOW='\033[1;33m'; C_BLUE='\033[0;34m'
C_CYAN='\033[0;36m'; C_NC='\033[0m'

_ok()   { echo -e "${C_GREEN}[+]${C_NC} $*"; }
_info() { echo -e "${C_BLUE}[*]${C_NC} $*"; }
_warn() { echo -e "${C_YELLOW}[!]${C_NC} $*"; }
_err()  { echo -e "${C_RED}[x]${C_NC} $*"; }

# ── Find Python 3 ────────────────────────────────────────────────────
if   command -v python3 &>/dev/null; then PYTHON=python3
elif command -v python  &>/dev/null; then PYTHON=python
else
    _info "Python not found — installing..."
    if command -v apt-get &>/dev/null; then
        apt-get update -qq && apt-get install -y -qq python3
        PYTHON=python3
    elif command -v yum &>/dev/null; then
        yum install -y -q python3
        PYTHON=python3
    else
        _err "Cannot install Python automatically. Please install python3 and re-run."
        exit 1
    fi
fi
_ok "Python: $($PYTHON --version 2>&1)"

# ── Download and run install.py ───────────────────────────────────────
URL="https://raw.githubusercontent.com/Soumya001/starminer/main/pool/installer/install.py"
TMPFILE=$(mktemp /tmp/starminer_install.XXXXXX.py)

_info "Downloading installer..."
if   command -v curl &>/dev/null;  then curl -fsSL  "$URL" -o "$TMPFILE"
elif command -v wget &>/dev/null;  then wget -q "$URL" -O "$TMPFILE"
else _err "curl or wget required."; exit 1; fi
_ok "Ready."
echo ""

# WORKER_NAME is passed through to install.py via environment
exec $PYTHON "$TMPFILE"
