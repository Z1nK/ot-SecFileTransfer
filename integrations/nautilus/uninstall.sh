#!/usr/bin/env bash
# Remove the per-user Nautilus integration of ftc put in place by install.sh
# (both the extension and the Nautilus script variants). Leaves files from
# the confide-client package under /usr alone.
#
#   uninstall.sh [--no-restart]

set -euo pipefail

DATA=${XDG_DATA_HOME:-$HOME/.local/share}
BIN=$HOME/.local/bin
SYS_EXT=/usr/share/nautilus-python/extensions/ftc_nautilus.py

restart=1
for arg in "$@"; do
  case $arg in
    --no-restart) restart=0 ;;
    -h|--help) sed -n '2,6s/^# \{0,1\}//p' "$0"; exit 0 ;;
    *) echo "uninstall.sh: unknown option: $arg (see --help)" >&2; exit 2 ;;
  esac
done

files=(
  "$BIN/ftc-send-dialog"
  "$BIN/ftc-get-dialog"
  "$DATA/nautilus-python/extensions/ftc_nautilus.py"
  "$DATA/nautilus/scripts/Send with ftc"
  "$DATA/nautilus/scripts/Get with ftc"
)

removed=0
for f in "${files[@]}"; do
  if [[ -e $f || -L $f ]]; then
    rm -f -- "$f"
    echo "removed $f"
    removed=$((removed + 1))
  fi
done
[[ $removed == 0 ]] && echo "Nothing to remove: the integration is not installed for $USER."

if [[ -e $SYS_EXT ]]; then
  echo "note: $SYS_EXT is installed by the confide-client package; remove that with apt/dnf."
fi

if [[ $removed -gt 0 ]]; then
  if [[ $restart == 1 ]] && pgrep -x nautilus >/dev/null 2>&1; then
    nautilus -q || true
    echo "Nautilus stopped; the items are gone when it is opened again."
  else
    echo "Restart Nautilus (nautilus -q) to drop the items."
  fi
fi
