# Developer guide

How to extend the product: which files to touch, and what to add so that the
build, help texts, tests and docs stay consistent. For how the code works,
see [implementation_details.md](implementation_details.md); for the design,
[Architecture.md](Architecture.md).

Each section is a checklist followed by a worked example.

- [1. Add a command to the `ftc` client](#1-add-a-command-to-the-ftc-client)
- [2. Add a REST endpoint to the server](#2-add-a-rest-endpoint-to-the-server)

---

## 1. Add a command to the `ftc` client

### How a command runs

```
main.cpp        global options (--server, --user, --ca-file, --allow-http, env FTC_*)
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
  the token themselves (`login`, `logout`).
- **Don't trust server replies.** Read fields with `str()` / `num()` from
  [reply.hpp](src/app/cli/reply.hpp) (a missing field gives `""` / `0`), and
  pass server paths through `common::RelativePath::parse` before using
  them as local paths.
- **Build URLs from parsed values.** Use `tx_target(id)` (in
  [cli.hpp](src/app/cli/cli.hpp)) for `/transactions/{id}`, and `net::url_encode_path()` for file paths in a URL.
- **Keep output stable.** The integration tests and users' scripts read it:
  one item per line, IDs in the first column.
- **Helpers shared by several commands** go into the matching module next to
  `main.cpp` (`reply`, `local_files`, `session`, `outgoing`), not into a
  command file. See [Sharing code between commands](#sharing-code-between-commands).

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

**5. Build and try it:**

```sh
cmake --build build --target ftc
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

When two commands do the same steps, put the steps in a module next to
`main.cpp` and call it from both. Example:
[outgoing.cpp](src/app/cli/outgoing.cpp) has `create_transaction`,
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
([api/handlers.cpp](src/lib/api/handlers.cpp)):

```cpp
Def{
    .method = verb::delete_,
    .pattern = "/transactions/{id}/files/{path*}",  // {name}: one segment, {name*}: the rest
    .access = Access::user,                         // user | password | peer
    .fn = &TransactionHandlers::remove_file,
},
```

`Access` ([api/router.hpp](src/lib/api/router.hpp)): `user` takes Basic or a
user token, `password` takes Basic only, `peer` takes only a peer's
`accept_token` (use it for `/internal/...` routes).

### Checklist

| # | Where | What |
|---|-------|------|
| 1 | [transaction/service.hpp](src/lib/transaction/service.hpp) / [service.cpp](src/lib/transaction/service.cpp) | The service function, if no existing one does the work. Lock the transaction, check access (`may_write` / read rules), check the state, change it, record a business event. Return `forbidden`, `notfound`, `conflict` etc.; never an HTTP status. |
| 2 | [logging/business_log.hpp](src/lib/logging/business_log.hpp) | A new `BizEventType` (and its name in `business_log.cpp`), if the action is new and must be audited. |
| 3 | [api/json.hpp](src/lib/api/json.hpp) / [json.cpp](src/lib/api/json.cpp) | `decode_*` for a new request body, `encode_*` for a new response shape, with the JSON written out in the comment. Reuse `encode_transaction` / `encode_files` / `encode_file` when they fit. |
| 4 | [api/handlers.hpp](src/lib/api/handlers.hpp) | Declare the handler with a comment `// METHOD /pattern -> status + body`, in the Users or Peers block. |
| 5 | [api/handlers.cpp](src/lib/api/handlers.cpp) | The handler, and its `Def` in `register_routes()`. If the endpoint only reuses an existing handler (like `GET /internal/transactions/{id}` reuses `get`), the `Def` is all you add. |
| 6 | [tests/unit/transaction/service_test.cpp](tests/unit/transaction/service_test.cpp) | Tests of the service rules: who is allowed, which states are refused. |
| 7 | [tests/unit/api/json_test.cpp](tests/unit/api/json_test.cpp) | Tests for new `decode_*` / `encode_*` functions, including bad input. |
| 8 | [tests/integration/two_sites.sh](tests/integration/two_sites.sh) | A step that calls the endpoint over HTTPS (through `ftc`, or `curl` with the saved token, as step 3 does) and checks status and result. There are no unit tests for `TransactionHandlers` themselves; this is where the HTTP mapping is tested. |
| 9 | [README.md](README.md) §4 | A row in the "Use the API" table (users' endpoints; `/internal` is covered by its one row). |
| 10 | [Architecture.md](Architecture.md) §8 | A row in the REST API table. |
| 11 | [implementation_details.md](implementation_details.md) §3, §5 | A row in the route table, and a call tree under §5 if the handler does more than parse + one service call + send. |
| 12 | [CHANGELOG.md](CHANGELOG.md) | An entry under `[Unreleased]` → Business logic → Added. |

A new group of endpoints that has nothing to do with transactions (like
`/auth/token`) gets its own handler class with its own `register_routes()`,
as [api/auth_handlers.cpp](src/lib/api/auth_handlers.cpp) does, called from
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

**1. Service** ([transaction/service.cpp](src/lib/transaction/service.cpp)).
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

**2. Handler declaration** ([api/handlers.hpp](src/lib/api/handlers.hpp)):

```cpp
  // DELETE /transactions/{id}/files/{path*} -> 204
  asio::awaitable<void> remove_file(RequestContext& ctx);
```

**3. Handler** ([api/handlers.cpp](src/lib/api/handlers.cpp)). Parse, call,
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
