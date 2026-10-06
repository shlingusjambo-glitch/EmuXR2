#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
PYTHON=${EMUXR_PYTHON:-python3}
if ! "$PYTHON" -c 'import av, numpy' >/dev/null 2>&1; then
    BUNDLED="$HOME/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3"
    if [ -x "$BUNDLED" ] && "$BUNDLED" -c 'import av, numpy' >/dev/null 2>&1; then PYTHON=$BUNDLED
    else echo 'av numpy is required: python3 -m pip install av numpy' >&2; exit 1; fi
fi
exec "$PYTHON" "$HERE/server.py" "$@"
