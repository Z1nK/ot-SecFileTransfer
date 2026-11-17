# Nautilus: "Send with ftc…" and "Get with ftc…"

- **Send:** right-click files or folders → **Send with ftc…** → pick a recent
  recipient or type one, optionally add a comment → the files are sent with
  `ftc send` (the comment with `--comment`).
- **Get:** right-click the empty space of a folder → **Get with ftc…** (or
  right-click a folder → **Get with ftc into folder…**) → tick transactions
  from your inbox → they are downloaded with `ftc get` into that folder,
  keeping their paths. Only committed and delivered transactions are listed.
  If a file already exists, a dialog asks whether to overwrite it or skip
  that transaction.

If there is no valid token, a password dialog comes first.

| File               | What it does |
|--------------------|--------------|
| `ftc-send-dialog`  | Send logic (bash + zenity): recipient and comment dialog, login, `ftc send`, result notification. |
| `ftc-get-dialog`   | Get logic (bash + zenity): inbox list, login, `ftc get`, overwrite question, result notification. |
| `ftc_nautilus.py`  | Nautilus extension: adds the menu items and starts the dialogs with the selected paths or the current folder. |
| `install.sh`       | Installs the above for the current user (without the package). |
| `uninstall.sh`     | Removes what `install.sh` installed. |

## Needs

On every distribution:

- `ftc` in `PATH`, and `FTC_SERVER` set for the desktop session (see
  [../README.md](../README.md)).
- `zenity` for the dialogs.
- `notify-send` for the result notification (optional; without it a zenity
  dialog shows the result).
- nautilus-python for the top-level menu item (optional; without it, use the
  helper as a Nautilus script, see below).

The `confide-client-nautilus` package requires `zenity` and recommends
the other two (installed by default with apt and dnf, if the repositories
have them). Installing by hand, put them in place yourself, as below. Check
the Nautilus version with `rpm -q nautilus` / `dpkg -l nautilus`. The
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
- If nautilus-python is missing, `confide-client-nautilus` still installs
  (it is only recommended), but the top-level menu items stay away; use the
  Nautilus script fallback below.
- RHEL 8 (GNOME 3.28) may not read `~/.config/environment.d/` for the
  desktop session. If the dialog says "No ftc server configured", put
  `export FTC_SERVER=https://...` in `~/.bash_profile` instead and log in
  again.

## Install

The integration is a package of its own, `confide-client-nautilus`, so that
`confide-client` (only `ftc`) stays free of desktop files on servers and
headless VMs. On a desktop:

```sh
sudo dnf install ./confide-client-*.rpm ./confide-client-nautilus-*.rpm   # RHEL / Fedora
sudo apt install ./confide-client_*.deb ./confide-client-nautilus_*.deb   # Debian / Ubuntu
nautilus -q
```

It puts `/usr/bin/ftc-send-dialog`, `/usr/bin/ftc-get-dialog` and
`/usr/share/nautilus-python/extensions/ftc_nautilus.py` in place and needs
the `confide-client` of the same version. Up to 0.7.6 these files were in
`confide-client` itself.

Without the package, for one user (no sudo), from this folder:

```sh
./install.sh
```

With nautilus-python it installs the dialogs in `~/.local/bin/` and the
extension in `~/.local/share/nautilus-python/extensions/`; without it, the
dialogs become Nautilus scripts (see below). It then restarts Nautilus
(`--no-restart` to skip) and warns about anything missing: zenity, `ftc`,
`FTC_SERVER`, or `~/.local/bin` not in the session's `PATH` (it is on most
distributions once the folder exists at login). Don't combine it with the
package: the menu items would show twice.

### Without nautilus-python

```sh
./install.sh --scripts
```

installs the dialogs as Nautilus scripts in
`~/.local/share/nautilus/scripts/`; they then show under right-click →
**Scripts** as "Send with ftc" and "Get with ftc". This is the default when
nautilus-python is not found; `--extension` forces the extension.

"Get with ftc" downloads into the selected folder if exactly one folder is
selected, else into the folder being shown.

## Uninstall

```sh
./uninstall.sh
```

removes the files of both variants from your home and restarts Nautilus.
Files from the `confide-client-nautilus` package are left alone; remove the
package with apt/dnf.

## Troubleshooting

- **"Send with ftc…" shows twice:** Nautilus loads `ftc_nautilus.py` from
  `~/.local/share/nautilus-python/extensions/` *and* from the same folder
  under every entry of `XDG_DATA_DIRS` (`/usr/local/share`, `/usr/share`,
  …), typically the package (`confide-client-nautilus`, or `confide-client`
  0.7.6) plus a per-user install. Find
  them with
  `find / -name ftc_nautilus.py -path '*nautilus-python*' 2>/dev/null`
  and keep one; `install.sh` and `uninstall.sh` list the other copies too.
  A copy with `class FtcSendMenu` is the first version, with Send only.
- **No "Get with ftc…" on files:** it is offered only on the empty space of
  a folder ("Get with ftc…") or on a single selected folder ("Get with ftc
  into folder…"), not on files.
- **No menu item:** run `nautilus -q; nautilus` in a terminal and look for
  Python errors. An item is hidden when its helper (`ftc-send-dialog`,
  `ftc-get-dialog`) is not in `PATH`, or the selection or folder is not
  local (e.g. network files).
- **"No ftc server configured":** `FTC_SERVER` is not in the session
  environment; see [../README.md](../README.md).
- **Test the dialogs without Nautilus:** `ftc-send-dialog some-file`,
  `ftc-get-dialog ~/Downloads`.
