# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project uses [Semantic Versioning](https://semver.org/).

Each release is split into two parts:

- **Business logic**: what users and administrators see — features, rules, API and CLI behaviour, configuration options.
- **Technical**: internals — code structure, build, packaging, CI, tests, tooling.

## [Unreleased] — HEAD

Changes since `v0.7.0`, not tagged yet.

### Business logic

#### Changed
- Upload and download file I/O and password checks (LDAP) run on a separate thread pool, so a slow disk, a long `fsync` or a slow directory server no longer stops the server from accepting and serving other requests. New option `rest.blocking_threads` (default `0` = 2 × CPU cores, at least 4).
- A local group named in `auth.admin_group` is refused as a transaction target (`unknown_destination`), so one send can't reach every admin.
- Peer tokens may not start with `fta_` either (the prefix of admin tokens).

#### Added
- **Admin interface**: `auth.admin_group` names the group whose members administer the server, for both local and LDAP users. Admins can list all transactions (metadata only, never file contents), show one with its files, delete one, change its retention, see server status and live sessions, end a user's sessions, and read business-log lines for a time range. REST under `/admin/...`, CLI as `ftc admin ...`. Without `admin_group` there is no admin API.
- Admin tokens (`fta_...`, from `POST /admin/token` / `ftc admin login`) are separate from user tokens and only valid on `/admin/...`; their lifetime is `auth.admin_token_ttl_s` / `auth.admin_token_idle_s` (default: the user token values). Group membership is checked on every admin request, so removal from the group takes effect at once.
- Business-log events `admin_deleted`, `retention_changed`, `sessions_revoked`, with the admin as `actor`.
- `ftc create`, `ftc add`, `ftc rm`, `ftc commit`: build a transaction step by step. `create` opens a transaction and prints its id, `add` uploads files and folders into it (same path rules as `send`), `rm` removes files by their path in the transaction, `commit` closes it. `ftc send` still does all steps at once.

### Technical

#### Changed
- `api::offload(pool, fn)` ([api/blocking.hpp](src/lib/api/blocking.hpp)) runs a blocking call on the server's `blocking` pool and resumes the coroutine on its strand; handlers get the pool as `RequestContext::blocking`. Used for authentication, `begin_upload`, upload chunk writes, `finish`, `open_download` and download chunk reads.
- `ftc` source split from one `main.cpp` into modules (`dispatch`, `session`, `password`, `reply`, `local_files`, `outgoing`) and one file per command group under `src/app/cli/commands/`. A command is a `Command` entry in a table, not a branch of an if-chain.

#### Added
- `logging::read_business_log`: business-log lines in a time range from `business.log.1` and `business.log`.
- `common::parse_time_arg`: `YYYY-MM-DD`, ISO 8601, `now` or an age like `2h` / `7d`, for `ftc admin` options.
- `net::HttpClient::login` / `logout` take the token endpoint (`/auth/token` or `/admin/token`).
- Unit tests for admin sessions, authentication, service operations, config and log reading; integration test step 7 for the admin interface.
- Integration test [ldap_admin.sh](tests/integration/ldap_admin.sh) (`ctest -L ldap`): the admin interface with LDAP users and group changes, against the docker test directory; skipped unless `CONFIDE_LDAP_TEST_URI` is set.
- [dev_info.md](dev_info.md): how to add features: `ftc` commands and REST endpoints.
- Integration test step for the step-by-step commands.

## [0.7.0] — 2026-10-20

### Business logic

#### Added
- **Forwarding to a peer server (FR-10)**: a transaction committed for `user@otherServer` is pushed to that server in the background, then marked `delivered`. A failed push stays `committed` and is retried with backoff (5 s doubling up to 10 min) while the error may go away (peer down, overloaded, failing). It is also picked up again after a restart. Errors a retry can't fix (unknown user on the peer, wrong peer token) are logged and wait for the next start.
- A retried push continues where the earlier one stopped: files are sent again only if the transaction is still open on the peer, and only the confirmation is redone if the peer already committed it.
- Peer endpoint `GET /internal/transactions/{id}`, so a peer can see the state of a transaction it pushed.
- Optional `comment` on a transaction (`ftc send --comment`), at most 256 characters, carried along when forwarded.
- `rest.bind` accepts a host name as well as an IP address (`0.0.0.0`, `::`, `10.1.2.3`). The name is resolved once at startup.

#### Changed
- **Stricter peer tokens** (may refuse an existing config): `[[peer]]` `token` and `accept_token` must be at least 32 visible ASCII characters (e.g. `openssl rand -hex 32`), must not start with `ftu_`, and every secret may be used only once across all peers and directions. Before, a shared `accept_token` made two peers indistinguishable, and the last one in the file won.

#### Fixed
- `ftc get` failed on files larger than 8 MiB (`body limit exceeded`).
- The server refuses to start if the system binds a different port or address than configured (seen on WSL2 for ports Windows keeps for itself), instead of silently listening where no client or peer will look.

### Technical

#### Added
- `net` library: the HTTP client moved out of `ftc` so the forwarder can use it too. New: an idle timeout per connect, read and write, `cancel()` from another thread, uploads streamed from any source (not just a file), and up to two retries when connecting or the TLS handshake fails before anything was sent.
- `forwarder` library with a worker thread, wired into `confide-server` (started after the API, stopped before retention).
- `TransactionService::list_outgoing()` and `outgoing()` for the forwarder.
- Unit tests for the client (timeouts, cancel, failing upload source) and the forwarder (against a real in-process server: delivery, resume, lost confirmation, permanent and transient errors, restart, shutdown during a stalled push).
- End-to-end test `tests/integration/two_sites.sh` (in `ctest`, label `integration`): two servers with TLS driven by `ftc` and `curl`.
- Benchmark `tests/bench/bench.sh`: 1000 small files and a 2 GiB file, with throughput and a peak memory check against the NFR-4 limit of 100 MiB.
- systemd support in the DEB and RPM packages: a `confide-server.service` unit plus install and remove scripts (`postinst`/`prerm`/`postrm`, `pre`/`post`/`preun`/`postun`).
- RPM packages for EL9 alongside EL8 in the release workflow. On both EL8 and EL9, Boost comes from vcpkg.

#### Changed
- `HttpServer` is split into several source files.
- LDAP connection and authentication handling rewritten using modern C++ features.
- `FileStore` document handling and upload logic moved into separate files.
- Configuration parsing split by section: authentication, storage, REST, TLS, logging, retention and peers.
- `.clang-tidy`: configured the `bugprone-easily-swappable-parameters` check.

#### Fixed
- Unit tests that shared a fixed temp folder (config, business log, retention, transaction service) failed at random under `ctest -j`; each test now uses its own folder.

## [0.6.0] — 2026-10-14

### Business logic

#### Added
- **Token authentication**: auth token API endpoints and a server-side token session store.
- **Login throttling** against brute-force attempts.
- `ftc` CLI: token management and a local token cache, so the password isn't needed on every call.
- `ftc` CLI: secure password input (no echo).
- `ftc` CLI: client-side HTTP security enforcement (refuses insecure connections unless explicitly allowed).
- **Retention**: the requested retention days are checked against a configured maximum when a transaction is created.
- **Retention**: expired transactions are deleted automatically.

#### Documentation
- Configuration, README and Architecture updated for token authentication.
- Client documentation updated for the security changes; added a warning about server-side TLS.

### Technical

#### Added
- New `retention` library with the scheduler that removes expired transactions.
- `ftc` CLI: signal handling.
- **Packaging**: installation split into components; CPack builds a component-based `.deb`.
- RPM package workflow (EL8).
- Unit tests for token authentication, throttling and the retention scheduler.
- `implementation_details.md` document.

#### Changed
- Base64 utilities moved from `auth` to the `common` library, along with their tests.
- Boost is linked as static libraries.
- Release workflow publishes several artifacts.
- CI runner updated to `ubuntu-26.04`.
- `config-read` example updated.

## [0.1.0] — 2026-10-13

First tagged release. It also covers the early tags `1` (PR #2) and `2` (PR #3), which mark intermediate merges.

### Business logic

#### Added
- **Transactions**: transaction model with its lifecycle (state machine) and service rules for creating, receiving and committing transactions.
- **REST API**: transaction endpoints served over HTTP.
- **File storage**: files stored per transaction, with paths checked so uploads can't escape the transaction folder.
- **Authentication**
  - Local users with PBKDF2-HMAC-SHA256 password hashes, checked from the `Authorization` header.
  - `confide-passwd` utility to generate password hashes.
  - Users kept in a separate file from the main configuration.
  - User groups.
  - Two auth back-ends to choose from: local and LDAP.
- **Business log**: auditable events written as JSON.
- **Client**: `ftc` command-line client, with its own README.
- **Versioning**: `--version` flag, better usage text, and the version string logged at startup.

#### Documentation
- Initial requirements, Architecture document and README.

### Technical

#### Added
- **Core libraries**
  - Technical logging with console and file sinks.
  - Error handling (`Result`, `CFD_TRY`).
  - `TransactionId`: generating and parsing UUID v4 identifiers.
  - ISO 8601 time formatting and parsing.
  - SHA-256 hashing (OpenSSL).
  - Base64 encoding and decoding.
  - Configuration loading (TOML via toml11).
- Transaction JSON codec (Boost.JSON) and repository.
- HTTP server core, routing and JSON utilities.
- LDAP client library and example.
- Docker file with a test LDAP instance.
- `confide::version` CMake target.
- **Build and CI**
  - CMake presets for Linux builds.
  - GitHub CI workflow, driven by tags (`vX.Y.Z` → `APP_VERSION`).
  - vcpkg baseline for dependencies.
- Google Test integration with unit tests for common, auth, transaction and API modules.

[Unreleased]: https://github.com/Z1nK/ot-SecFileTransfer/compare/v0.6.0...HEAD
[0.6.0]: https://github.com/Z1nK/ot-SecFileTransfer/compare/v0.1.0...v0.6.0
[0.1.0]: https://github.com/Z1nK/ot-SecFileTransfer/releases/tag/v0.1.0
