#!/usr/bin/env bash
# Explicit backlog links + task identities; Python standard library only.
set -euo pipefail
exec python3 "$(dirname "$0")/check_doc_refs.py" "$@"
