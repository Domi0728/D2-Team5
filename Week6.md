# Week 6 backend handoff

## Release and scope

Continues the 2026-11-25 Acceptance release without changing the C11/libmicrohttpd/SQLite/json-c/OpenSSL stack or schema version 2. New code consists of a cancellation-boundary fix, four regression methods, an executable HTTP demo/load harness, a local demonstration page, and deployment templates. There is no supplied GitHub revision or actual teammate frontend to merge or accept.

## Requirement correction

Task 2 REQ-1.9 permits withdrawal/cancellation before the ride starts. Previously the server also rejected every operation at or after scheduled departure even when the ride was still CONFIRMED. Removed that extra restriction: an unstarted ride within the existing grace period can be cancelled/left. IN_PROGRESS and terminal states remain rejected. WAITING and unresolved reconfirmation still expire at departure through existing maintenance. The 10-minute warning is already present and now tested at 601 seconds, 600 seconds, departure, and one second after departure. No new request/response field is added.

The existing 30-minute confirmed-start grace and 60-minute overlap interval remain provisional configuration decisions. This release does not approve those values on behalf of the group.

## Evidence

- 47 regression tests passed, including exact warning boundaries, repeat-withdrawal seat/notification counts, started-ride rejection and a backup restore exercised through HTTP.
- The isolated HTTP demonstration completes 11 recorded steps, including a material edit and reconfirmation before start and both arrival confirmations before completion.
- 400/400 matching reads passed with 8 clients and 30 candidate rides. See LoadResults_Week6.json for timings. This is a bounded loopback benchmark, not a production load guarantee or HTTPS benchmark.
- Browser demonstration checks are recorded separately in UIResults_Week6.txt.

## Acceptance mapping

| Baseline | Available local evidence | Remaining gate |
|---|---|---|
| REQ-1.1–1.3 | Existing account, eligibility, publication/edit and reconfirmation HTTP tests | Actual frontend forms and full role/session matrix |
| REQ-1.4–1.5, AT-02 | Matching endpoint/date/window/ranking tests, bounded read load | Remaining filter/query-name matrix and UI empty-result guidance |
| REQ-1.6 | Existing contact revocation, history visibility and suspension tests | Full privacy review against actual frontend |
| REQ-1.7–1.8, AT-03 | Invitation expiry/decision, same/different-key concurrency, transactional rollback and last-seat tests | Complete recipient/event matrix |
| REQ-1.9–1.10, AT-04–05, AT-07 | Withdrawal/cancellation timing, seat release, reconfirmation and start/arrival tests | Group confirmation of provisional grace/overlap values |
| REQ-1.11–1.15 | Existing notification, history, report, admin and retention tests | Full requirement-by-requirement audit and production operation |
| AT-01, AT-06, AT-08 | Portions covered by existing API and workflow tests | Not newly accepted wholesale; actual frontend/user acceptance still required |

Local backend handoff is runnable. The complete production application is not declared accepted: actual frontend integration, domain/certificate deployment, target-host security/operations validation and remaining requirement gates still need evidence. No push, external deployment or teammate message was performed.

## Reference-sized load and connection diagnostics

Also ran the Task 2 reference-sized fixture: 500 total accounts (including the administrator), 2000 initial rides (30 active, 1970 historical), 20 concurrent authenticated readers and 500 matching queries, followed by 20 concurrent workflows each publishing, editing, inviting and confirming. Fixtures are synthetic and explicitly identified in the script. See ReferenceLoad_Week6.json and Run1/Run2 JSON evidence. This does not replace a TLS benchmark or actual frontend usability acceptance.

An initial write burst encountered a BrokenPipe transport error; evidence is retained in ReferenceLoad_Before_Week6.txt. A retry of the original setting passed, so the root cause is not conclusively isolated. Increased the bounded connection allowance from 64 to 256 for burst headroom and enabled libmicrohttpd error logging. The thread pool remains 8, the per-request body limit remains 64 KiB and the timeout remains 15 seconds. Three reference-sized runs after this adjustment passed; this is evidence for the tested workload, not proof of unlimited capacity or a definitive diagnosis. Target-host long-duration testing remains pending.

Browser launch was unavailable in this environment and the browser download was denied. The local demo page route, real POST workflow and origin restriction were tested through HTTP, and its JavaScript passed node --check. Browser rendering and actual frontend usability are not marked accepted.
