#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "${ECHO_PYTHON:-python3}" "$root/tools/source_setup.py" "$@" --root "$root"
