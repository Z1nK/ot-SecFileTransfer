# Secure Transaction-Based File Transfer Service (C++)

`confide` is a C++23 server that sends files between users as **immutable transactions**.
The sender creates a transaction, uploads files into it and commits it. After the commit
nothing in it can change, and the target user can download the files.

- REST API over HTTP or HTTPS (Boost.Beast/Asio, thread pool, streamed bodies in 1 MB chunks)
- SHA-256 for every file, checked on upload and on commit
- Users from the config file, passwords stored as PBKDF2-HMAC-SHA256 hashes, HTTP Basic auth
- Path traversal (`../`, absolute paths) is rejected
- Two logs: `business.log` (who sent what to whom, JSON lines) and `technical.log`

See [Requirements.md](Requirements.md) and [Architecture.md](Architecture.md) for the design.

### Status

| Works now | Not done yet |
|-----------|--------------|
| create / upload / delete / commit / list / download over REST | resume of an interrupted upload (FR-15, "could") |
| forwarding to a peer (`bob@siteB`): pushed after commit, retried with backoff while the peer is down, `delivered` once it confirmed | |
| TLS on/off | |
| retention: expired transactions are deleted every `sweep_interval_s` | |
| Basic auth and user tokens (`ftc login`), login throttling, access rules, path checks | |
| business and technical logs, log rotation | |
| `ftc` client: `login`, `logout`, `send`, `inbox`, `outbox`, `show`, `files`, `get` | |
| unit tests, a two-server end-to-end test, a benchmark with a memory check | |

## 1. Build

### Requirements

- Linux (tested on Ubuntu 26.04 / WSL2)
- g++ 15 (C++23)
- CMake 3.21+ and Ninja
- [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set. It installs `toml11` and `gtest` from [vcpkg.json](vcpkg.json).
- Boost 1.83+ (`json` and `program_options` components, plus header-only Asio/Beast) and OpenSSL, from the system

On Ubuntu:

```bash
sudo apt install g++ cmake ninja-build libboost-all-dev libssl-dev
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg        # add to your shell profile
```

### Configure and build

The presets in [CMakePresets.json](CMakePresets.json) set the vcpkg toolchain and Ninja.

```bash
cmake --preset x64-debug-linux           # or x64-release-linux
cmake --build --preset x64-debug-linux
```

Both presets use the same directory `build/`. Binaries go to `build/bin/`:

| Binary | What it is |
|--------|------------|
| `confide-server` | the server |
| `confide-passwd` | prints a password hash for the config |
| `ftc` | command-line client (`ftc --help`) |
| `*_tests` | unit tests (one per module) |
| `example-*` | small examples, not needed to run the server |

To build without tests, add `-DWITH_GOOGLE_TEST=OFF` to the configure command; without the
examples, `-DWITH_EXAMPLES=OFF`.

LDAP login (`auth.provider = "ldap"`, see [server.example.toml](config/server.example.toml)) is
optional: add `-DWITH_LDAP=ON` and install OpenLDAP (`sudo apt install libldap-dev`). A test
directory runs with `docker compose -f docker/docker-compose.yml up -d`; its tests run with
`CONFIDE_LDAP_TEST_URI=ldap://localhost:1389 ctest --test-dir build -R Ldap`.

Optional: `cmake --install build` copies `confide-server` and `confide-passwd` to
`install/bin/`, and `cd build && cpack` makes a `.deb` package. The install also writes the
config to `/etc/confide`; add `-DCONFIDE_CONFIG_DIR=etc/confide` to the configure command to
keep it under `install/` instead.

### Installing the package

The `confide` package (`.deb` or `.rpm`) installs:

| Path | |
|---|---|
| `/usr/bin/confide-server` | the server |
| `/etc/confide/server.toml` | config, a copy of [server.example.toml](config/server.example.toml); kept on upgrade if edited |
| `/usr/lib/systemd/system/confide-server.service` | systemd unit, the same on Ubuntu and RHEL |

It also adds a `confide` system user, which runs the server. `/var/lib/confide` (storage) and
`/var/log/confide` (logs) are created by systemd when the service starts. The service is not
started or enabled on install, because the config has to be edited first:

```bash
sudo editor /etc/confide/server.toml          # instance name, TLS cert/key, peers
# users file next to it, readable by the server only:
sudo install -m 600 -o confide -g confide /dev/null /etc/confide/users
read -rs PW && printf '%s\n' "$PW" | confide-passwd --user alice | sudo tee -a /etc/confide/users
sudo systemctl enable --now confide-server
journalctl -u confide-server                  # startup errors; the rest is in /var/log/confide
```

The TLS key and any other file named in the config must also be readable by the `confide`
user (for secrets: owner `confide`, mode 600). A different `storage.root` or `log.dir` must be made writable with
`sudo systemctl edit confide-server` (`ReadWritePaths=...`).

## 2. Configure

The server reads one TOML file. Start from [config/server.example.toml](config/server.example.toml).
Relative paths in the file are resolved against the directory of the config file.

### Password hashes

Plaintext passwords are not allowed in the config. Make a hash with `confide-passwd`.
It reads the password from the first line of stdin, so the password does not end up
in the shell history or in `ps`:

```bash
read -rs PW && printf '%s\n' "$PW" | build/bin/confide-passwd
# pbkdf2-sha256$600000$...$...
```

`--iterations N` changes the PBKDF2 cost (lower values are only for local tests).

### Users file

Users are kept in a separate file, like `/etc/passwd`: one `name:password_hash` per line.
The config points to it with `auth.users_file`. Blank lines and lines starting with `#` are
ignored. A bad line stops the server from starting, with the file and line number in the
message. Start from [config/users.example](config/users.example).

```
# name:password_hash
alice:pbkdf2-sha256$600000$...$...
bob:pbkdf2-sha256$600000$...$...
```

`confide-passwd --user NAME` prints the whole line, so you can append it directly:

```bash
read -rs PW && printf '%s\n' "$PW" | build/bin/confide-passwd --user alice >> users
chmod 600 users
```

The server warns in the technical log if other users can read the file. Users can also be
put in the main config as `[[user]]` blocks; both sources are merged, and the same name in
both is an error. The file is read once at startup: restart the server after a change.

### Groups file

Groups are optional and kept in a file like `/etc/group`, without the password and GID
fields: one `name:member,member,...` per line. The config points to it with
`auth.groups_file`. Blank lines and `#` comments are ignored, spaces around members are
allowed, and a group may have no members. Start from
[config/groups.example](config/groups.example).

```
# name:member,member,...
devs:alice,bob
ops:carol
```

A group is a receiver like a user: `ftc send --to devs` or, from another server,
`--to devs@siteA`. It is still one transaction with one copy of the files. Every member
sees it in their inbox once it is committed and can download it, and only the sender can
change it. Membership is checked on each request, so a new member also sees older group
transactions and a removed member loses access.

Users and groups share one namespace: a group with a user's name is an error, and so is a
member that is not a configured user. Groups can also be put in the main config as
`[[group]]` blocks (`name`, `members = ["alice", "bob"]`). The file is read once at
startup: restart the server after a change.

### Settings

Only `instance_name`, `[storage].root` and, with TLS on, `tls.cert` / `tls.key` are required.
Everything else has a default. Unknown keys give a warning in the technical log.

| Key | Default | Meaning |
|-----|---------|---------|
| `instance_name` | — | name of this server, `[A-Za-z0-9._-]`; the `siteA` in `bob@siteA` |
| `storage.root` | — | data folder; holds `tmp/`, `transactions/` and (by default) `logs/` |
| `rest.bind` | `0.0.0.0` | listen address: IP (`0.0.0.0`, `::`, `10.1.2.3`) or host name, resolved once at startup |
| `rest.port` | `8080` | listen port |
| `rest.threads` | `0` | worker threads, `0` = number of CPU cores |
| `rest.request_timeout_ms` | `30000` | request timeout |
| `tls.enabled` | `true` | HTTPS on/off. `false` sends passwords and files in clear text: local testing only |
| `tls.cert`, `tls.key` | — | PEM certificate and key, required when TLS is on |
| `tls.ca` | system CAs | CA bundle to verify peer servers |
| `auth.users_file` | — | file with one `name:password_hash` per line (see [Users file](#users-file)) |
| `[[user]]` `name`, `password_hash` | — | one block per local user, in addition to `users_file` |
| `auth.groups_file` | — | file with one `name:member,member,...` per line (see [Groups file](#groups-file)) |
| `[[group]]` `name`, `members` | — | one block per group, in addition to `groups_file` |
| `log.level` | `info` | `trace`, `debug`, `info`, `warn`, `error` |
| `log.dir` | `<storage.root>/logs` | folder for `technical.log` and `business.log` |
| `log.rotate_bytes` | `67108864` | rotate a log at this size, `0` = never |
| `retention.default_days` | `30` | retention when the client does not set one (must be > 0) |
| `retention.max_days` | `365` | upper limit for `retention_days` |
| `retention.sweep_interval_s` | `3600` | how often to look for expired transactions |
| `[[peer]]` `name`, `host`, `port` | port `8080` | another server that transactions can be sent to |
| `[[peer]]` `token` | — | secret this server sends to the peer |
| `[[peer]]` `accept_token` | — | secret the peer must send to us (`Authorization: Bearer ...`) |

Peer tokens are shared secrets you make yourself, two per pair of servers (one per
direction), e.g. with `openssl rand -hex 32`. siteA's `token` for siteB is siteB's
`accept_token` for siteA, and the other way round. The config refuses a token shorter
than 32 characters, one with spaces or control characters, one starting with `ftu_`
(the prefix of user tokens), and any secret used twice, in the same block or across
peers: the `accept_token` alone tells the server which peer is calling. The server
keeps only their SHA-256 in memory and compares in constant time.

Peers are called over https when `[tls] enabled = true` (the same switch as our own
API) and verified with `tls.ca`, so both servers must use TLS or both not. The peer's
certificate must name `host`. Each pair of servers shares two tokens, one per direction:
siteA's `token` is siteB's `accept_token` for siteA, and the other way round.

After a commit to `bob@siteB`, siteA pushes the transaction to siteB in the
background and marks it `delivered`; `ftc show ID` on siteA tells you which. If
siteB can't be reached, siteA retries after 5 s, 10 s, 20 s ... up to every 10
minutes, and again after a restart. Errors that a retry can't fix (no user `bob` on
siteB, wrong token) are logged in siteA's `technical.log`; the transaction then
stays `committed` until the next start.

### Minimal local config

A config for testing on one machine, without TLS:

```toml
instance_name = "siteA"

[storage]
root = "./data"

[rest]
bind = "127.0.0.1"
port = 8080

[tls]
enabled = false

[auth]
users_file = "users"
```

and next to it a `users` file with lines from `confide-passwd --user alice` and
`confide-passwd --user bob`.

### TLS

For a test certificate:

```bash
openssl req -x509 -newkey rsa:2048 -nodes -days 365 -subj "/CN=localhost" \
  -keyout key.pem -out cert.pem
```

```toml
[tls]
enabled = true
cert = "cert.pem"
key = "key.pem"
```

Then call the server with `curl --cacert cert.pem https://localhost:8080/...`.

## 3. Run

```bash
build/bin/confide-server path/to/server.toml
```

The server creates the storage and log folders if they don't exist, writes
`<instance> listening on <addr>:<port> (http|https)` to `technical.log`, and stops cleanly on
`Ctrl+C` (SIGINT) or SIGTERM. Startup errors (bad config, port in use, bad password hash)
are printed to stderr and the exit code is `1`. Transactions are kept on disk and loaded
again after a restart.

## 4. Use the API

User requests carry either HTTP Basic auth (`curl -u user:password`) or a user token
(`Authorization: Bearer ftu_...`) from `POST /auth/token`. Errors come back as
`{"error": "<code>", "detail": "..."}`.

| Method | Path | Purpose |
|--------|------|---------|
| `POST` | `/auth/token` | log in: Basic auth only (a token can't get a new token); answers `{"token": "ftu_...", "user": "alice", "expires_at": "..."}` |
| `DELETE` | `/auth/token` | log out: the Bearer token of the request stops working |
| `POST` | `/transactions` | create; body `{"target_user": "bob", "target_server": "", "retention_days": 30, "comment": "Q3 reports"}` (only `target_user` is required; `comment` is at most 256 characters, no control characters) |
| `GET` | `/transactions?box=in\|out` | my incoming (default) or outgoing transactions |
| `GET` | `/transactions/{id}` | one transaction and its state |
| `PUT` | `/transactions/{id}/files/{path}` | upload a file (raw body). Optional header `X-Checksum-SHA256: <hex>` |
| `DELETE` | `/transactions/{id}/files/{path}` | remove a file (only while `open`) |
| `POST` | `/transactions/{id}/commit` | lock the transaction |
| `GET` | `/transactions/{id}/files` | list files with size and sha256 |
| `GET` | `/transactions/{id}/files/{path}` | download a file; the response has `X-Checksum-SHA256` |
| `POST`/`PUT` | `/internal/transactions/...` | server-to-server, peer Bearer token only |

| HTTP status | Error code | When |
|-------------|------------|------|
| 400 | `validation` | bad JSON, bad id, bad path (`..`, absolute) |
| 401 | `auth` | wrong or missing credentials |
| 403 | `forbidden` | not your transaction |
| 404 | `notfound` | no such transaction or file |
| 409 | `conflict` | change to a committed transaction |
| 422 | `integrity` / `unknown_destination` | checksum mismatch / unknown target user or server |
| 429 | `throttled` | too many wrong passwords for this user or from this address; wait and retry |

Tokens live `auth.token_ttl_s` (default 3600) from login and are dropped after
`auth.token_idle_s` (default 900) without use. They are kept in memory only, so a server
restart logs everybody out. After 5 wrong passwords for one user from one address (20
from one address, 50 for one user), the server stops checking passwords for that key
for 30 s, doubling up to 15 min; with LDAP this keeps the server from being used to run
accounts into the directory's own lockout.

### Full flow with curl

alice sends a file to bob on the same server:

```bash
U=http://127.0.0.1:8080

# 1. alice creates a transaction for bob
curl -u alice:alice-pw -X POST $U/transactions -d '{"target_user":"bob"}'
# {"id":"b3c18f72-...","state":"open",...}
ID=b3c18f72-...

# 2. alice uploads files; the path after /files/ is kept as the file's folder path
curl -u alice:alice-pw -X PUT \
  -H "X-Checksum-SHA256: $(sha256sum a.txt | cut -d' ' -f1)" \
  --data-binary @a.txt $U/transactions/$ID/files/report/a.txt

# 3. alice commits
curl -u alice:alice-pw -X POST $U/transactions/$ID/commit
# {"id":"...","state":"committed",...}

# 4. bob lists incoming transactions and their files
curl -u bob:bob-pw "$U/transactions?box=in"
curl -u bob:bob-pw $U/transactions/$ID/files

# 5. bob downloads
curl -u bob:bob-pw -o a.txt $U/transactions/$ID/files/report/a.txt
```

Use `--data-binary` (not `-d`) for uploads, so curl does not change the file.

### Same flow with ftc

`ftc send` does steps 1–3 in one command: it creates the transaction, uploads every file
with its SHA-256 and commits. `ftc get` downloads all files of a transaction and checks
each SHA-256 before it keeps the file.

The server comes from `-s` / `FTC_SERVER`, the user from `-u` / `FTC_USER`. The first
command (or `ftc login`) asks for the password on the terminal without echo, trades it
for a token and saves the token in
`$XDG_STATE_HOME/ftc/tokens/<user>@<host>_<port>` (default `~/.local/state/...`, folder
0700, file 0600; ftc refuses a token file that other users can read). Later commands send
only the token. When it expires, ftc asks for the password again; in a script (stdin not a
terminal) it fails and tells you to run `ftc login`. `ftc logout` ends the session on the
server and deletes the file. `FTC_PASSWORD` still works instead of typing (development
only: other processes of the same user can read the environment).

ftc refuses a plain `http://` server, since the password and token would travel
unencrypted. For local testing against a server with `[tls] enabled = false`, add
`--allow-http`, as below.

```bash
export FTC_SERVER=http://127.0.0.1:8080

# 0. log in once; the token is used by the next commands
build/bin/ftc --allow-http -u alice login
# password for alice:
# logged in as alice until 2026-10-13T21:00:00Z

# 1-3. alice sends the folder report/ (holds a.txt) to bob; a folder keeps its
#      name, so the file arrives as report/a.txt
build/bin/ftc --allow-http -u alice send --to bob report/
# created b3c18f72-... for bob
#   report/a.txt
# committed b3c18f72-... (1 files)
ID=b3c18f72-...

# 4. bob lists incoming transactions and their files (asks bob's password once)
build/bin/ftc --allow-http -u bob inbox
build/bin/ftc --allow-http -u bob show $ID
build/bin/ftc --allow-http -u bob files $ID

# 5. bob downloads into ./downloads (creates ./downloads/report/a.txt)
build/bin/ftc --allow-http -u bob get $ID downloads
```

Notes:

- `--to bob@siteB` sends to a user on another server, `--retention DAYS` sets `retention_days`.
- `--comment "Q3 reports"` (`-m`) attaches a short note, shown in the COMMENT column of `inbox`/`outbox`.
- `ftc outbox` lists the transactions you sent.
- `get` does not overwrite existing files unless you add `--force`.
- If an upload fails, `send` prints the id of the transaction it left `open`.
- With TLS, use an `https://` URL and `--ca-file cert.pem` (or `FTC_CA_FILE`) for a self-signed certificate.

## 5. Check

### Tests

```bash
ctest --preset x64-debug-linux              # unit and end-to-end tests
ctest --preset x64-debug-linux -L integration   # only the end-to-end test
```

or one module: `build/bin/transaction_tests`, `build/bin/api_tests`, and so on
(all GoogleTest, so `--gtest_filter=...` works).

The end-to-end test [tests/integration/two_sites.sh](tests/integration/two_sites.sh)
starts two servers with TLS on this machine and runs `ftc` against them: a local
transfer, a transfer forwarded from siteA to siteB, the immutability and unknown
destination checks, and a transfer that waits while siteB is down. It needs
`openssl` and `curl`, and keeps its folder with the logs if it fails:

```bash
tests/integration/two_sites.sh build/bin
```

### Benchmark and memory

[tests/bench/bench.sh](tests/bench/bench.sh) sends 1000 small files and one large
file (2 GiB by default) through two servers with TLS, forwarding the large one, and
prints times, throughput and the peak memory of both servers and `ftc`. It fails if a
download differs from what was sent, or if a process peaks above 100 MiB (NFR-4).
Use a Release build, and a `TMPDIR` with four times the file size free:

```bash
TMPDIR=/var/tmp tests/bench/bench.sh build/bin            # 2048 MiB, 1000 files
tests/bench/bench.sh build/bin 256 200                    # quicker
```

### Manual checks

With the server running and the flow above done, these should hold:

| Check | Command | Expected |
|-------|---------|----------|
| committed is immutable | `curl -u alice:alice-pw -X PUT --data-binary @a.txt $U/transactions/$ID/files/b.txt` | `409` `conflict` |
| wrong password | `curl -u bob:wrong $U/transactions` | `401` `auth` |
| path traversal | `curl -u alice:alice-pw -X PUT --data-binary @a.txt "$U/transactions/$ID/files/..%2Fx"` | `400` `validation` |
| bad checksum | upload with `-H "X-Checksum-SHA256: 0000...0000"` (64 zeros) | `422` `integrity` |
| other user's transaction | a third user requests `$U/transactions/$ID` | `404` (the server does not reveal that it exists) |
| downloaded file is intact | `sha256sum a.txt` | same as `X-Checksum-SHA256` from the download |

### What is on disk

```
<storage.root>/
├── tmp/                         # uploads in progress, cleaned on restart
├── transactions/<id>/
│   ├── meta.json                # sender, target, state, times
│   ├── manifest.json            # after commit: files, sizes, sha256
│   └── files/<relative/path>    # the files
└── logs/
    ├── business.log             # tx_created, file_added, committed, downloaded, ...
    └── technical.log
```

`business.log` after the flow above:

```json
{"v":1,"ts":"...","event":"tx_created","tx":"b3c18f72-...","actor":"alice","from":"alice","to":"bob"}
{"v":1,"ts":"...","event":"file_added","tx":"b3c18f72-...","actor":"alice","from":"alice","to":"bob","file":"report/a.txt","size":6,"sha256":"5891b5..."}
{"v":1,"ts":"...","event":"committed","tx":"b3c18f72-...","actor":"alice","from":"alice","to":"bob","files":1,"bytes":6}
{"v":1,"ts":"...","event":"downloaded","tx":"b3c18f72-...","actor":"bob","from":"alice","to":"bob","file":"report/a.txt","size":6,"sha256":"5891b5..."}
```

## Project layout

```
src/app/main/      confide-server entry point
src/app/passwd/    confide-passwd
src/app/cli/       ftc
src/lib/           common, config, logging, storage, auth, transaction, retention,
                   net (HTTP client, shared by ftc and forwarder), api, forwarder
tests/unit/        GoogleTest unit tests, one folder per module
tests/integration/ end-to-end test with two servers
tests/bench/       benchmark and memory check
config/            example server config
```
