import concurrent.futures
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get('RIDE_TEST_BINARY', ROOT / 'build/ride_server'))
PASSWORD = 'SamplePassword123'


class ApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.folder = Path(cls.tmp.name)
        cls.clock = cls.folder / 'clock'
        cls.base_time = 1800000000
        cls.clock.write_text(str(cls.base_time))
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            cls.port = sock.getsockname()[1]
        cls.url = f'http://127.0.0.1:{cls.port}'
        cls.env = dict(os.environ, RIDE_DB=str(cls.folder / 'test.db'),
                       RIDE_PORT=str(cls.port), RIDE_ADMIN_PASSWORD=PASSWORD)
        subprocess.run([BINARY, '--create-admin', 'admin'], env=cls.env, check=True, capture_output=True)
        subprocess.run([BINARY, '--seed'], env=cls.env, check=True, capture_output=True)
        cls.log = open(cls.folder / 'server.log', 'w+')
        cls.start_server()
        cls.admin = cls.login('admin')
        cls.sequence = 0

    @classmethod
    def start_server(cls):
        cls.server = subprocess.Popen([BINARY, '--test-clock', cls.clock], env=cls.env,
                                      stdout=cls.log, stderr=cls.log)
        for _ in range(100):
            try:
                if cls.call('GET', '/health')[0] == 200:
                    return
            except OSError:
                time.sleep(0.03)
        raise RuntimeError('Server did not start')

    @classmethod
    def tearDownClass(cls):
        cls.server.terminate()
        cls.server.wait(timeout=10)
        cls.log.seek(0)
        output = cls.log.read()
        cls.log.close()
        cls.tmp.cleanup()
        if 'ERROR: AddressSanitizer' in output or 'runtime error:' in output or cls.server.returncode != 0:
            raise AssertionError(output)

    @classmethod
    def call(cls, method, path, body=None, token=None, key=None, raw=None):
        headers = {'Content-Type': 'application/json'}
        if token:
            headers['Authorization'] = 'Bearer ' + token
        if method != 'GET':
            headers['Idempotency-Key'] = key or str(uuid.uuid4())
        data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        request = urllib.request.Request(cls.url + path, data=data, headers=headers, method=method)
        try:
            response = urllib.request.urlopen(request, timeout=20)
        except urllib.error.HTTPError as exc:
            response = exc
        with response:
            return response.status, json.load(response)

    @classmethod
    def login(cls, student):
        status, result = cls.call('POST', '/api/login', {'student_id': student, 'password': PASSWORD})
        assert status == 200, result
        return result['access_token']

    def setUp(self):
        type(self).sequence += 1
        self.set_clock(self.base_time)

    def set_clock(self, value):
        temporary = self.clock.with_suffix('.tmp')
        temporary.write_text(str(value))
        temporary.replace(self.clock)

    def account(self, approved=True):
        student = f'{self.sequence:06d}{uuid.uuid4().int % 10000:04d}'
        status, user = self.call('POST', '/api/register', {
            'student_id': student, 'real_name': "Student O'Neil", 'nickname': 'Student', 'password': PASSWORD})
        self.assertEqual(status, 200, user)
        if approved:
            self.expect('POST', f"/api/admin/users/{user['id']}/verify",
                        {'decision': 'APPROVED', 'reason': 'Enrollment checked'}, self.admin)
        return user['id'], self.login(student)

    def expect(self, method, path, body=None, token=None, status=200, **kwargs):
        code, result = self.call(method, path, body, token, **kwargs)
        self.assertEqual(code, status, result)
        return result

    def create(self, token, capacity=3, departure=None, **changes):
        data = dict(origin_id=1, destination_id=2, departure_at=departure or self.base_time+3600,
                    meeting_point='North gate', capacity=capacity, tolerance_minutes=15, luggage_note='')
        data.update(changes)
        return self.expect('POST', '/api/rides', data, token)

    def ride(self, token, rid):
        return self.expect('GET', f'/api/rides/{rid}', token=token)

    def action(self, token, ride, action, **extra):
        data = dict(expected_version=ride['version'], **extra)
        return self.expect('POST', f"/api/rides/{ride['id']}/{action}", data, token)

    def invite(self, token, ride, recipient):
        return self.action(token, ride, 'invitations', recipient_id=recipient)

    def accept(self, token, invitation, ride):
        return self.expect('POST', f"/api/invitations/{invitation['id']}/respond",
                           {'action': 'accept', 'expected_version': ride['version']}, token)['ride']

    def group(self, capacity=3):
        owner, ot = self.account()
        member, mt = self.account()
        ride = self.create(ot, capacity)
        ride = self.accept(mt, self.invite(ot, ride, member), ride)
        return owner, ot, member, mt, ride

    def test_auth_validation_and_permissions(self):
        uid, token = self.account(False)
        self.expect('POST', '/api/rides', {}, token, status=403)
        self.expect('GET', '/api/admin/users', token=token, status=403)
        self.expect('GET', '/api/me', status=401)
        self.expect('POST', '/api/login', {'student_id': 'admin', 'password': 'wrong'}, status=401)
        self.expect('POST', '/api/register', raw=b'{bad', status=400)
        self.expect('POST', '/api/register', {'student_id': '1234567890', 'real_name': 'x\x00y',
                                             'nickname': 'x', 'password': PASSWORD}, status=422)
        me = self.expect('GET', '/api/me', token=token)
        self.assertEqual(me['real_name'], "Student O'Neil")
        self.assertNotIn('password_hash', me)

    def test_creation_idempotency_and_boundaries(self):
        uid, token = self.account()
        ride = self.create(token)
        self.assertEqual((ride['status'], ride['available_seats']), ('WAITING', 2))
        self.expect('POST', '/api/rides', dict(origin_id=1), token, status=422)
        self.expect('POST', '/api/rides', {}, token, key='x', status=400)
        key = str(uuid.uuid4())
        body = {'expected_version': ride['version'], 'reason': 'Plans changed'}
        path = f"/api/rides/{ride['id']}/cancel"
        first = self.expect('POST', path, body, token, key=key)
        self.assertEqual(first, self.expect('POST', path, body, token, key=key))
        self.expect('POST', path, dict(body, reason='Different'), token, status=409, key=key)

    def test_join_request_and_duplicate_membership(self):
        owner, ot = self.account()
        member, mt = self.account()
        ride = self.create(ot)
        request = self.action(mt, ride, 'join-requests')
        self.assertEqual(self.ride(ot, ride['id'])['member_count'], 1)
        ride = self.accept(ot, request, ride)
        self.assertEqual(ride['status'], 'CONFIRMED')
        self.expect('POST', f"/api/rides/{ride['id']}/join-requests",
                    {'expected_version': ride['version']}, mt, status=409)

    def test_concurrent_last_seat(self):
        owner, ot = self.account()
        first, ft = self.account()
        second, st = self.account()
        ride = self.create(ot, 2)
        invites = [(ft, self.invite(ot, ride, first)), (st, self.invite(ot, ride, second))]
        self.assertEqual(self.ride(ot, ride['id'])['available_seats'], 1)
        def respond(pair):
            token, invitation = pair
            return self.call('POST', f"/api/invitations/{invitation['id']}/respond",
                             {'action': 'accept', 'expected_version': ride['version']}, token)
        with concurrent.futures.ThreadPoolExecutor(2) as pool:
            results = list(pool.map(respond, invites))
        self.assertEqual(sorted(x[0] for x in results), [200, 409])
        self.assertEqual(self.ride(ot, ride['id'])['available_seats'], 0)

    def test_material_edit_and_reconfirmation(self):
        owner, ot, member, mt, ride = self.group()
        data = {k: ride[k] for k in ('origin_id','destination_id','departure_at','meeting_point','capacity','tolerance_minutes','luggage_note')}
        data.update(expected_version=ride['version'], luggage_note='One bag')
        ride = self.expect('PATCH', f"/api/rides/{ride['id']}", data, ot)
        self.assertEqual(ride['status'], 'CONFIRMED')
        data.update(expected_version=ride['version'], meeting_point='South gate')
        ride = self.expect('PATCH', f"/api/rides/{ride['id']}", data, ot)
        self.assertEqual(ride['status'], 'RECONFIRMATION_REQUIRED')
        ride = self.action(mt, ride, 'reconfirm')
        self.assertEqual(ride['status'], 'CONFIRMED')

    def test_withdraw_and_overlap(self):
        owner, ot, member, mt, ride = self.group()
        data = {k: ride[k] for k in ('origin_id','destination_id','departure_at','meeting_point','capacity','tolerance_minutes','luggage_note')}
        self.expect('POST', '/api/rides', data, mt, status=409)
        ride = self.action(mt, ride, 'withdraw')
        self.assertEqual((ride['status'], ride['member_count']), ('WAITING', 1))
        ride = self.action(ot, ride, 'cancel', reason='No group')
        self.assertEqual(ride['status'], 'CANCELLED')

    def test_completion_history_and_privacy(self):
        owner, ot, member, mt, ride = self.group()
        stranger, token = self.account()
        self.expect('POST', f"/api/rides/{ride['id']}/start", {'expected_version': ride['version']}, ot, status=409)
        self.set_clock(ride['departure_at'])
        ride = self.action(ot, ride, 'start')
        self.expect('GET', f"/api/rides/{ride['id']}", token=token, status=403)
        ride = self.action(mt, ride, 'arrive')
        self.assertEqual(ride['status'], 'IN_PROGRESS')
        ride = self.action(ot, ride, 'arrive')
        self.assertEqual(ride['status'], 'COMPLETED')
        history = self.expect('GET', '/api/me/history', token=mt)
        self.assertIn(ride['id'], [x['id'] for x in history])
        draft = self.action(mt, ride, 'reuse')
        self.assertNotIn('members', draft)
        self.expect('GET', f'/api/users/{member}/history', token=token, status=404)

    def test_expiry_and_notifications(self):
        owner, ot = self.account()
        member, mt = self.account()
        ride = self.create(ot)
        invitation = self.invite(ot, ride, member)
        self.set_clock(self.base_time+301)
        self.expect('POST', f"/api/invitations/{invitation['id']}/respond",
                    {'action':'accept','expected_version':ride['version']}, mt, status=409)
        items = self.expect('GET', '/api/notifications', token=mt)['items']
        self.assertTrue(any(x['content']=='Invitation expired' for x in items))
        self.expect('POST', f"/api/notifications/{items[0]['id']}/read", {}, ot, status=404)
        self.set_clock(ride['departure_at'])
        self.assertEqual(self.ride(ot, ride['id'])['status'], 'EXPIRED')

    def test_matches_direction_and_window(self):
        owner, ot = self.account()
        seeker, st = self.account()
        ride = self.create(ot, departure=self.base_time+7200)
        query = f'/api/matches?origin_id=1&destination_id=2&departure_at={ride["departure_at"]+900}&tolerance_minutes=15'
        matches = self.expect('GET', query, token=st)
        self.assertIn(ride['id'], [x['id'] for x in matches])
        matches = self.expect('GET', query.replace('tolerance_minutes=15','tolerance_minutes=14'), token=st)
        self.assertNotIn(ride['id'], [x['id'] for x in matches])
        matches = self.expect('GET', query.replace('origin_id=1&destination_id=2','origin_id=2&destination_id=1'), token=st)
        self.assertNotIn(ride['id'], [x['id'] for x in matches])

    def test_contact_and_moderation(self):
        owner, ot, member, mt, ride = self.group()
        outsider, token = self.account()
        self.expect('PATCH','/api/me', dict(nickname='Member', phone='+853 12345678', contact_share=True, history_visible=False), mt)
        self.assertIn('phone', self.expect('GET',f'/api/users/{member}',token=ot))
        self.assertNotIn('phone', self.expect('GET',f'/api/users/{member}',token=token))
        body = dict(ride_id=ride['id'], target_user_id=member, category='BEHAVIOUR', description='Incident description')
        self.expect('POST','/api/reports',body,token,status=403)
        report = self.expect('POST','/api/reports',body,ot)
        self.expect('POST',f"/api/admin/reports/{report['id']}/review", dict(action='RESTRICT', note='Reviewed evidence',restriction_hours=2), self.admin)
        self.expect('POST','/api/rides',{},mt,status=403)
        self.expect('GET','/api/me/history',token=mt)
        self.expect('POST',f'/api/admin/users/{member}/restore',{'reason':'Restriction lifted'},self.admin)
        self.assertEqual(self.expect('GET','/api/me',token=mt)['restriction_until'],0)

    def test_location_impact(self):
        owner, token = self.account()
        self.create(token)
        locations = self.expect('GET','/api/locations',token=token)
        self.assertIsInstance(locations[0]['aliases'], list)
        self.expect('PATCH','/api/admin/locations/1',dict(active=False,confirm_affected=False),self.admin,status=409)
        self.assertGreater(self.expect('GET','/api/admin/locations/1/impact',token=self.admin)['affected_rides'],0)

    def test_z_restart_and_backup(self):
        uid, token = self.account()
        self.server.terminate()
        self.server.wait(timeout=10)
        self.start_server()
        self.assertEqual(self.expect('GET','/api/me',token=token)['id'], uid)
        backup = self.folder / 'backup.db'
        subprocess.run([BINARY,'--backup',backup], env=self.env, check=True, capture_output=True)
        with sqlite3.connect(backup) as connection:
            self.assertEqual(connection.execute('PRAGMA integrity_check').fetchone()[0], 'ok')
            stored = connection.execute('SELECT password_hash FROM users WHERE id=?',(uid,)).fetchone()[0]
            self.assertNotIn(PASSWORD,stored)


if __name__ == '__main__':
    unittest.main()
