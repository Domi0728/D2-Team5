# Database migration update, 2026-11-11

This is an incremental update to Campus_Ride_Backend_C_Week4.zip, not a replacement project. The backend remains C11 with libmicrohttpd, SQLite, json-c and OpenSSL. HTTP paths, JSON fields, response codes, authentication and idempotency rules are unchanged. Python runs tests only.

## Changes

- `src/db.c`: initialise or upgrade the database inside one BEGIN IMMEDIATE transaction. Check the version ledger before running migrations. Roll back on any migration error. Reject unknown versions, incomplete version history and non-empty unversioned databases instead of guessing their structure.
- `src/main.c`: add `--migrate`, which upgrades the selected database and exits without starting HTTP.
- `tests/test_migrations.py`: seven tests for fresh/repeated initialisation, v1 data preservation, failure rollback and retry, future versions, incomplete history, unversioned databases and concurrent migration processes.
- `README.md`: startup and acceptance-progress links.

Version 1 is the unchanged original `schema.sql` and embedded `src/schema.h`. Version 2 adds `session_expiry`, `invitation_expiry` and `ride_closed_at` indexes. Version 2 is implemented in db.c; executing schema.sql manually creates only the baseline. Use the executable to migrate to the current version. No index performance improvement is claimed without load measurements.

The migration preserves existing settings with INSERT OR IGNORE. Data and version records commit together. Migration errors stop startup. The HTTP server is not opened by --migrate. The version ledger must be consecutive: 1, then 2. Do not edit it by hand. A supported version ledger is not a full schema-corruption detector; backup and integrity checks still matter.

## Fresh local setup

Ubuntu or Ubuntu in WSL, inside `campus_ride_c`:

```sh
sudo apt update
sudo apt install build-essential libmicrohttpd-dev libsqlite3-dev libjson-c-dev libssl-dev python3 curl
make
./build/ride_server --migrate
./build/ride_server --create-admin admin
./build/ride_server --seed
./build/ride_server
```

The admin command prompts for a password. --seed is for demo data. --migrate reports `Database schema version 2 ready`. The normal server startup also migrates automatically.

## Upgrade an existing v1 database

Stop the old server first. Before replacing its executable, use the OLD executable to back up the database to a new path:

```sh
RIDE_DB=data/ride.db ./build/ride_server --backup data/before-migration-2026-11-11.db
```

Retain the old release and backup separately. Copy in this release's source files, preserving your data folder, then:

```sh
make clean
make
RIDE_DB=data/ride.db ./build/ride_server --migrate
make test
RIDE_DB=data/ride.db ./build/ride_server
```

Do not run old and new server versions against the same live database. Automatic schema downgrades are not supported. For rollback, stop the new server and run the old release against a separate restored copy of the pre-upgrade backup. This run tested migration rollback, not a full deployment restore drill. Real user databases were not opened or migrated in this run.

## Existing HTTP examples

No new frontend request is required. Existing examples below remain valid.

```sh
curl http://127.0.0.1:8080/health
curl -X POST http://127.0.0.1:8080/api/register \
  -H 'Content-Type: application/json' \
  -d '{"student_id":"1234567890","real_name":"Demo Student","nickname":"Demo","password":"ExampleOnly123"}'
curl -X POST http://127.0.0.1:8080/api/login \
  -H 'Content-Type: application/json' \
  -d '{"student_id":"1234567890","password":"ExampleOnly123"}'
```

The password is demonstration-only. Registration returns an id and PENDING; only approval permits publishing. Use the token returned by login for GET /api/me. An admin can approve the returned id using POST /api/admin/users/{id}/verify, bearer authorization and an Idempotency-Key, with body `{"decision":"APPROVED","reason":"Enrollment checked"}`. Existing API.md describes the unchanged publishing and joining calls.

## Validation

Original baseline: 19 HTTP tests passed. After rebuilding the changed C sources: 26 tests passed (19 existing HTTP tests plus 7 migration tests). Run `make test`. Tests use temporary databases. See TestResults_2026-11-11.txt and BuildResults_2026-11-11.txt.

The execution environment had development libraries in `../tmp/cdeps/usr` rather than system include paths. Compilation used the explicit include/library/rpath flags captured in BuildResults_2026-11-11.txt and produced no compiler warnings. A plain clean build in that environment initially failed for missing json-c headers; the subsequent build used the existing local dependencies. A normally provisioned Ubuntu/WSL machine uses the install commands above and plain make. Binaries and environment-specific dependencies are excluded from delivery.
