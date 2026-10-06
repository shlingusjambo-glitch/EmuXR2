#!/bin/sh
# Launch native Horizon panels through VrShell; browser accepts an optional URL.
set -eu
ADB=${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}/platform-tools/adb
case "${1:-settings}" in
    settings) PANEL=systemux://settings ;;
    library) PANEL=systemux://library ;;
    browser) PANEL=com.oculus.browser/.PanelActivity ;;
    *) echo 'Usage: launch.sh settings|library|browser [URL]' >&2; exit 1 ;;
esac
# adb shell receives a command string: restrict URL characters that could escape
# its quoted argument. Ordinary URLs (including query strings) remain supported.
URL=${2:-https://example.com}
case "$URL" in *\'*|*\`*|*\$*|*\\*|*' '*|*'"'*) echo 'URL contains unsupported shell characters.' >&2; exit 1;; esac
exec "$ADB" shell "am broadcast -a com.oculus.vrshell.intent.action.LAUNCH -n com.oculus.vrshell/.ShellControlBroadcastReceiver --es intent_data '$PANEL' --es uri '$URL'"
