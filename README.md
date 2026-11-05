# Campus Ride Matching Backend

A C11 HTTP server for the campus carpool project. It stores accounts, rides, memberships, invitations, reports and notifications in SQLite. The frontend communicates with it through JSON requests.

## Run locally

Use Ubuntu or Ubuntu in WSL. Open a terminal in the extracted project folder.

```sh
sudo apt update
sudo apt install build-essential libmicrohttpd-dev libsqlite3-dev libjson-c-dev libssl-dev curl
make
./build/ride_server --create-admin admin
./build/ride_server --seed
./build/ride_server
```

Choose an administrator password with at least 10 characters, including letters and digits. Create the administrator only once per database. The seed command is for a demonstration database and adds sample locations and directions.

In another terminal:

```sh
curl http://127.0.0.1:8080/health
```

Expected response: `{"ok":true}`.

| Variable | Default | Use |
|---|---|---|
| `RIDE_DB` | `data/ride.db` | SQLite file |
| `RIDE_BIND` | `127.0.0.1` | IPv4 address to listen on |
| `RIDE_PORT` | `8080` | HTTP port |
| `RIDE_ORIGINS` | Local frontend ports 5173 and 3000 | Comma-separated allowed origins, without spaces |

Use `RIDE_ORIGINS` for a different frontend address. The default server listens locally. For integration across machines, use a trusted development network or a configured HTTPS proxy. Keep the database and real credentials outside version control.

## Day 1 to Day 7

| Day | Work | Main files |
|---|---|---|
| 1 | HTTP server, database schema, student registration, login and verification | `main.c`, `db.c`, `auth.c`, `schema.sql` |
| 2 | Locations, directed routes, ride publication and editing | `admin.c`, `rides.c` |
| 3 | Matching, invitations and join requests | `rides.c` |
| 4 | Seat allocation, conflict checks, reconfirmation and ride states | `rides.c`, `service.c` |
| 5 | Notifications, history, report review and retention | `admin.c`, `service.c` |
| 6 | HTTP integration checks for permissions, state changes, concurrency and persistence | `tests/test_api.py` |
| 7 | Setup, API handoff and design notes | `README.md`, `docs/API.md`, `docs/Design.md` |

Start with [API.md](docs/API.md) for frontend integration and [Design.md](docs/Design.md) to compare the code with the ER diagram and STD. The server is written in C. The HTTP test runner uses Python 3's standard library.

```sh
sudo apt install python3
make test
```

Tests use a temporary database and do not touch your application data. Back up the application database with `./build/ride_server --backup data/ride-backup.db`. The backup target must not exist.

This build is for local demonstration and frontend integration. The final team Task 3 diagrams, actual frontend and deployment environment have not been checked against this build.
