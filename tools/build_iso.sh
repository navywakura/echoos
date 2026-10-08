#!/usr/bin/env bash
# RXos — build the bootable ISO (thin wrapper so `tools/` is self-contained;
# the actual rules live in the Makefile).
set -euo pipefail
cd "$(dirname "$0")/.."
make iso
