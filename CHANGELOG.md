# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project uses [Semantic Versioning](https://semver.org/).

Each release is split into two parts:

- **Business logic**: what users and administrators see — features, rules, API and CLI behaviour, configuration options.
- **Technical**: internals — code structure, build, packaging, CI, tests, tooling.

## [Unreleased] — HEAD

Changes since `v0.6.0`, not tagged yet.

### Business logic

#### Added
- **Forwarding to a peer server (FR-10)**: a transaction committed for `user@otherServer` is pushed to that server in the background, then marked `delivered`. A failed push stays `committed` and is retried with backoff (5 s doubling up to 10 min) while the error may go away (peer down, overloaded, failing). It is also picked up again after a restart. Errors a retry can't fix (unknown user on the peer, wrong peer token) are logged and wait for the next start.
- A retried push continues where the earlier one stopped: files are sent again only if the transaction is still open on the peer, and only the confirmation is redone if the peer already committed it.
- Peer endpoint `GET /internal/transactions/{id}`, so a peer can see the state of a transaction it pushed.
- Optional `comment` on a transaction (`ftc send --comment`), at most 256 characters, carried along when forwarded.
- `rest.bind` accepts a host name as well as an IP address (`0.0.0.0`, `::`, `10.1.2.3`). The name is resolved once at startup.

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
