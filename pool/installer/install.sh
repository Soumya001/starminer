#!/bin/bash
# Puzzle #135 Pool Universal Installer
# Linux / macOS — curl -fsSL https://starnetlive.space/install-135.sh | bash

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

banner() {
    echo ""
    echo -e "${CYAN}===============================================${NC}"
    echo -e "${CYAN}  Puzzle #135 Pool — Universal Installer${NC}"
    echo -e "${CYAN}  Pollard's Kangaroo  |  ~13.5 BTC Prize${NC}"
    echo -e "${CYAN}===============================================${NC}"
    echo ""
}

check_python() {
    if command -v python3 &> /dev/null; then
        PYTHON=python3
    elif command -v python &> /dev/null; then
        PYTHON=python
    else
        echo -e "${RED}[!] Python 3 is required but not found.${NC}"
        echo -e "${YELLOW}    Please install Python 3.8+ and re-run.${NC}"
        exit 1
    fi
    VER=$($PYTHON --version 2>&1 | awk '{print $2}')
    echo -e "${GREEN}[+] Python found: $VER${NC}"
}

download_installer() {
    local url="https://raw.githubusercontent.com/Soumya001/puzzle-135-pool/main/installer/install.py"
    local tmpfile=$(mktemp /tmp/pool135_installer.XXXXXX.py)
    echo -e "${BLUE}[*] Downloading installer...${NC}"
    if command -v curl &> /dev/null; then
        curl -fsSL "$url" -o "$tmpfile"
    elif command -v wget &> /dev/null; then
        wget -q "$url" -O "$tmpfile"
    else
        echo -e "${RED}[!] curl or wget is required.${NC}"
        exit 1
    fi
    echo -e "${GREEN}[+] Installer downloaded.${NC}"
    echo ""
    $PYTHON "$tmpfile"
    rm -f "$tmpfile"
}

banner
check_python
download_installer
