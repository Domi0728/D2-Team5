import concurrent.futures
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get('RIDE_TEST_BINARY', ROOT / 'build/ride_server'))


class MigrationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / 'ride.db'
        self.env = dict(os.environ, RIDE_DB=str(self.path))

    def run_migration(self):
        return subprocess.run([BINARY, '--migrate'], env=self.env, cwd=ROOT,
                              capture_output=True, text=True, timeout=25)

    def sql(self, statement):
        with sqlite3.connect(self.path) as conn:
            return conn.execute(statement).fetchall()

    def snapshot(self):
        with sqlite3.connect(self.path) as conn:
            return list(conn.iterdump())

    def legacy(self):
        with sqlite3.connect(self.path) as conn:
            conn.executescript((ROOT / 'schema.sql').read_text())
            conn.executescript("""
                INSERT INTO config VALUES('retention_days',42);
                INSERT INTO users(id,student_id,real_name,nickname,password_hash,created_at)
                    VALUES(1,'0000000001','Fixture','Fixture','fixture-hash',1800000000);
                INSERT INTO locations VALUES(1,'Campus',1),(2,'Port',1);
                INSERT INTO routes VALUES(1,2,1);
                INSERT INTO rides(id,organiser_id,origin_id,destination_id,origin_name,
                    destination_name,departure_at,meeting_point,capacity,created_at,updated_at)
                    VALUES(1,1,1,2,'Campus','Port',1800003600,'Gate',3,1800000000,1800000000);
                INSERT INTO memberships(ride_id,user_id,member_type,consent_version,joined_at)
                    VALUES(1,1,'ORGANISER',1,1800000000);
                INSERT INTO sessions VALUES('fixture-token-hash',1,1800007200);
                INSERT INTO notifications(user_id,event_key,content,created_at)
                    VALUES(1,'fixture-event','Fixture notice',1800000000);
            """)

    def assert_current(self):
        self.assertEqual(self.sql('SELECT version FROM schema_migrations ORDER BY version'), [(1,), (2,)])
        self.assertEqual(self.sql('PRAGMA integrity_check'), [('ok',)])
        self.assertEqual(self.sql('PRAGMA foreign_key_check'), [])

    def test_fresh_database_and_repeated_start(self):
        result = self.run_migration()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_current()
        self.assertEqual(self.sql('SELECT count(*) FROM config'), [(8,)])
        before = self.snapshot()
        result = self.run_migration()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.snapshot(), before)

    def test_upgrade_preserves_records_and_custom_config(self):
        self.legacy()
        tables = ['users', 'locations', 'routes', 'rides', 'memberships', 'sessions', 'notifications']
        before = {table: self.sql('SELECT * FROM ' + table) for table in tables}
        result = self.run_migration()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_current()
        for table in tables:
            self.assertEqual(self.sql('SELECT * FROM ' + table), before[table])
        self.assertEqual(self.sql("SELECT value FROM config WHERE key='retention_days'"), [(42,)])

    def test_failure_rolls_back_indexes_and_version(self):
        self.legacy()
        self.sql('CREATE TABLE invitation_expiry(value TEXT)')
        before = self.snapshot()
        result = self.run_migration()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('rolled back', result.stderr)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.sql("SELECT name FROM sqlite_master WHERE name='session_expiry'"), [])
        self.sql('DROP TABLE invitation_expiry')
        result = self.run_migration()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_current()

    def test_future_version_is_rejected_without_changes(self):
        self.legacy()
        self.sql('INSERT INTO schema_migrations VALUES(99)')
        before = self.snapshot()
        result = self.run_migration()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Unsupported', result.stderr)
        self.assertEqual(self.snapshot(), before)

    def test_incomplete_version_history_is_rejected(self):
        self.legacy()
        self.sql('UPDATE schema_migrations SET version=2')
        before = self.snapshot()
        self.assertNotEqual(self.run_migration().returncode, 0)
        self.assertEqual(self.snapshot(), before)

    def test_unversioned_nonempty_database_is_rejected(self):
        self.sql('CREATE TABLE unrelated(value TEXT)')
        before = self.snapshot()
        result = self.run_migration()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Unversioned', result.stderr)
        self.assertEqual(self.snapshot(), before)

    def test_concurrent_upgrade_applies_once(self):
        self.legacy()
        with concurrent.futures.ThreadPoolExecutor(2) as pool:
            results = list(pool.map(lambda _: self.run_migration(), range(2)))
        for result in results:
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_current()
        self.assertEqual(self.sql("SELECT count(*) FROM sqlite_master WHERE type='index' "
                                 "AND name IN ('session_expiry','invitation_expiry','ride_closed_at')"), [(3,)])


if __name__ == '__main__':
    unittest.main()
