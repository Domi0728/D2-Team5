# Week 4: Joining and Ride States

This version extends the initial backend. The earlier version already included the main joining and ride-state endpoints. This update fixes boundary cases and adds seven integration tests. No database migration is required.

## Changes in this update

- Acceptance checks both the joining student and the organiser again. If an organiser becomes restricted after sending an invitation, acceptance returns 403 and leaves seats and membership unchanged.
- Repeating arrival confirmation with the current ride version returns the current ride without increasing its version or sending another arrival notification. This also works after completion.
- A member who already confirmed the current plan does not cause another update while other members still need to reconfirm.
- Request processing reads the current time again after acquiring the database lock, so invitation and session expiry checks do not use a time captured before waiting.
- Test accounts now use a counter rather than random IDs, avoiding accidental collisions in tests.

## Files to review

| File | What to read |
|---|---|
| src/rides.c | invitation_decision accepts, rejects or withdraws invitations. ride_routes handles editing, withdrawing, cancelling, reconfirming, starting and arriving. |
| src/service.c | update_group derives the pre-start state from member count and consent. maintenance expires invitations and rides. handle_request wraps operations in a transaction. |
| tests/test_api.py | The test_week4_* methods check this week's edge cases through HTTP. |

## Joining example

1. Student A creates a ride with capacity 3. A occupies one place, so two places remain.
2. A invites B. The pending invitation uses no place.
3. B reads the latest ride and accepts using its version as expected_version.
4. The server checks permissions, invitation expiry, organiser eligibility, route availability, capacity and B's other rides.
5. If all checks pass, the server adds B to memberships and marks the invitation ACCEPTED in the same transaction.
6. The group now has two members. If their consent is current, its state becomes CONFIRMED.

The database lock and transaction protect the seat update. The expected_version field also stops requests based on stale information. Two people accepting the last place with the same ride version cannot both succeed.

## Ride states

| State | Meaning |
|---|---|
| WAITING | Only the organiser remains. |
| CONFIRMED | At least two members agree to the current plan. This means the group has formed, not that the journey has ended. |
| RECONFIRMATION_REQUIRED | A route, time or meeting-point edit requires existing members to agree again. |
| IN_PROGRESS | The organiser has started the journey. |
| COMPLETED | Every active member has confirmed arrival. |
| CANCELLED | The organiser cancelled the journey before departure. |
| EXPIRED | The ride did not become ready by departure, or a confirmed ride missed its start grace period. |

Member withdrawal and whole-ride cancellation are different operations. A member withdrawing frees one place. If only the organiser remains, the state returns to WAITING. The organiser uses cancel to cancel the entire ride.

## Frontend calls

All authenticated writes need an Idempotency-Key header. Use a new key for a new action and the same key with the exact same body for a network retry. A retry can return the earlier successful response, so refresh the ride afterward if newer information is needed.

| Action | POST endpoint | JSON body |
|---|---|---|
| Accept invitation | /api/invitations/{id}/respond | {"action":"accept","expected_version":1} |
| Reject invitation | /api/invitations/{id}/respond | {"action":"reject","expected_version":1} |
| Withdraw invitation | /api/invitations/{id}/respond | {"action":"withdraw","expected_version":1} |
| Leave group | /api/rides/{id}/withdraw | {"expected_version":2} |
| Cancel ride | /api/rides/{id}/cancel | {"expected_version":2,"reason":"Plans changed"} |
| Reconfirm | /api/rides/{id}/reconfirm | {"expected_version":3} |
| Start | /api/rides/{id}/start | {"expected_version":4} |
| Arrive | /api/rides/{id}/arrive | {"expected_version":5} |

Replace example versions with the version returned by GET /api/rides/{id}. A stale version returns 409 even for a repeated action with a new key. Refresh before submitting again.

## Rules retained from the prototype

The organiser counts toward capacity. Invitations expire after five minutes or at departure, whichever comes first. Withdrawal and cancellation require a future departure. A confirmed group has a configurable 30-minute start grace period. The overlap check uses a configurable 60-minute departure window rather than a measured journey duration. These are project defaults to confirm with the group.

## Validation

Run make test from the project folder. The backend is C. Python is only the HTTP test runner, and requires no extra Python packages.

The suite covers joining without duplicate membership, two simultaneous requests for the last seat, invitation rejection and withdrawal, restricted organisers, repeated arrival, reconfirmation, pending invitations invalidated by edits or cancellation, permissions, expiry, completion, privacy and persistence.

Actual frontend integration and deployment still need testing. This is a coursework prototype, not a completed production service.
