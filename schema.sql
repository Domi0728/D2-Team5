
CREATE TABLE IF NOT EXISTS schema_migrations(version INTEGER PRIMARY KEY);
CREATE TABLE IF NOT EXISTS users(
 id INTEGER PRIMARY KEY, student_id TEXT UNIQUE NOT NULL, real_name TEXT NOT NULL,
 nickname TEXT NOT NULL, password_hash TEXT NOT NULL,
 role TEXT NOT NULL CHECK(role IN ('STUDENT','ADMIN')) DEFAULT 'STUDENT',
 verify_status TEXT NOT NULL DEFAULT 'PENDING' CHECK(verify_status IN ('PENDING','APPROVED','REJECTED')),
 restriction_until INTEGER, suspended INTEGER NOT NULL DEFAULT 0,
 phone TEXT NOT NULL DEFAULT '', contact_share INTEGER NOT NULL DEFAULT 0,
 history_visible INTEGER NOT NULL DEFAULT 0, avatar TEXT NOT NULL DEFAULT '',
 tags TEXT NOT NULL DEFAULT '[]', created_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS sessions(
 token_hash TEXT PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id), expires_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS login_attempts(
 bucket TEXT PRIMARY KEY, failures INTEGER NOT NULL DEFAULT 0, window_start INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS locations(
 id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL, active INTEGER NOT NULL DEFAULT 1);
CREATE TABLE IF NOT EXISTS location_aliases(
 alias TEXT PRIMARY KEY COLLATE NOCASE, location_id INTEGER NOT NULL REFERENCES locations(id));
CREATE TABLE IF NOT EXISTS routes(
 origin_id INTEGER NOT NULL REFERENCES locations(id), destination_id INTEGER NOT NULL REFERENCES locations(id),
 active INTEGER NOT NULL DEFAULT 1, PRIMARY KEY(origin_id,destination_id), CHECK(origin_id<>destination_id));
CREATE TABLE IF NOT EXISTS config(key TEXT PRIMARY KEY, value INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS rides(
 id INTEGER PRIMARY KEY, organiser_id INTEGER NOT NULL REFERENCES users(id),
 origin_id INTEGER NOT NULL REFERENCES locations(id), destination_id INTEGER NOT NULL REFERENCES locations(id),
 origin_name TEXT NOT NULL, destination_name TEXT NOT NULL, departure_at INTEGER NOT NULL,
 meeting_point TEXT NOT NULL, capacity INTEGER NOT NULL CHECK(capacity BETWEEN 2 AND 6),
 tolerance_minutes INTEGER NOT NULL DEFAULT 15, luggage_note TEXT NOT NULL DEFAULT '', tags TEXT NOT NULL DEFAULT '[]',
 status TEXT NOT NULL DEFAULT 'WAITING' CHECK(status IN
 ('WAITING','CONFIRMED','RECONFIRMATION_REQUIRED','IN_PROGRESS','COMPLETED','CANCELLED','EXPIRED')),
 version INTEGER NOT NULL DEFAULT 1, consent_version INTEGER NOT NULL DEFAULT 1,
 created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, closed_at INTEGER, cancel_reason TEXT NOT NULL DEFAULT '',
 CHECK(origin_id<>destination_id));
CREATE TABLE IF NOT EXISTS memberships(
 id INTEGER PRIMARY KEY, ride_id INTEGER NOT NULL REFERENCES rides(id) ON DELETE CASCADE,
 user_id INTEGER NOT NULL REFERENCES users(id), member_type TEXT NOT NULL,
 active INTEGER NOT NULL DEFAULT 1, consent_version INTEGER NOT NULL, arrival_flag INTEGER NOT NULL DEFAULT 0,
 joined_at INTEGER NOT NULL, UNIQUE(ride_id,user_id));
CREATE TABLE IF NOT EXISTS invitations(
 id INTEGER PRIMARY KEY, ride_id INTEGER NOT NULL REFERENCES rides(id) ON DELETE CASCADE,
 sender_id INTEGER NOT NULL REFERENCES users(id), recipient_id INTEGER NOT NULL REFERENCES users(id),
 kind TEXT NOT NULL DEFAULT 'INVITE' CHECK(kind IN ('INVITE','REQUEST')),
 status TEXT NOT NULL DEFAULT 'PENDING' CHECK(status IN ('PENDING','ACCEPTED','REJECTED','WITHDRAWN','EXPIRED')),
 ride_version INTEGER NOT NULL, created_at INTEGER NOT NULL, deadline INTEGER NOT NULL);
CREATE UNIQUE INDEX IF NOT EXISTS pending_invitation ON invitations(ride_id,sender_id,recipient_id,kind) WHERE status='PENDING';
CREATE TABLE IF NOT EXISTS notifications(
 id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id), event_key TEXT NOT NULL,
 ride_id INTEGER REFERENCES rides(id) ON DELETE SET NULL, content TEXT NOT NULL,
 is_read INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL, UNIQUE(user_id,event_key));
CREATE TABLE IF NOT EXISTS reports(
 id INTEGER PRIMARY KEY, reporter_id INTEGER NOT NULL REFERENCES users(id),
 ride_id INTEGER REFERENCES rides(id) ON DELETE SET NULL, target_user_id INTEGER NOT NULL REFERENCES users(id),
 category TEXT NOT NULL, description TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'PENDING',
 decision TEXT, moderation_note TEXT, created_at INTEGER NOT NULL, reviewed_at INTEGER,
 UNIQUE(reporter_id,ride_id,target_user_id,category));
CREATE TABLE IF NOT EXISTS audit_logs(
 id INTEGER PRIMARY KEY, actor_id INTEGER REFERENCES users(id), action TEXT NOT NULL,
 entity_id INTEGER, details TEXT NOT NULL, created_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS ride_changes(
 id INTEGER PRIMARY KEY, ride_id INTEGER NOT NULL REFERENCES rides(id) ON DELETE CASCADE,
 version INTEGER NOT NULL, actor_id INTEGER NOT NULL REFERENCES users(id), snapshot TEXT NOT NULL, created_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS idempotency(
 user_id INTEGER NOT NULL REFERENCES users(id), key TEXT NOT NULL, fingerprint TEXT NOT NULL,
 result TEXT NOT NULL, created_at INTEGER NOT NULL, PRIMARY KEY(user_id,key));
CREATE INDEX IF NOT EXISTS ride_search ON rides(origin_id,destination_id,departure_at,status);
CREATE INDEX IF NOT EXISTS member_user ON memberships(user_id,active);
CREATE INDEX IF NOT EXISTS inbox_user ON notifications(user_id,created_at);
INSERT OR IGNORE INTO schema_migrations VALUES(1);
