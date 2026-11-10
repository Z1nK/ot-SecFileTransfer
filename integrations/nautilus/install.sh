#!/usr/bin/env bash
# Install the Nautilus integration of ftc for the current user (no sudo):
# the "Send with ftc…" / "Get with ftc…" menu items and their dialogs.
#
#   install.sh [--extension | --scripts] [--no-restart]
#
#   --extension   nautilus-python extension: dialogs in ~/.local/bin,
#                 ftc_nautilus.py in ~/.local/share/nautilus-python/extensions
#   --scripts     Nautilus scripts (right-click -> Scripts), needs only zenity
#   (neither)     --extension if nautilus-python is installed, else --scripts
#   --no-restart  do not run `nautilus -q` at the end
#
# Not needed with the confide-client package, which installs everything
# system-wide. Undo with uninstall.sh.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
DATA=${XDG_DATA_HOME:-$HOME/.local/share}
BIN=$HOME/.local/bin
SYS_EXT=/usr/share/nautilus-python/extensions/ftc_nautilus.py

mode=auto
restart=1
for arg in "$@"; do
  case $arg in
    --extension) mode=extension ;;
    --scripts) mode=scripts ;;
    --no-restart) restart=0 ;;
    -h|--help) sed -n '2,15s/^# \{0,1\}//p' "$0"; exit 0 ;;
    *) echo "install.sh: unknown option: $arg (see --help)" >&2; exit 2 ;;
  esac
done

warn() { echo "warning: $*" >&2; }

have_nautilus_python() {
  [[ -d /usr/share/nautilus-python ]] && return 0
  compgen -G '/usr/lib*/nautilus/extensions-*/libnautilus-python.so' >/dev/null && return 0
  compgen -G '/usr/lib/*/nautilus/extensions-*/libnautilus-python.so' >/dev/null
}

if [[ $mode == auto ]]; then
  if have_nautilus_python; then
    mode=extension
    echo "nautilus-python found: installing the extension."
  else
    mode=scripts
    echo "nautilus-python not found: installing as Nautilus scripts (right-click -> Scripts)."
  fi
fi

case $mode in
  extension)
    if [[ -e $SYS_EXT ]]; then
      warn "$SYS_EXT exists (confide-client package): the menu items would show twice."
      warn "Remove the package or run uninstall.sh afterwards."
    fi
    install -Dm755 "$HERE/ftc-send-dialog" "$BIN/ftc-send-dialog"
    install -Dm755 "$HERE/ftc-get-dialog" "$BIN/ftc-get-dialog"
    install -Dm644 "$HERE/ftc_nautilus.py" "$DATA/nautilus-python/extensions/ftc_nautilus.py"
    echo "installed $BIN/ftc-send-dialog"
    echo "installed $BIN/ftc-get-dialog"
    echo "installed $DATA/nautilus-python/extensions/ftc_nautilus.py"
    case ":$PATH:" in
      *":$BIN:"*) ;;
      *) warn "$BIN is not in PATH: the menu items stay hidden until it is (log out and in again)." ;;
    esac
    ;;
  scripts)
    install -Dm755 "$HERE/ftc-send-dialog" "$DATA/nautilus/scripts/Send with ftc"
    install -Dm755 "$HERE/ftc-get-dialog" "$DATA/nautilus/scripts/Get with ftc"
    echo "installed $DATA/nautilus/scripts/Send with ftc"
    echo "installed $DATA/nautilus/scripts/Get with ftc"
    ;;
esac

# What the dialogs need at run time; see README.md, "Needs".
command -v zenity >/dev/null || warn "zenity is not installed: the dialogs cannot open (see integrations/nautilus/README.md, \"Needs\")."
command -v notify-send >/dev/null || warn "notify-send is not installed: results are shown in a dialog instead."
command -v "${FTC_BIN:-ftc}" >/dev/null || warn "ftc is not in PATH."
if command -v systemctl >/dev/null \
   && ! systemctl --user show-environment 2>/dev/null | grep -q '^FTC_SERVER='; then
  warn "FTC_SERVER is not set for the desktop session; see integrations/README.md."
fi

if [[ $restart == 1 ]] && pgrep -x nautilus >/dev/null 2>&1; then
  nautilus -q || true
  echo "Nautilus stopped; it loads the new items when opened again."
else
  echo "Restart Nautilus (nautilus -q) to load the new items."
fi
