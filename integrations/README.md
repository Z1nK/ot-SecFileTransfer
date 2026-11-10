# Desktop integrations

File manager actions that hand the work to `ftc`. They contain no transfer
logic of their own: `ftc` keeps the token, logs in, uploads and commits; the
integrations only collect the selected paths and the recipient.

| Folder       | Desktop        | Status |
|--------------|----------------|--------|
| `nautilus/`  | GNOME Files    | right-click → "Send with ftc…", "Get with ftc…" |
| `explorer/`  | Windows        | planned |

They use two `ftc` commands meant for front ends:

- `ftc recent` prints the last recipients of `ftc send` (newest first, at
  most 10), without contacting the server. The list is kept per server and
  user, next to the token (`~/.local/state/ftc/recent/`,
  `%LOCALAPPDATA%\ftc\recent\`).
- `ftc login` reads the password from the first line of stdin when stdin is
  not a terminal, so a dialog can pipe it in.

## Telling ftc which server to use

A file manager does not read `~/.bashrc` or `~/.zshrc`, so `FTC_SERVER` set
there is not seen. On Linux with systemd, set it for the whole desktop
session in `~/.config/environment.d/ftc.conf`:

```
FTC_SERVER=https://ftc.example.org:8443
# FTC_USER=alice                  # default: your login name
# FTC_CA_FILE=/path/to/ca.pem     # default: system CAs
```

then log out and in again. Check with `systemctl --user show-environment | grep FTC_`.
