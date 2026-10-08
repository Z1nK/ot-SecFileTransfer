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
| create / upload / delete / commit / list / download over REST | forwarding to a peer server (transactions to `user@otherServer` stay `committed`) |
| TLS on/off | retention sweep (expired transactions are not deleted yet) |
| Basic auth, access rules, path checks | Bearer tokens for users (`auth.token_ttl_s` is read but unused) |
| business and technical logs, log rotation | |
| `/internal/...` peer endpoints (receiving side) | |
| `ftc` client: `send`, `inbox`, `outbox`, `show`, `files`, `get` | |

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

To build without tests, add `-DWITH_GOOGLE_TEST=OFF` to the configure command.

Optional: `cmake --install build` copies `confide-server` and `confide-passwd` to
`install/bin/`, and `cd build && cpack` makes a `.deb` package.

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

### Settings

Only `instance_name`, `[storage].root` and, with TLS on, `tls.cert` / `tls.key` are required.
Everything else has a default. Unknown keys give a warning in the technical log.

| Key | Default | Meaning |
|-----|---------|---------|
| `instance_name` | — | name of this server, `[A-Za-z0-9._-]`; the `siteA` in `bob@siteA` |
| `storage.root` | — | data folder; holds `tmp/`, `transactions/` and (by default) `logs/` |
| `rest.bind` | `0.0.0.0` | listen address |
| `rest.port` | `8080` | listen port |
| `rest.threads` | `0` | worker threads, `0` = number of CPU cores |
| `rest.request_timeout_ms` | `30000` | request timeout |
| `tls.enabled` | `true` | HTTPS on/off. `false` sends passwords and files in clear text: local testing only |
| `tls.cert`, `tls.key` | — | PEM certificate and key, required when TLS is on |
| `tls.ca` | system CAs | CA bundle to verify peer servers |
| `[[user]]` `name`, `password_hash` | — | one block per local user |
| `log.level` | `info` | `trace`, `debug`, `info`, `warn`, `error` |
| `log.dir` | `<storage.root>/logs` | folder for `technical.log` and `business.log` |
| `log.rotate_bytes` | `67108864` | rotate a log at this size, `0` = never |
| `retention.default_days` | `30` | retention when the client does not set one (must be > 0) |
| `retention.max_days` | `365` | upper limit for `retention_days` |
| `retention.sweep_interval_s` | `3600` | how often to look for expired transactions |
| `[[peer]]` `name`, `host`, `port` | port `8080` | another server that transactions can be sent to |
| `[[peer]]` `token` | — | secret this server sends to the peer |
| `[[peer]]` `accept_token` | — | secret the peer must send to us (`Authorization: Bearer ...`). Use a different secret than `token` |

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

[[user]]
name = "alice"
password_hash = "<output of confide-passwd>"

[[user]]
name = "bob"
password_hash = "<output of confide-passwd>"
```

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

All user requests use HTTP Basic auth (`curl -u user:password`). Errors come back as
`{"error": "<code>", "detail": "..."}`.

| Method | Path | Purpose |
|--------|------|---------|
| `POST` | `/transactions` | create; body `{"target_user": "bob", "target_server": "", "retention_days": 30}` (only `target_user` is required) |
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

## 5. Check

### Unit tests

```bash
ctest --preset x64-debug-linux
```

or one module: `build/bin/transaction_tests`, `build/bin/api_tests`, and so on
(all GoogleTest, so `--gtest_filter=...` works).

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
src/lib/           common, config, logging, storage, auth, transaction, api
tests/unit/        GoogleTest unit tests, one folder per module
config/            example server config
```
