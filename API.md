# HTTP API

Base URL: `http://127.0.0.1:8080`. Send JSON objects using `Content-Type: application/json`. Every route except health, registration and login needs `Authorization: Bearer <access_token>`. For authenticated POST and PATCH requests, also send a unique `Idempotency-Key` (8–80 letters, digits, hyphens, periods or underscores). Use the same key and exact body to retry a request after a connection error. Use a new key when submitting an updated request. Successful writes are cached for about one day; the bearer session still has to be valid.

Timestamps are Unix seconds. For display, use the Macau time zone. IDs and versions are numbers. SQLite flags in responses use 0/1; when sending a boolean setting, send JSON `true` or `false`.

Successful requests return HTTP 200. Errors use `{"error":{"code":"STALE_VERSION","message":"..."}}`. Common codes are 400 for malformed requests, 401 for missing or invalid credentials, 403 for insufficient access, 409 for business conflicts, 422 for invalid inputs and 503 for a database failure. On `STALE_VERSION`, fetch the latest ride, show the updated plan and obtain confirmation before sending a fresh request.

## Accounts and setup

| Method and path | Input or result |
|---|---|
| GET `/health` | Health response |
| POST `/api/register` | `student_id` (10-digit string), `real_name`, `nickname`, `password`; returns ID and PENDING |
| POST `/api/login` | `student_id`, `password`; returns `access_token`, `expires_at`, `user` |
| GET `/api/me` | Own account |
| PATCH `/api/me` | All four fields: `nickname`, `phone`, `contact_share`, `history_visible` |
| POST `/api/logout` | Empty object; revokes token |
| GET `/api/users/{id}` | Student profile; phone only when sharing permits |
| GET `/api/users/{id}/history` | Summary of another student's history if shared |
| GET `/api/locations` | Active locations and alias arrays |
| GET `/api/routes` | Active directed routes |
| GET `/api/settings` | Current settings as key/value rows |

An administrator logs in at the same endpoint, using the administrator name as `student_id`. Students can log in before approval but cannot publish or join rides.

## Rides

| Method and path | Input or result |
|---|---|
| GET `/api/rides` | Upcoming open rides |
| POST `/api/rides` | Ride fields below |
| GET `/api/rides/{id}` | Current ride, version and seats; members only for participants or administrators |
| PATCH `/api/rides/{id}` | All ride fields plus `expected_version`; organiser only |
| GET `/api/matches` | Query: `origin_id`, `destination_id`, `departure_at`, optional `tolerance_minutes`, `min_seats`, `sort=default\|departure` |
| GET `/api/me/rides` | Own active rides |
| GET `/api/me/history` | Own closed rides |
| POST `/api/rides/{id}/reuse` | Empty object; returns draft fields from own closed ride |
| POST `/api/rides/{id}/cancel` | `expected_version`, optional `reason`; organiser only |
| POST `/api/rides/{id}/withdraw` | `expected_version`, optional `reason`; members other than organiser |
| POST `/api/rides/{id}/reconfirm` | `expected_version`; accept the current plan |
| POST `/api/rides/{id}/start` | `expected_version`; organiser only, at or after departure |
| POST `/api/rides/{id}/arrive` | `expected_version`; record own arrival |

Ride request example (replace `departure_at` with a future time before sending):

```json
{
  "origin_id": 1,
  "destination_id": 2,
  "departure_at": 1800003600,
  "meeting_point": "North gate",
  "capacity": 3,
  "tolerance_minutes": 15,
  "luggage_note": ""
}
```

A ride response has `id`, `status`, `version`, `consent_version`, `member_count` and `available_seats`, in addition to its ride details. Capacity includes the organiser. Use the ride's `version` for `expected_version`; the consent version tracks material plan changes only. Ride lists, matches and notifications accept `limit` (1–100) and `offset`.

## Invitations, notifications and reports

| Method and path | Input or result |
|---|---|
| GET `/api/invitations` | Sent and received invitations/requests |
| POST `/api/rides/{id}/invitations` | `recipient_id`, `expected_version`; organiser invites a student |
| POST `/api/rides/{id}/join-requests` | `expected_version`; student asks the organiser to join |
| POST `/api/invitations/{id}/respond` | `action` = accept, reject or withdraw; `expected_version` is the current ride version |
| GET `/api/notifications` | `unread_count`, `items` |
| POST `/api/notifications/{id}/read` | Empty object |
| GET `/api/reports` | Own submitted reports |
| POST `/api/reports` | `ride_id`, `target_user_id`, `category` (NO_SHOW, LATE, BEHAVIOUR or OTHER), `description` |

Only the recipient can accept or reject, and only the sender can withdraw. A pending invitation or request does not hold a seat. The response to an invitation decision includes the updated `ride`.

## Administrator

| Method and path | Input or result |
|---|---|
| GET `/api/admin/users` | Users and verification state |
| POST `/api/admin/users/{id}/verify` | `decision` = APPROVED or REJECTED; `reason` |
| POST `/api/admin/users/{id}/restore` | `reason`; clears a suspension or restriction |
| GET `/api/admin/reports` | Submitted reports |
| POST `/api/admin/reports/{id}/review` | `action` = DISMISS, WARN, RESTRICT or SUSPEND; `note`; `restriction_hours` 1–720 for RESTRICT |
| GET `/api/admin/locations` | All locations |
| POST `/api/admin/locations` | `name`, `aliases` array |
| GET `/api/admin/locations/{id}/impact` | Active rides using location |
| PATCH `/api/admin/locations/{id}` | `active`, `confirm_affected` booleans |
| POST `/api/admin/routes` | `origin_id`, `destination_id`, `active` boolean |
| GET `/api/admin/config` | Key/value settings |
| PATCH `/api/admin/config` | Complete settings object, as below |
| GET `/api/admin/audit` | Recent action log |
| POST `/api/admin/maintenance` | Empty object; remove eligible old history |

```json
{
  "default_tolerance_minutes": 15,
  "max_tolerance_minutes": 120,
  "invite_minutes": 5,
  "max_capacity": 6,
  "late_cancel_minutes": 10,
  "overlap_minutes": 60,
  "start_grace_minutes": 30,
  "retention_days": 90
}
```

Tolerance bounds: 0–120, default no greater than maximum. Other bounds in the above order: 1–30, 2–6, 0–60, 1–240, 1–120 and 1–365.

## First integration flow

1. Log in as administrator. Register two student accounts and approve both.
2. Log in as student A and publish a ride. Keep its ID and version.
3. Log in as student B and send a join request with the current ride version.
4. Student A accepts the request. The response should show CONFIRMED and two members.
5. At departure, student A starts the ride. Both students confirm arrival. The ride should then appear in both histories.
