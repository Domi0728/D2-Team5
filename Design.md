# Design Notes

This implementation uses the Task 1 proposal, Task 2 requirements and the supplied ER diagram as inputs. The table below is a mapping to structured analysis concepts for Task 3.

| Process | Data stores | Implementation |
|---|---|---|
| Account management and approval | users, sessions | auth.c |
| Publish and maintain rides | locations, routes, rides, ride_changes | rides.c |
| Search and form a group | rides, invitations, memberships | rides.c |
| Manage ride states | rides, memberships, invitations | rides.c, service.c |
| Send notifications and access history | notifications, rides, memberships | admin.c, rides.c, service.c |
| Review reports and manage settings | reports, config, audit_logs | admin.c |

Student and administrator records share the users table, with a role field. The ER relationship between a student and a ride is stored in memberships. Available seats are calculated from capacity minus active memberships. An invitation's ride_version stores the consent revision at send time.

## State transitions

| From | Event and condition | To |
|---|---|---|
| Initial | Approved student publishes a valid ride | WAITING |
| WAITING | A second member joins and both accept the current plan | CONFIRMED |
| CONFIRMED | A material edit makes a member's consent outdated | RECONFIRMATION_REQUIRED |
| RECONFIRMATION_REQUIRED | All active members accept the current plan | CONFIRMED |
| CONFIRMED or RECONFIRMATION_REQUIRED | Withdrawal leaves only the organiser | WAITING |
| Any pre-start state | Organiser cancels before departure | CANCELLED |
| WAITING or RECONFIRMATION_REQUIRED | Departure time is reached | EXPIRED |
| CONFIRMED | Start grace period ends without starting | EXPIRED |
| CONFIRMED | Organiser starts after departure with valid member consent | IN_PROGRESS |
| IN_PROGRESS | Every active member confirms arrival | COMPLETED |

A material edit changes the route, departure time or meeting point. It withdraws pending invitations and requires existing members to reconfirm. Capacity cannot be reduced below the current membership count. Seat allocation and membership changes use one database transaction. A request for the last seat can fail because the ride version changed or because the group is full.

## Prototype defaults

- Matching compares the same directed route and Macau calendar date. It filters by the requested time tolerance and available seats. Default ordering is smallest time difference, most shared completed rides, earliest publication, then ride ID.
- An invitation is open for up to five minutes and never extends past departure.
- Overlap is approximated by departures less than 60 minutes apart. The default start grace period is 30 minutes. Both are configurable.
- Cancellation and withdrawal are allowed before departure. The late warning is returned within 10 minutes of departure.
- Retention cleanup is an administrator action, with a default 90-day cutoff. Open reports and their rides are retained. Audit records are retained separately.
- Locations in the seed are demonstration data. The group should confirm the supported routes before collecting any real records.

## Scope

The prototype includes admin approval and in-app notifications. It does not integrate university login, push delivery, payments, GPS, or a driver dispatch service. Avatar and preference tag fields exist in the schema but have no editing API. Deployment, throughput at target data volumes, and long-running availability are separate checks. The project can be run locally with one server instance.
