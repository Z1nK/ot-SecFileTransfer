# Developer guide

How to extend the product: which files to touch, and what to add so that the
build, help texts, tests and docs stay consistent. For how the code works,
see [implementation_details.md](implementation_details.md); for the design,
[Architecture.md](Architecture.md).

Each section is a checklist followed by a worked example.

- [1. Add a command to the `ftc` client](#1-add-a-command-to-the-ftc-client)
- [2. Add a REST endpoint to the server](#2-add-a-rest-endpoint-to-the-server)
- [3. Add a client setting (config file / ENV / option)](#3-add-a-client-setting-config-file--env--option)

---

## 1. Add a command to the `ftc` client

### How a command runs

```
main.cpp        global options -> cli::Global
   │              config files < env FTC_* < --server/--user/--ca-file (see §3)
   │              a broken config file           -> "ftc: <file>: ...", exit 2
   │
dispatch.cpp    run_command(name, args, global)
   │              find(name) in kAll           -> unknown name: usage, exit 2
   │              cmd->options(desc, hidden, pos)
   │              --help / parse args          -> parse error: usage, exit 2
   │              cmd->run(global, vm)         -> error: "ftc NAME: [code] detail", exit 1
   │
commands/*.cpp  run_<name>(global, vm)
                  connect(global)              session.cpp: token, login, re-login
                  client.call(verb, target)    net::HttpClient
                  parse_object / parse_list    reply.cpp
```

A command is a `Command` value
([commands/commands.hpp](src/app/cli/commands/commands.hpp)):

```cpp
struct Command {
  std::string_view name;      // "ftc NAME"
  std::string_view synopsis;  // after "ftc NAME" in the usage line, starts with a space
  void (*options)(po::options_description& desc,     // shown in --help
                  po::options_description& hidden,   // positional arguments
                  po::positional_options_description& pos) = nullptr;
  Result<void> (*run)(const Global& global, const po::variables_map& vm) = nullptr;
};
```

`dispatch.cpp` handles `--help`, parsing, error printing and exit codes.
The command only declares its options and does the work.

### Checklist

| # | Where | What |
|---|-------|------|
| 1 | `src/app/cli/commands/<group>.cpp` | `options` function (if the command takes arguments), `run_<name>` function, and the `const Command k<Name>{...}` definition. Use an existing file if the command fits its group (`query.cpp` for read-only views, `auth.cpp` for the session); otherwise make a new file. |
| 2 | [commands/commands.hpp](src/app/cli/commands/commands.hpp) | `extern const Command k<Name>;` under the comment naming its file. |
| 3 | [dispatch.cpp](src/app/cli/dispatch.cpp) | Add `&k<Name>` to `kAll`, and a line to the `kCommands` help text. Both are needed: `kAll` makes the command run, `kCommands` makes it show up in `ftc --help`. |
| 4 | [src/app/cli/CMakeLists.txt](src/app/cli/CMakeLists.txt) | Only for a new `.cpp` file: add it to `target_sources`. |
| 5 | [tests/integration/two_sites.sh](tests/integration/two_sites.sh) | A step that runs the command through `ftc_as USER SITE ...` and checks the result. |
| 6 | [README.md](README.md) | The "Same flow with ftc" section, if users need to know about the command. |
| 7 | [CHANGELOG.md](CHANGELOG.md) | An entry under `[Unreleased]` → Business logic → Added. |
| 8 | [implementation_details.md](implementation_details.md) §10 | The call tree, if the command does something new (not just one `client.call`). |

An admin command (`ftc admin NAME`) goes into
[commands/admin.cpp](src/app/cli/commands/admin.cpp) instead: add it to
`kAdminAll` and to the `kAdminCommands` help text there (steps 2 and 3 don't
apply), and connect with `connect(global, Scope::admin)`. Don't name an
option `--user`, `--server` or `--ca-file`: the global options take them
wherever they appear on the command line.

If the command needs a server endpoint that does not exist yet, add that
first: see [2. Add a REST endpoint to the server](#2-add-a-rest-endpoint-to-the-server).

### Rules for commands

- **Return errors, don't print them.** Return `fail(ErrCode::..., "detail")`
  or pass errors on with `CFD_TRY` / `CFD_TRYV`. `dispatch.cpp` prints
  `ftc NAME: [code] detail` and exits with 1. Print to `std::cerr` yourself
  only for warnings, or for extra context on a failure (like `send` saying
  which transaction was left open).
- **Check local input before `connect()`.** `connect()` may ask for a
  password, so parse IDs, check paths etc. first. That way a typo fails at
  once instead of after a login prompt.
- **Use `connect(global)`** for anything that needs a logged-in client. It
  loads the saved token, logs in if there is none, and logs in again when
  the token expires. Use `open_session(global)` only for commands that manage
  the token themselves (`login`, `logout`). A command that needs no server
  (`recent`, `config`) calls neither, so it works offline and never asks for
  a password.
- **Use `Global` as it is.** `main.cpp` has already merged the config files,
  the environment and the command line into `global.server`, `global.user`
  and `global.ca_file`. Don't read `FTC_*` variables or config files in a
  command. If a command needs a new shared setting, add it as in §3.
- **Don't trust server replies.** Read fields with `str()` / `num()` from
  [reply.hpp](src/lib/client/reply.hpp) (a missing field gives `""` / `0`), and
  pass server paths through `common::RelativePath::parse` before using
  them as local paths.
- **Build URLs from parsed values.** Use `tx_target(id)` (in
  [client/client.hpp](src/lib/client/client.hpp)) for `/transactions/{id}`, and `net::url_encode_path()` for file paths in a URL.
- **Keep output stable.** The integration tests and users' scripts read it:
  one item per line, IDs in the first column.
- **Helpers shared by several commands** go into a module, not into a
  command file: protocol logic with no terminal I/O into the client library
  [src/lib/client/](src/lib/client/) (`reply`, `local_files`, `outgoing`,
  `token_cache`), so other front ends (GUI, ...) can reuse it; terminal glue
  next to `main.cpp` (`session`, `password`). See [Sharing code between commands](#sharing-code-between-commands).

### Worked example: how `ftc rm ID PATH...` was added

The server could already remove a file from an open transaction
(`DELETE /transactions/{id}/files/{path*}`); `rm` exposes it. It lives with
`create` / `add` / `commit` in
[commands/compose.cpp](src/app/cli/commands/compose.cpp), so these are the
pieces as they are in the code.

**1. Options and run function** in `commands/compose.cpp`. `rm` and `add`
take the same arguments, so they share one options function:

```cpp
// ID first, then one or more paths.
void id_and_paths(po::options_description& /*desc*/, po::options_description& hidden,
                  po::positional_options_description& pos) {
  hidden.add_options()("id", po::value<std::string>()->required())(
      "path", po::value<std::vector<std::string>>()->required());
  pos.add("id", 1).add("path", -1);
}

Result<void> run_rm(const Global& global, const po::variables_map& vm) {
  // Paths as the transaction has them (see `ftc files ID`), not local ones.
  CFD_TRY(id, common::TransactionId::parse(vm["id"].as<std::string>()));
  std::vector<common::RelativePath> paths;
  for (const auto& p : vm["path"].as<std::vector<std::string>>()) {
    CFD_TRY(rel, common::RelativePath::parse(p));
    paths.push_back(std::move(rel));
  }

  CFD_TRY(client, connect(global));     // only after the local checks
  for (const auto& rel : paths) {
    CFD_TRYV(client.call(http_verb::delete_,
                         tx_target(id) + "/files/" + net::url_encode_path(rel.str())));
    std::cout << "  " << rel.str() << '\n';
  }
  std::cout << "removed " << paths.size() << " files from " << id.str() << '\n';
  return {};
}
```

and, at the end of the file outside the anonymous namespace:

```cpp
const Command kRm{.name = "rm", .synopsis = " ID PATH...", .options = id_and_paths, .run = run_rm};
```

**2. [commands/commands.hpp](src/app/cli/commands/commands.hpp):**

```cpp
// commands/compose.cpp
extern const Command kCreate;
extern const Command kAdd;
extern const Command kRm;
extern const Command kCommit;
```

**3. [dispatch.cpp](src/app/cli/dispatch.cpp):** `&kRm` in `kAll`, and the
help text in `kCommands`, keeping the column alignment:

```
  rm ID PATH...         remove files from an open transaction, by their
                        path in the transaction (see 'ftc files ID')
```

**4. [src/app/cli/CMakeLists.txt](src/app/cli/CMakeLists.txt):**
`commands/compose.cpp` in `target_sources` (once for the whole file).

**5. Build and try it.** Run with `FTC_CONFIG=` so your own
`~/.config/ftc/ftc.toml` or `/etc/ftc/ftc.toml` doesn't change the result:

```sh
cmake --build build --target ftc
export FTC_CONFIG=               # no config file while trying things out
build/bin/ftc rm --help          # usage line comes from .synopsis
build/bin/ftc rm                 # "the option '--id' is required but missing", exit 2
```

**6. Test:** step 6 of
[tests/integration/two_sites.sh](tests/integration/two_sites.sh) runs
`create`, `add`, `rm`, `commit` and checks the result. It also checks the
errors: `rm` of a missing file gives `notfound`, `add` after commit gives
`conflict`. Run:

```sh
ctest --test-dir build -R integration --output-on-failure
```

**7. Docs:** CHANGELOG entry under `[Unreleased]`, README "Same flow with
ftc".

### Sharing code between commands

When two commands do the same steps, put the steps in a module and call it
from both (in [src/lib/client/](src/lib/client/) unless it prints or prompts:
pass a callback instead, like `upload_files`' `on_uploaded`). Example:
[outgoing.cpp](src/lib/client/outgoing.cpp) has `create_transaction`,
`upload_files` and `commit_transaction`. `send` calls all three; `create`,
`add` and `commit` call one each. Options shared by several commands go
into a function declared in `commands.hpp` (`new_transaction_options` /
`new_transaction_from` for `--to`, `--retention`, `--comment`).

### Options with values

For options the user sees, add them to `desc`. Use the long name in
`vm[...]`; give short names only to frequently used options:

```cpp
desc.add_options()
    ("force,f", "overwrite existing files")                       // flag: vm.contains("force")
    ("retention,r", po::value<int>()->value_name("DAYS"),          // optional: check vm.contains
     "days to keep the transaction")
    ("to,t", po::value<std::string>()->required()->value_name("USER"), "receiver");
```

Positional arguments go into `hidden` and `pos`. Use `default_value` for
optional ones (see `get_options` in
[commands/get.cpp](src/app/cli/commands/get.cpp): `DIR` defaults to `.`).
Options can't be guessed from a prefix (`allow_guessing` is off), so
`--forc` is an error and not `--force`.

---

## 2. Add a REST endpoint to the server

### How a request reaches a handler

```
api/server.cpp     HttpServer: one coroutine per connection
   │                 router.match(method, target)   -> 404 / 405 / 400 before any handler
   │                 check route.access             -> 401 if the credentials don't fit
   │                 route.handler(ctx)             -> 500 if it throws or sends nothing
   │
api/handlers.cpp   TransactionHandlers::<fn>(ctx)
   │                 id_param / path_param          parse {id}, {path*}       -> 400
   │                 read_body_string + decode_*    JSON request body         -> 400
   │                 service_.<call>(ctx.caller, ...)
   │                 ctx.exchange.send(Response::json(status, encode_*(...)))
   │                 on error: co_return co_await fail(ctx, err)
   │
transaction/service.cpp   TransactionService: access rules, state, business log
```

The layers have separate jobs (see
[implementation_details.md](implementation_details.md) §3–§5):

- **Router / server**: path matching, authentication by `Access`, 404/405/401.
- **Handler**: HTTP only. Parse parameters and body, call one service
  function, encode the answer. No business rules.
- **Service**: who may do what, in which state, and the business log entry.
  Knows nothing about HTTP.

A route is one `Def` in `TransactionHandlers::register_routes()`
([api/handlers.cpp](src/lib/confide/api/handlers.cpp)):

```cpp
Def{
    .method = verb::delete_,
    .pattern = "/transactions/{id}/files/{path*}",  // {name}: one segment, {name*}: the rest
    .access = Access::user,                         // user | password | peer
    .fn = &TransactionHandlers::remove_file,
},
```

`Access` ([api/router.hpp](src/lib/confide/api/router.hpp)): `user` takes Basic or a
user token, `password` takes Basic only, `peer` takes only a peer's
`accept_token` (use it for `/internal/...` routes).

### Checklist

| # | Where | What |
|---|-------|------|
| 1 | [transaction/service.hpp](src/lib/confide/transaction/service.hpp) / [service.cpp](src/lib/confide/transaction/service.cpp) | The service function, if no existing one does the work. Lock the transaction, check access (`may_write` / read rules), check the state, change it, record a business event. Return `forbidden`, `notfound`, `conflict` etc.; never an HTTP status. |
| 2 | [logging/business_log.hpp](src/lib/confide/logging/business_log.hpp) | A new `BizEventType` (and its name in `business_log.cpp`), if the action is new and must be audited. |
| 3 | [api/json.hpp](src/lib/confide/api/json.hpp) / [json.cpp](src/lib/confide/api/json.cpp) | `decode_*` for a new request body, `encode_*` for a new response shape, with the JSON written out in the comment. Reuse `encode_transaction` / `encode_files` / `encode_file` when they fit. |
| 4 | [api/handlers.hpp](src/lib/confide/api/handlers.hpp) | Declare the handler with a comment `// METHOD /pattern -> status + body`, in the Users or Peers block. |
| 5 | [api/handlers.cpp](src/lib/confide/api/handlers.cpp) | The handler, and its `Def` in `register_routes()`. If the endpoint only reuses an existing handler (like `GET /internal/transactions/{id}` reuses `get`), the `Def` is all you add. |
| 6 | [tests/unit/transaction/service_test.cpp](tests/unit/transaction/service_test.cpp) | Tests of the service rules: who is allowed, which states are refused. |
| 7 | [tests/unit/api/json_test.cpp](tests/unit/api/json_test.cpp) | Tests for new `decode_*` / `encode_*` functions, including bad input. |
| 8 | [tests/integration/two_sites.sh](tests/integration/two_sites.sh) | A step that calls the endpoint over HTTPS (through `ftc`, or `curl` with the saved token, as step 3 does) and checks status and result. There are no unit tests for `TransactionHandlers` themselves; this is where the HTTP mapping is tested. |
| 9 | [README.md](README.md) §4 | A row in the "Use the API" table (users' endpoints; `/internal` is covered by its one row). |
| 10 | [Architecture.md](Architecture.md) §8 | A row in the REST API table. |
| 11 | [implementation_details.md](implementation_details.md) §3, §5 | A row in the route table, and a call tree under §5 if the handler does more than parse + one service call + send. |
| 12 | [CHANGELOG.md](CHANGELOG.md) | An entry under `[Unreleased]` → Business logic → Added. |

A new group of endpoints that has nothing to do with transactions (like
`/auth/token`) gets its own handler class with its own `register_routes()`,
as [api/auth_handlers.cpp](src/lib/confide/api/auth_handlers.cpp) does, called from
[src/app/main/main.cpp](src/app/main/main.cpp) next to the others.

### Rules for handlers

- **Every error goes through `fail()`.** Write
  `co_return co_await fail(ctx, err);`. `fail()` turns the `ErrCode` into
  the HTTP status (`status_for()`, table in
  [implementation_details.md](implementation_details.md) §1), answers
  `forbidden` as **404** so outsiders can't probe ids, and logs 5xx to the
  technical log.
- **Send exactly one response.** A handler that returns without sending
  gets a 500 from the server.
- **Parse parameters with the helpers.** `id_param(ctx)` and
  `path_param(ctx)` give `validation` (400) on bad input. Path parameters
  are already URL-decoded; never use them as paths before
  `RelativePath::parse`.
- **Limit what you read.** JSON bodies go through
  `read_body_string(options_.max_json_bytes)`. File bodies are streamed in
  `storage::kChunkBytes` chunks (see `upload`), never read whole.
- **No business rules in the handler.** If the check depends on who the
  caller is or what state the transaction is in, it goes into the service,
  so the forwarder and the tests get the same rules.
- **Pick the status from what happened:** `201` + body when something was
  created, `200` + body for reads and state changes, `204`
  (`Response::no_content()`) when there is nothing to return.

### Worked example: `DELETE /transactions/{id}/files/{path*}`

Removes a file from an open transaction. These are the pieces as they are
in the code.

**1. Service** ([transaction/service.cpp](src/lib/confide/transaction/service.cpp)).
All rules live here:

```cpp
Result<void> TransactionService::remove_file(const Caller& caller, const common::TransactionId& id,
                                             const common::RelativePath& path) {
  const auto mu = lock_for(id);
  const std::scoped_lock lock(*mu);

  auto tx = repo_.find(id);
  if (!tx) {
    return not_found(id);
  }
  if (!may_write(caller, *tx)) {
    return forbidden(id);                      // handler answers 404
  }
  CFD_TRYV(require_mutable(tx->state));        // committed -> conflict (409)

  const auto it = std::ranges::find(tx->files, path, &FileEntry::path);
  if (it == tx->files.end()) {
    return fail(ErrCode::notfound, "no such file: " + path.str());
  }
  CFD_TRYV(store_.remove_file(id, path));
  tx->files.erase(it);
  CFD_TRYV(repo_.save(*tx));

  auto ev = event_for(BizEventType::file_removed, *tx, caller);
  ev.file = path.str();
  blog_.record(ev);
  return {};
}
```

**2. Handler declaration** ([api/handlers.hpp](src/lib/confide/api/handlers.hpp)):

```cpp
  // DELETE /transactions/{id}/files/{path*} -> 204
  asio::awaitable<void> remove_file(RequestContext& ctx);
```

**3. Handler** ([api/handlers.cpp](src/lib/confide/api/handlers.cpp)). Parse, call,
answer:

```cpp
asio::awaitable<void> TransactionHandlers::remove_file(RequestContext& ctx) {
  auto id = id_param(ctx);
  if (!id) {
    co_return co_await fail(ctx, id.error());
  }
  auto path = path_param(ctx);
  if (!path) {
    co_return co_await fail(ctx, path.error());
  }
  auto removed = service_.remove_file(ctx.caller, *id, *path);
  if (!removed) {
    co_return co_await fail(ctx, removed.error());
  }
  co_await ctx.exchange.send(Response::no_content());
}
```

**4. Route** in `register_routes()`, next to the other
`/transactions/{id}/files/{path*}` routes (the `Def` shown above). Same
pattern with a different method is fine; the router answers 405 for a
method that has no route.

**5. Build and try it:**

```sh
cmake --build build
U=http://127.0.0.1:8080
curl -u alice:alice-pw -X DELETE -w '%{http_code}\n' $U/transactions/$ID/files/report/a.txt
# 204; again -> 404 {"error":"notfound",...}; after commit -> 409
```

**6. Tests:** `GroupTargetTest.MembersCannotWrite` in
[service_test.cpp](tests/unit/transaction/service_test.cpp) checks that only
the sender may remove; step 6 of
[two_sites.sh](tests/integration/two_sites.sh) removes a file with
`ftc rm`, checks that removing it again gives `notfound` and that the file
is not delivered. Run:

```sh
ctest --test-dir build --output-on-failure
```

**7. Docs:** a row in README §4, Architecture §8 and implementation_details
§3; the call tree in implementation_details §5.3; CHANGELOG entry.

---

## 3. Add a client setting (config file / ENV / option)

### How settings are merged

`ftc` takes each global setting from the first layer that has it (see
[implementation_details.md](implementation_details.md) §10.1):

```
command line  -s / -u / --ca-file                       highest
environment   FTC_SERVER / FTC_USER / FTC_CA_FILE
user file     ~/.config/ftc/ftc.toml   ($XDG_CONFIG_HOME)
system file   /etc/ftc/ftc.toml        (FTC_SYSTEM_CONFIG, installed by confide-client)
default       none / OS user / system CAs                  lowest
```

Each layer is a `client::Settings`
([settings.hpp](src/lib/client/settings.hpp)): one `std::optional` per key
(nullopt = this layer doesn't set it) plus `from`, where each value came from.
`overlay(low, high)` replaces whatever `high` sets. An empty value is set,
and after merging it means "the default", so a user can cancel what the
system file sets.

### First decide where the setting may come from

- **Secrets never go in a file.** Passwords and tokens: environment
  (development only) or prompt, as `FTC_PASSWORD`. If someone puts one in a
  file, `parse_settings()` warns and ignores it.
- **Settings that make ftc less secure** go on the command line only, as
  `--allow-http`, so a shared `/etc/ftc/ftc.toml` can't turn them on for
  every user without anyone noticing. They are ignored with a warning in a file.
- **Everything else** (where to connect, as whom, which CA, timeouts, ...)
  goes in all three layers.

### Checklist (a setting in all three layers)

| # | Where | What |
|---|-------|------|
| 1 | [client/settings.hpp](src/lib/client/settings.hpp) | A field in `Settings` and in `Settings::Origins`; the key in the file comment at the top. |
| 2 | [client/settings.cpp](src/lib/client/settings.cpp) | Read it in `parse_settings()` (with a type check that names the file), skip it in the unknown-key loop, and copy it in `overlay()`. A path is resolved against `base_dir`, like `ca_file`. |
| 3 | [main.cpp](src/app/cli/main.cpp) | The option in `global.add_options()` (help text naming the env variable), the env name in `env_option()`, the CLI/env names in `kFromCli` / `kFromEnv`, the field in `layer()`, and its default when building `cli::Global`. |
| 4 | [cli.hpp](src/app/cli/cli.hpp) | The field in `cli::Global`. |
| 5 | [commands/config.cpp](src/app/cli/commands/config.cpp) | The key in `kKeys` and `entry()`, so `ftc config` shows it with its origin. |
| 6 | [config/ftc.example.toml](config/ftc.example.toml) | The key, commented out, with its default. This file is installed as `/etc/ftc/ftc.toml`. |
| 7 | [tests/unit/client/settings_test.cpp](tests/unit/client/settings_test.cpp) | Parsing (good value, wrong type) and `overlay()`. |
| 8 | [tests/integration/two_sites.sh](tests/integration/two_sites.sh) step 8 | Only if it changes what ftc does on the network: from the file alone, and overridden by env and option. |
| 9 | [README.md](README.md) "Client configuration" | A row in the priority table. |
| 10 | [implementation_details.md](implementation_details.md) §10.1 | The default, if it is not obvious. |
| 11 | [CHANGELOG.md](CHANGELOG.md) | An entry under `[Unreleased]` → Business logic → Added. |

Options that belong to one command (`--retention`, `--force`) are not global
settings: they stay in that command's `options` function (§1) and don't go
in the file.

### Keeping tests and scripts away from the machine's config

Any script that runs ftc (tests, benchmarks, desktop dialogs that must behave
the same everywhere) should decide which config ftc reads:

- `FTC_CONFIG=` reads no config file at all. `ftc_as` in
  [tests/integration/lib.sh](tests/integration/lib.sh), `ftc_ldap` and
  [tests/bench/bench.sh](tests/bench/bench.sh) do this; a new wrapper must too,
  or a developer's `/etc/ftc/ftc.toml` (or a broken one) breaks the test.
- `FTC_CONFIG=path` reads only that file (it must exist).
- `XDG_STATE_HOME=$WORK/ftc-state` keeps tokens and recent recipients out of
  your home folder, as the wrappers already do.
- A script that needs to know the server or user asks
  `ftc config --get server` (as the Nautilus dialogs do) instead of reading
  `FTC_SERVER` itself, which would miss the config files.

### Packaging

`/etc/ftc/ftc.toml` belongs to the `client` CPack component
([CMakeLists.txt](CMakeLists.txt)). It is a conffile in the `.deb`
([packaging/deb/client/conffiles](packaging/deb/client/conffiles)) and
`%config(noreplace)` 0644 in the `.rpm` (`CPACK_RPM_CLIENT_USER_FILELIST`).
That is why an admin's edits survive upgrades. The release workflow checks
the mode and the config mark on EL8/EL9. To move it elsewhere, set
`-DFTC_CONFIG_DIR=...`; `FTC_SYSTEM_CONFIG` (compiled into ftc) follows. Keep
the client file out of `/etc/confide`: that folder is `0750 root:confide`,
so ordinary users can't read it.

Check a local package build:

```sh
cd build && cpack -G DEB
dpkg-deb -c confide-client_*.deb | grep ftc.toml         # -rw-r--r-- ./etc/ftc/ftc.toml
dpkg-deb --ctrl-tarfile confide-client_*.deb | tar -xO ./conffiles
```
