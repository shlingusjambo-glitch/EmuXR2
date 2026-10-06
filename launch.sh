#!/bin/sh
# Launch native Horizon panels through VrShell; browser accepts an optional URL.
set -eu
ADB_BIN=${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}/platform-tools/adb
SERIAL=${ANDROID_SERIAL:-emulator-5554}
case "${1:-settings}" in
    settings) PANEL=systemux://settings ;;
    library) PANEL=systemux://library ;;
    installed) PANEL=systemux://library/installed ;;
    app)
        PACKAGE=${2:-}
        case "$PACKAGE" in ''|*[!a-zA-Z0-9_.]*) echo 'Usage: launch.sh app <installed.package.name>' >&2; exit 1;; esac
        COMPONENT=$("$ADB_BIN" -s "$SERIAL" shell cmd package resolve-activity --brief -a android.intent.action.MAIN "$PACKAGE" | tr -d '\r' | tail -n 1)
        case "$COMPONENT" in "$PACKAGE"/*) exec "$ADB_BIN" -s "$SERIAL" shell am start -a android.intent.action.MAIN -n "$COMPONENT" ;;
            *) echo "No launchable installed activity for $PACKAGE" >&2; exit 1;; esac
        ;;
    browser) PANEL=com.oculus.browser/.PanelActivity ;;
    *) echo 'Usage: launch.sh settings|library|installed|browser [URL], or app <package>' >&2; exit 1 ;;
esac
# adb shell receives a command string: restrict URL characters that could escape
# its quoted argument. Ordinary URLs (including query strings) remain supported.
URL=${2:-https://example.com}
case "$URL" in *\'*|*\`*|*\$*|*\\*|*' '*|*'"'*) echo 'URL contains unsupported shell characters.' >&2; exit 1;; esac
exec "$ADB_BIN" -s "$SERIAL" shell "am broadcast -a com.oculus.vrshell.intent.action.LAUNCH -n com.oculus.vrshell/.ShellControlBroadcastReceiver --es intent_data '$PANEL' --es uri '$URL'"
