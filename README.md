# Network Failure & Recovery Simulator

A local-first network operations simulator for exploring how router failures
affect links and end-to-end reachability. The backend is written in C and serves
both the dashboard and its JSON REST API. The frontend uses plain HTML, CSS,
and vanilla JavaScript. It starts with six routers and lets you choose between
two and twelve. It simulates a network; it does not monitor physical devices or
networks.

## Requirements

- A C compiler such as GCC or Clang
- GNU Make
- `curl` to run the API smoke test

## Build and run

From this directory:

```sh
make
make run
```

The server listens on `http://127.0.0.1:8080` by default. To choose another
port or listen address:

```sh
PORT=9000 make run
HOST=0.0.0.0 PORT=9000 ./network-simulator
```

Binding to `0.0.0.0` is useful for a hosted preview; local runs bind to
`127.0.0.1` by default. Stop the server with Ctrl+C.

## Run the functional API tests

```sh
make test
```

The test script starts the C server on port `18081`, exercises the REST API,
checks metrics and BFS reachability, and shuts the server down. Set
`TEST_PORT` to use a different available port.

## REST API

All routes are served by the C process on the same origin as the dashboard.

| Method | Route | Purpose |
| --- | --- | --- |
| `GET` | `/sim-api/v1/state` | Read topology, routers, links, metrics, BFS reachability, and recent events |
| `POST` | `/sim-api/v1/topology` | Change topology with `{"topology":"star"}`, `"ring"`, or `"mesh"` |
| `POST` | `/sim-api/v1/router-count` | Choose the number of routers with `{"count":8}` (2–12; default 6) |
| `POST` | `/sim-api/v1/routers/{id}/fail` | Fail one router |
| `POST` | `/sim-api/v1/routers/{id}/recover` | Recover one router |
| `POST` | `/sim-api/v1/routers/fail-all` | Fail all routers |
| `POST` | `/sim-api/v1/routers/recover-all` | Recover all routers |
| `POST` | `/sim-api/v1/reset` | Restore the initial star topology and recover all routers |

The dashboard refreshes from `GET /sim-api/v1/state` every three seconds. Router
and topology state is held in memory by the local server and returns to its
initial state after a server restart. Changing the router count adds healthy
routers or removes routers from the end of the list; existing routers keep
their current status.

## Project files

- `server.c` — small POSIX-socket HTTP server, API routing, and static asset delivery
- `network.c`, `network.h` — topology model, state transitions, metrics, BFS, and JSON serialization
- `index.html`, `styles.css`, `app.js`, `favicon.svg` — dashboard markup, styling, browser behavior, and icon
- `Makefile` — build, run, test, and clean targets
- `tests/test_api.sh` — functional HTTP API smoke test

## License

MIT. See `LICENSE`.
