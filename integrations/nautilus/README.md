# Nautilus: "Send with ftc…" and "Get with ftc…"

- **Send:** right-click files or folders → **Send with ftc…** → pick a recent
  recipient or type one → the files are sent with `ftc send`.
- **Get:** right-click the empty space of a folder → **Get with ftc…** (or
  right-click a folder → **Get with ftc into folder…**) → tick transactions
  from your inbox → they are downloaded with `ftc get` into that folder,
  keeping their paths. Only committed and delivered transactions are listed.
  If a file already exists, a dialog asks whether to overwrite it or skip
  that transaction.

If there is no valid token, a password dialog comes first.

| File               | What it does |
|--------------------|--------------|
| `ftc-send-dialog`  | Send logic (bash + zenity): recipient dialog, login, `ftc send`, result notification. |
| `ftc-get-dialog`   | Get logic (bash + zenity): inbox list, login, `ftc get`, overwrite question, result notification. |
| `ftc_nautilus.py`  | Nautilus extension: adds the menu items and starts the dialogs with the selected paths or the current folder. |

## Needs

On every distribution:

- `ftc` in `PATH`, and `FTC_SERVER` set for the desktop session (see
  [../README.md](../README.md)).
- `zenity` for the dialogs.
- `notify-send` for the result notification (optional; without it a zenity
  dialog shows the result).
- nautilus-python for the top-level menu item (optional; without it, use the
  helper as a Nautilus script, see below).

The `confide-client` package only *suggests* these, so they are not
installed with it. Check the Nautilus version with `nautilus --version`. The
extension works with both the old API (Nautilus 3.x–42) and the new one (43+).

### Debian / Ubuntu

```sh
sudo apt install zenity libnotify-bin python3-nautilus
```

### Fedora

```sh
sudo dnf install zenity libnotify nautilus-python
```

### RHEL 8 / 9 / 10 (and Rocky, Alma)

```sh
sudo dnf install zenity libnotify      # AppStream / BaseOS
dnf info nautilus-python               # is it available?
```

- `nautilus-python` may not be in the RHEL repositories. If `dnf info` finds
  nothing, enable EPEL (`sudo dnf install epel-release` on Rocky/Alma; on
  RHEL, the `epel-release` RPM from the Fedora project) and try again. If
  it is not there either, use the Nautilus script fallback below. It needs
  only zenity.
- dnf does not install the packages that `confide-client` suggests, so
  install the ones above yourself.
- RHEL 8 (GNOME 3.28) may not read `~/.config/environment.d/` for the
  desktop session. If the dialog says "No ftc server configured", put
  `export FTC_SERVER=https://...` in `~/.bash_profile` instead and log in
  again.

## Install

From the `confide-client` package, all files are already in place
(`/usr/bin/ftc-send-dialog`, `/usr/bin/ftc-get-dialog`,
`/usr/share/nautilus-python/extensions/`);
only restart Nautilus: `nautilus -q`.

By hand, for one user:

```sh
install -Dm755 ftc-send-dialog ~/.local/bin/ftc-send-dialog
install -Dm755 ftc-get-dialog ~/.local/bin/ftc-get-dialog
install -Dm644 ftc_nautilus.py ~/.local/share/nautilus-python/extensions/ftc_nautilus.py
nautilus -q
```

`~/.local/bin` must be in the session's `PATH` (it is on most distributions
once the folder exists at login).

### Without nautilus-python

Use the helpers as Nautilus scripts; they then show under right-click →
**Scripts**:

```sh
install -Dm755 ftc-send-dialog ~/.local/share/nautilus/scripts/"Send with ftc"
install -Dm755 ftc-get-dialog ~/.local/share/nautilus/scripts/"Get with ftc"
```

"Get with ftc" downloads into the selected folder if exactly one folder is
selected, else into the folder being shown.

## Uninstall

```sh
rm ~/.local/bin/ftc-send-dialog ~/.local/bin/ftc-get-dialog \
   ~/.local/share/nautilus-python/extensions/ftc_nautilus.py \
   ~/.local/share/nautilus/scripts/"Send with ftc" \
   ~/.local/share/nautilus/scripts/"Get with ftc"
nautilus -q
```

## Troubleshooting

- **No menu item:** run `nautilus -q; nautilus` in a terminal and look for
  Python errors. An item is hidden when its helper (`ftc-send-dialog`,
  `ftc-get-dialog`) is not in `PATH`, or the selection or folder is not
  local (e.g. network files).
- **"No ftc server configured":** `FTC_SERVER` is not in the session
  environment; see [../README.md](../README.md).
- **Test the dialogs without Nautilus:** `ftc-send-dialog some-file`,
  `ftc-get-dialog ~/Downloads`.
