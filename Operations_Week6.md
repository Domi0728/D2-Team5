# Runbook

## Local demonstration

Install README dependencies on Ubuntu or WSL. Keep the existing application database outside Git. From the extracted project root:

```sh
make
make test
python3 tools/demo.py --serve
```

Open http://127.0.0.1:5173 and click the run button. The demonstration makes real HTTP calls to the C executable on a temporary port. Python is only the demo/test launcher, not the application backend. It creates disposable student accounts, approves them, publishes a ride, matches it, sends and accepts an invitation, changes the meeting point, reconfirms, starts, records both arrivals, and checks history and notifications. It advances a test clock to avoid waiting for departure. Temporary accounts and the database are deleted after completion. Do not use --test-clock for ordinary deployment. Ctrl+C stops the page server. It listens only on loopback and is not a production frontend.

For a command-line demonstration: `python3 tools/demo.py`. For the bounded local matching load test: `python3 tools/demo.py --load`. Both save dated-independent Week6 JSON results under docs. The packaged report contains previously recorded successful data; the run button on the served page performs a fresh run.

## Ordinary application startup

For a new database only, create an administrator and seed demo locations using README.md. For an existing database preserve RIDE_DB and migrate without deleting it.

```sh
make
./build/ride_server --migrate
./build/ride_server
```

Set RIDE_ORIGINS to the actual frontend origin, without a trailing slash. The JSON API and authentication conventions remain in docs/API.md. The demo does not replace frontend registration, navigation or UX acceptance.

## Backup

Use a new private filename each time. The server's SQLite backup command supports a consistent copy while the source is running. Choose a backup directory readable only by the service operator. Do not commit backups, tokens or passwords.

```sh
RIDE_DB=/path/to/ride.db ./build/ride_server --backup /private/backups/ride-20261125.db
```

The target must not exist. For the submitted test evidence, test_week6_backup_restore_http created a backup, stopped its isolated server, restarted against the backup, compared ride/member data, authenticated the existing student, read notifications and verified PRAGMA integrity_check='ok'. No real database was replaced.

## Restore drill or incident recovery

1. Stop the application so it cannot write. Preserve the old database and associated WAL/SHM files together; do not delete them.
2. Validate the chosen backup with SQLite `PRAGMA integrity_check` in a read-only connection; expect exactly `ok`.
3. Copy the backup to a NEW database filename in a protected directory. Do not overwrite a live file. Point RIDE_DB to this new file, run --migrate, then start the server.
4. Check /health, administrator login, representative rides, memberships, notifications and permissions. The restored data are only as recent as the backup.
5. Reopen access after verification. Roll back by stopping the server and restoring the previous RIDE_DB setting if verification fails.

## HTTPS and service templates

The deploy directory contains reviewable systemd and nginx examples only. They have NOT been installed or executed on a deployment host. Configure a real domain and valid certificate paths before nginx validation. Create a dedicated ride user, install the executable under /opt/campus-ride, and a private writable /var/lib/campus-ride directory. Set RIDE_ORIGINS in /etc/campus-ride.env to the approved HTTPS frontend origin. Do not put the demo server, test clock or demonstration credentials online.

On the target host validate with `nginx -t` and `systemd-analyze verify deploy/ride-backend.service` before enabling anything. Verify HTTP redirects to HTTPS, trusted certificate chain, allowed/disallowed origins, role checks, backup permissions, service restart, and restore. These target-host checks and actual deployment remain pending; no domain, certificate or host access was provided.

Reference load: `python3 tools/reference_load.py` creates synthetic fixtures in an isolated database, measures 20 concurrent readers and 20 core-action workflows, and writes docs/ReferenceLoad_Week6.json. It does not load your real database.
