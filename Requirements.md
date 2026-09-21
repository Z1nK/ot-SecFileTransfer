# Requirements: Secure Transaction-Based File Transfer Service (C++)

## 1. Goal

A C++23 server that transfers files between users as **immutable transactions**. A sender creates a transaction, adds files, and commits it. After that the transaction can no longer change and the target user can download it. A committed transaction can also be forwarded to a second server instance.

Priorities: **M** = must, **S** = should (if time allows), **C** = could (stretch).

## 2. Functional Requirements

| ID | Pri | Requirement |
|----|-----|-------------|
| FR-1 | M | REST API to create a transaction with a target user (and a target server name for forwarding). |
| FR-2 | M | While a transaction is **open**, the sender can add files (with folder paths) and delete files. |
| FR-3 | M | **Commit** makes the transaction immutable: any later add/delete is rejected. Transaction states: `open → committed → delivered`. |
| FR-4 | M | The target user can list their incoming transactions and download files from committed ones. |
| FR-5 | M | Authentication with local users from a config file (login + token or Basic auth). A user sees only their own transactions. |
| FR-6 | M | Each file has a SHA-256 checksum; it is verified on upload and on commit. |
| FR-7 | M | Large files (10 GB+) are uploaded and downloaded as a stream in chunks, without loading the whole file into memory. |
| FR-8 | M | Two separate logs: a **business log** (who sent what to whom, when, file names and sizes, JSON lines) and a **technical log** (levels: debug/info/warn/error). |
| FR-9 | M | Per-transaction retention period; a background job deletes expired transactions. |
| FR-10 | S | **One-hop forwarding:** a committed transaction is pushed to another server instance found by name in a static config (`server name → host:port`). The receiving server stores it for the target user. |
| FR-11 | S | Optional TLS for the API and transfer, switched on or off in the config (on by default). |
| FR-12 | S | Read-only archive: committed transactions stay readable by authorised users until retention ends. |
| FR-13 | S | If the target user or server is unknown, the API returns a clear error ("destination unknown"). |
| FR-14 | S | Command-line client `ftc` (C++, same code base): `send <folder or file>` creates a transaction, uploads all files with their relative paths and commits; `inbox` lists incoming transactions; `get` downloads a transaction keeping its folder structure. `curl` still works for the raw API. |
| FR-15 | C | Resume of an interrupted upload. |

## 3. Non-Functional Requirements

| ID | Requirement |
|----|-------------|
| NFR-1 | **Stack:** C++23 (`-std=c++23`, GCC 15.2 default is gnu++20 and supports C++23), CMake, standard library plus Boost (Asio/Beast for HTTP). Any other library must be free and MIT-like. No Python. |
| NFR-2 | **Platform:** server runs on Ubuntu 26.04 with g++ 15.2.0 (also WSL). Avoid Linux-only APIs where possible (use `std::filesystem`, Asio) so that the `ftc` client can later be built for Windows and macOS. |
| NFR-3 | **Concurrency:** handles at least 10 simultaneous clients with a thread pool; no data races. |
| NFR-4 | **Memory:** constant memory usage for large files (target: under ~100 MB RSS while transferring a 10 GB file). |
| NFR-5 | **Performance:** small-file batches (e.g. 1000 files of a few KB) and one large file both complete without errors; a simple benchmark for both cases is included. |
| NFR-6 | **Integrity:** an interrupted or corrupted upload never becomes a committed transaction; stored files always match their checksums. |
| NFR-7 | **Security:** passwords are stored hashed; users cannot access other users' transactions; path traversal (`../`) is blocked. |
| NFR-8 | **Configuration:** one config file (server name, port, storage path, users, TLS on/off, retention default, peers). |
| NFR-9 | **Testing:** unit tests for the transaction state machine and checksum; integration test that runs the full flow (create, upload, commit, download) on one or two local instances. |
| NFR-10 | **Code quality:** RAII, no raw `new`/`delete`, modules separated (API, transaction store, storage, auth, logging, network client, forwarder, CLI client). |
| NFR-11 | **Documentation:** README with build steps, config example, and API examples (`curl`). |

## 4. Usage Scenario

The service never reads a user's disk by itself: the client program uploads files over HTTP, and the receiver downloads them the same way. Example: alice (siteA) sends the folder `~/data/report` to bob (siteB).

```bash
# alice, on her machine
ftc send --to bob@siteB ~/data/report        # create + upload all files + commit

# bob, on his machine
ftc inbox                                     # list incoming transactions
ftc get tx-123 ~/downloads                    # download, folder structure is kept
```

The same flow with plain `curl` (see `Architecture.md`, section 2) is used for testing the API.

## 5. Out of Scope (from the original idea)

**future work**.
LDAP integration, multi-hop routing and discovery, edge instances, custom TCP/UDP transfer protocol, antivirus scanning, Python hooks and integrations, user-cache fallback, cloud or mixed deployment, adaptive network tuning, GUI.

## 6. Definition of Done

1. Server builds with `cmake && make` and starts from a config file.
2. The full flow works via REST and via `ftc`: create transaction → add files → commit → target user downloads.
3. A committed transaction rejects any change.
4. A 2 GB file transfers with stable memory usage and a correct checksum.
5. Two logs are produced; expired transactions are cleaned up.
6. Tests pass; README is written.
