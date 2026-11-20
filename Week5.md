# Week 5: Notifications, History, Privacy and Administration

This release continues Campus_Ride_Backend_C_2026-11-11_Migrations.zip, which already includes Week 4. It is a complete source package, not a standalone replacement backend. C11, libmicrohttpd, SQLite, json-c and OpenSSL are unchanged. Python is used only for tests. Database version remains 2.

## What changed

- Notification inbox: optional read/unread filter, with the existing response structure and pagination.
- Batch read: mark only the caller's notifications through a displayed ID. Notifications arriving after that ID stay unread. Retries use the existing Idempotency-Key mechanism.
- Reports: own/admin lists accept status and pagination filters; a report detail endpoint allows only its reporter or an administrator. List responses remain arrays.
- Audit log: administrator-only action/actor filters and pagination.
- Account restoration: sends a notification to the restored student. Retrying the same request does not add a second notice or audit record.
- Retention: administrator-only preview counts without deleting data; cleanup returns deletion counts and checks that rides are terminal. Pending reports and recently reviewed reports keep their ride records. Cleanup and its audit entry commit in one transaction.
- Nine additional HTTP tests cover these changes and existing history, route reuse, contact revocation, rejected/suspended accounts, logout, session expiry, and moderation rollback.

The earlier release already implemented report review and restrictions, profile privacy, history and route reuse. This release verifies those paths instead of claiming they are all newly implemented.

## Files changed

- src/admin.c: inbox, report and audit query extensions, restoration notice, retention preview/counts.
- tests/test_api.py: nine Week 5 test cases; all earlier tests retained.
- README.md and docs/API.md: current entry point and additive API contract.
- docs/Week5.md, docs/Feishu_Week5.md, docs/TestResults_Week5.txt, docs/Changes_Week5.patch: handoff and evidence.

## Start or upgrade

Use Ubuntu or Ubuntu in WSL with VS Code. A native Windows C compiler does not provide the server's POSIX dependencies.

```sh
sudo apt update
sudo apt install build-essential libmicrohttpd-dev libsqlite3-dev libjson-c-dev libssl-dev python3 curl
make
./build/ride_server --migrate
./build/ride_server --create-admin admin
./build/ride_server --seed
./build/ride_server
```

For an existing database, stop the server and make a backup using the old executable before updating source. Keep the data folder; do not replace it with an empty folder. Skip --create-admin and --seed when the database is already set up. Run make clean, make, --migrate, then make test. Tests use temporary databases and do not modify application records. No new migration is needed for a version-2 database.

## API examples

Use actual login tokens. Authenticated writes require an Idempotency-Key. Keep the same key and exact body for a retry; use a new key for a new action.

```sh
curl 'http://127.0.0.1:8080/api/notifications?read=unread&limit=20&offset=0' \
  -H "Authorization: Bearer $STUDENT_TOKEN"

curl -X POST http://127.0.0.1:8080/api/notifications/read-all \
  -H "Authorization: Bearer $STUDENT_TOKEN" \
  -H 'Content-Type: application/json' \
  -H 'Idempotency-Key: inbox-read-batch-001' \
  -d '{"through_id":42}'

curl 'http://127.0.0.1:8080/api/reports?status=PENDING&limit=20' \
  -H "Authorization: Bearer $STUDENT_TOKEN"

curl 'http://127.0.0.1:8080/api/admin/audit?action=review_report&limit=20' \
  -H "Authorization: Bearer $ADMIN_TOKEN"

curl http://127.0.0.1:8080/api/admin/maintenance/preview \
  -H "Authorization: Bearer $ADMIN_TOKEN"
```

Replace 42 with the highest notification ID from the inbox snapshot the user intends to mark read. Skip the batch request when the inbox is empty. Preview counts are a snapshot; a later cleanup can differ if data or time changes. Merely fetching the preview does not run retention cleanup. Normal request handling still performs the existing invitation/session expiry checks.

## Local validation and limits

The normal build passed all 35 tests (28 HTTP and 7 migration tests). Run make test. See TestResults_Week5.txt for actual results. Sanitizer execution is recorded separately in SanitizerResults_Week5.txt; leak detection is unavailable in this container, as explained in BuildResults_Week5.txt. A fault-injection test forces the moderation audit insert to fail and verifies that the restriction, report decision and notifications all roll back. Test-clock changes and fault injection are test-only.

No real frontend build, GitHub repository revision, deployed environment, or production database was available. These are local backend checks, not completed frontend integration, deployment, load testing, or full Task 2 acceptance. No remote push or deployment was performed.

## Rules still requiring confirmation

The 30-minute confirmed-ride start grace, 60-minute departure overlap interval, service endpoints, retention period and other prototype defaults remain configurable proposals. This release does not silently change their values. The supplied STD distinguishes member withdrawal from whole-ride cancellation and completes travel only after all active members arrive. Chat, payments and ratings remain outside this release.

The earlier four-stage acceptance plan remains a separate checklist. Week 5 is the older weekly work label, not evidence that earlier stages passed. The account/privacy tests extend S1-02; complete notification-event coverage, all match tie-breakers, browser integration, load and deployment acceptance remain open.
