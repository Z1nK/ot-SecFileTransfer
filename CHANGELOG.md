# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project uses [Semantic Versioning](https://semver.org/).

Each release is split into two parts:

- **Business logic**: what users and administrators see — features, rules, API and CLI behaviour, configuration options.
- **Technical**: internals — code structure, build, packaging, CI, tests, tooling.

## [Unreleased]

### Business logic

#### Added
- **GNOME Files (Nautilus): "Get with ftc…"** on the background of a folder (and "Get with ftc into folder…" on a selected folder) opens a dialog (`ftc-get-dialog`, zenity) that lists the downloadable transactions of the inbox (committed and delivered), logs in if needed and runs `ftc get` for the chosen ones into that folder. Existing files are overwritten only after asking. Installed with the client package as `bin/ftc-get-dialog`.
- **Nautilus integration: `install.sh` / `uninstall.sh`** in `integrations/nautilus/` install and remove the menu items for the current user without the package (the nautilus-python extension, or Nautilus scripts when nautilus-python is missing) and warn about missing dependencies.
- **A sender can delete their own open transaction**: `DELETE /transactions/{id}` and `ftc delete ID` remove it with its files and log `discarded`. Only the sender, and only while it is `open`; once committed it answers 409 and stays until retention or an admin removes it. `ftc send` names the command when it leaves a transaction open after a failed upload.
- **`rest.max_upload_bytes`** sets the largest single file a client or peer may upload; a larger one is refused with 400 from its `Content-Length`, before any of it is read. Default `0` = no limit, as before.
- **`origin_server` in transaction JSON.** Users now see which server a received transaction came from (it was admin-only), and `ftc inbox` shows the sender as `alice@siteA` for those: `alice` alone could not be told from the alice of one's own server.

#### Changed
- **A peer's push is checked like local input.** The sender name must follow the rule for user names (1-64 chars of `[A-Za-z0-9._-]`), and an expiry beyond this server's `retention.max_days` is shortened to it.
- **Old failed logins are forgotten.** A user, address or pair with no failed login for an hour starts again at zero.

#### Fixed
- **LDAP: a recipient typed in another case never got the transaction.** `--to Alice` was accepted (LDAP ignores case) but stored as typed, while alice's inbox matches the directory's spelling exactly. The target is now stored as the directory spells it, for local sends and for pushes from a peer.
- **An LDAP outage could stall every request.** An inbox listing asked the directory about group membership once per transaction while holding the lock on all transactions, waiting `timeout_s` each time. It now asks once per target name, without the lock and off the network threads, and after an unanswered lookup the next ones fail at once for 10 s.
- **One address could end up throttled for good.** The per-address count of failed logins only ever grew (a successful login does not clear it), so an address shared by many users reached the limit and every further typo blocked all of them.
- **Requests for transaction ids that do not exist** each left a small lock object behind, so any user could make the server's memory grow.

### Technical

#### Changed
- `transaction::DestinationCheck` returns the target name to store; `auth::IUserDirectory` has `canonical_user()`; `is_valid_name` moved to `common/name.hpp` so `transaction` can use it without `config`.
- `TransactionService::lock_existing()` replaces `lock_for()` wherever the id comes from a request.

## [0.7.6] — 2026-11-10

### Business logic

#### Changed
- File I/O, password checks (LDAP) and transaction storage access run on a separate thread pool, so a slow disk, a long `fsync` or a slow directory server no longer stops the server from accepting and serving other requests. New option `rest.blocking_threads` (default `0` = 2 × CPU cores, at least 4).
- `ftc --user` defaults to the account `ftc` runs as (from the system user database, so LDAP accounts too; not from `$USER`). `--user` and `FTC_USER` still take precedence.

#### Added
- `ftc recent`: prints the last recipients of `ftc send` (newest first, at most 10), without contacting the server. The list is kept per server and user next to the token cache (`$XDG_STATE_HOME/ftc/recent/`, default `~/.local/state/ftc/recent/`; `%LOCALAPPDATA%\ftc\recent\` on Windows). A successful `send` adds its recipient; failing to update the list is only a warning.
- **GNOME Files (Nautilus) integration**: right-click → "Send with ftc…" on files and folders opens a dialog (`ftc-send-dialog`, zenity) that offers the recent recipients, logs in if needed and runs `ftc send`. Installed with the client package on Linux (`bin/ftc-send-dialog`, `share/nautilus-python/extensions/ftc_nautilus.py`); `zenity`, `libnotify` and `nautilus-python` are suggested, not required. See [integrations/README.md](integrations/README.md).
- **Windows client**: `ftc` builds and runs on Windows. Tokens are kept in `%LOCALAPPDATA%\ftc\tokens`, owned by the user with a protected DACL that grants only that user (the equivalent of `0700`/`0600` on POSIX); a folder or file open to other accounts is refused. Password input is hidden on the Windows console too. LDAP is not built on Windows.

### Technical

#### Changed
- `api::offload(pool, fn)` ([api/blocking.hpp](src/lib/confide/api/blocking.hpp)) runs a blocking call on the server's `blocking` pool and resumes the coroutine on its strand; handlers get the pool as `RequestContext::blocking`. Used for authentication, uploads and downloads (begin, chunks, finish), transaction create, remove-file, commit and receive, the admin handlers, and the transaction repository methods.
- Library layout: the server libraries moved under `src/lib/confide/`, and the client-side parts of `ftc` (`local_files`, `outgoing`, `reply`, `token_cache`) moved out of `src/app/cli/` into a new library `src/lib/client/` (namespace `confide::client`), so other front ends can reuse them.
- `TransactionService` split from one `service.cpp` into `service/{admin,jobs,peer,reading,sender}.cpp`, with shared helpers in `service/detail.hpp` and the data types in `service_types.hpp`.
- Platform-specific code isolated: `src/app/cli/platform.hpp` (terminal check, hidden input, current user) with `platform_posix.cpp` / `platform_win.cpp`; `TokenCache` split into `token_cache_posix.cpp` / `token_cache_win.cpp` with common parts in `token_cache_detail.hpp`. `HttpClient` handles file paths as UTF-8 on all platforms.

#### Added
- CMake presets `x64-debug-windows` / `x64-release-windows` (Ninja, MSVC, vcpkg triplet `x64-windows-static-md`, `WITH_LDAP=OFF`); `_WIN32_WINNT=0x0A00` defined for Asio.
- [docker/builders/build-rpm.sh](docker/builders/build-rpm.sh) and `Dockerfile.rpm`: build and install-test the RPMs locally (Rocky Linux 8/9) from the working tree, the same way the release workflow does. Packages go to `dist/elN/`.
- `client::RecentRecipients` with unit tests; `two_sites.sh` checks `ftc recent` after a send.

## [0.7.5] — 2026-10-27

### Business logic

#### Changed
- A local group named in `auth.admin_group` is refused as a transaction target (`unknown_destination`), so one send can't reach every admin.
- Peer tokens may not start with `fta_` either (the prefix of admin tokens).

#### Added
- **Admin interface**: `auth.admin_group` names the group whose members administer the server, for both local and LDAP users. Admins can list all transactions (metadata only, never file contents), show one with its files, delete one, change its retention, see server status and live sessions, end a user's sessions, and read business-log lines for a time range. REST under `/admin/...`, CLI as `ftc admin ...`. Without `admin_group` there is no admin API.
- Admin tokens (`fta_...`, from `POST /admin/token` / `ftc admin login`) are separate from user tokens and only valid on `/admin/...`; their lifetime is `auth.admin_token_ttl_s` / `auth.admin_token_idle_s` (default: the user token values). Group membership is checked on every admin request, so removal from the group takes effect at once.
- Business-log events `admin_deleted`, `retention_changed`, `sessions_revoked`, with the admin as `actor`.
- `ftc create`, `ftc add`, `ftc rm`, `ftc commit`: build a transaction step by step. `create` opens a transaction and prints its id, `add` uploads files and folders into it (same path rules as `send`), `rm` removes files by their path in the transaction, `commit` closes it. `ftc send` still does all steps at once.

### Technical

#### Changed
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

[Unreleased]: https://github.com/Z1nK/ot-SecFileTransfer/compare/v0.7.6...HEAD
[0.7.6]: https://github.com/Z1nK/ot-SecFileTransfer/compare/v0.7.5...v0.7.6
[0.7.5]: https://github.com/Z1nK/ot-SecFileTransfer/compare/v0.7.0...v0.7.5
[0.7.0]: https://github.com/Z1nK/ot-SecFileTransfer/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/Z1nK/ot-SecFileTransfer/compare/v0.1.0...v0.6.0
[0.1.0]: https://github.com/Z1nK/ot-SecFileTransfer/releases/tag/v0.1.0
