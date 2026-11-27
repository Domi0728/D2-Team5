import concurrent.futures
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import threading
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
        cls.account_counter = 0

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
        type(self).account_counter += 1
        student = f'{self.account_counter:010d}'
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
        loser = next(result for code, result in results if code == 409)
        self.assertEqual(loser['error']['code'], 'GROUP_FULL')
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

    def test_week4_reject_and_withdraw_invitation(self):
        owner, ot = self.account()
        member, mt = self.account()
        ride = self.create(ot)
        inv = self.invite(ot, ride, member)
        self.expect('POST', f"/api/invitations/{inv['id']}/respond",
                    dict(action='reject', expected_version=ride['version']), ot, status=403)
        result = self.expect('POST', f"/api/invitations/{inv['id']}/respond",
                             dict(action='reject', expected_version=ride['version']), mt)
        self.assertEqual(result['status'], 'REJECTED')
        self.assertEqual(result['ride']['member_count'], 1)
        inv = self.invite(ot, ride, member)
        result = self.expect('POST', f"/api/invitations/{inv['id']}/respond",
                             dict(action='withdraw', expected_version=ride['version']), ot)
        self.assertEqual(result['status'], 'WITHDRAWN')
        self.assertEqual(result['ride']['available_seats'], 2)

    def test_week4_restricted_organiser_cannot_add_member(self):
        owner, ot, member, mt, ride = self.group()
        candidate, ct = self.account()
        inv = self.invite(ot, ride, candidate)
        report = self.expect('POST', '/api/reports', dict(ride_id=ride['id'], target_user_id=owner,
                             category='BEHAVIOUR', description='Incident description'), mt)
        self.expect('POST', f"/api/admin/reports/{report['id']}/review",
                    dict(action='RESTRICT', note='Restriction after review', restriction_hours=2), self.admin)
        self.expect('POST', f"/api/invitations/{inv['id']}/respond",
                    dict(action='accept', expected_version=ride['version']), ct, status=403)
        current = self.ride(mt, ride['id'])
        self.assertEqual(current['member_count'], 2)
        self.assertEqual(current['available_seats'], 1)
        invitation = next(x for x in self.expect('GET', '/api/invitations', token=ct) if x['id']==inv['id'])
        self.assertEqual(invitation['status'], 'PENDING')

    def test_week4_duplicate_arrival(self):
        owner, ot, member, mt, ride = self.group()
        self.set_clock(ride['departure_at'])
        ride = self.action(ot, ride, 'start')
        ride = self.action(mt, ride, 'arrive')
        notifications = self.expect('GET', '/api/notifications', token=ot)
        repeated = self.action(mt, ride, 'arrive')
        self.assertEqual(repeated, ride)
        self.assertEqual(self.expect('GET', '/api/notifications', token=ot), notifications)
        ride = self.action(ot, ride, 'arrive')
        self.assertEqual(ride['status'], 'COMPLETED')
        self.assertEqual(self.action(mt, ride, 'arrive')['version'], ride['version'])

    def test_week4_material_edit_invalidates_invitation(self):
        owner, ot, member, mt, ride = self.group()
        candidate, ct = self.account()
        inv = self.invite(ot, ride, candidate)
        data = {k: ride[k] for k in ('origin_id','destination_id','departure_at','meeting_point','capacity','tolerance_minutes','luggage_note')}
        data.update(expected_version=ride['version'], meeting_point='East gate')
        ride = self.expect('PATCH', f"/api/rides/{ride['id']}", data, ot)
        self.assertEqual(ride['status'], 'RECONFIRMATION_REQUIRED')
        self.expect('POST', f"/api/invitations/{inv['id']}/respond",
                    dict(action='accept', expected_version=ride['version']), ct, status=409)
        self.assertEqual(self.action(ot, ride, 'reconfirm')['version'], ride['version'])
        ride = self.action(mt, ride, 'reconfirm')
        self.assertEqual(ride['status'], 'CONFIRMED')

    def test_week4_only_organiser_starts_and_cancels(self):
        owner, ot, member, mt, ride = self.group()
        stranger, st = self.account()
        for token, action in [(mt, 'cancel'), (ot, 'withdraw'), (st, 'withdraw'), (st, 'reconfirm')]:
            self.expect('POST', f"/api/rides/{ride['id']}/{action}",
                        dict(expected_version=ride['version']), token, status=403)
        self.set_clock(ride['departure_at'])
        self.expect('POST', f"/api/rides/{ride['id']}/start",
                    dict(expected_version=ride['version']), mt, status=403)
        ride = self.action(ot, ride, 'start')
        self.expect('POST', f"/api/rides/{ride['id']}/arrive",
                    dict(expected_version=ride['version']), st, status=403)
        self.expect('POST', f"/api/rides/{ride['id']}/cancel",
                    dict(expected_version=ride['version']), ot, status=409)

    def test_week4_confirmed_ride_expires_after_grace(self):
        owner, ot, member, mt, ride = self.group()
        self.set_clock(ride['departure_at'] + 30*60)
        ride = self.ride(ot, ride['id'])
        self.assertEqual(ride['status'], 'EXPIRED')
        self.expect('POST', f"/api/rides/{ride['id']}/start",
                    dict(expected_version=ride['version']), ot, status=409)

    def test_week4_cancel_invalidates_pending_invitation(self):
        owner, ot = self.account()
        candidate, ct = self.account()
        ride = self.create(ot)
        inv = self.invite(ot, ride, candidate)
        ride = self.action(ot, ride, 'cancel')
        self.expect('POST', f"/api/invitations/{inv['id']}/respond",
                    dict(action='accept', expected_version=ride['version']), ct, status=409)
        self.assertEqual(self.ride(ot, ride['id'])['member_count'], 1)

    def test_week5_notification_read_all_snapshot_and_retry(self):
        uid, token = self.account()
        other, other_token = self.account()
        initial = self.expect('GET', '/api/notifications?read=unread', token=token)
        through = max(x['id'] for x in initial['items'])
        other_before = self.expect('GET', '/api/notifications', token=other_token)
        self.expect('POST', f'/api/admin/users/{uid}/verify',
                    dict(decision='APPROVED', reason='Checked again'), self.admin)
        key = str(uuid.uuid4())
        result = self.expect('POST', '/api/notifications/read-all', {'through_id': through}, token, key=key)
        self.assertEqual(result['updated_count'], len(initial['items']))
        self.assertEqual(result, self.expect('POST', '/api/notifications/read-all', {'through_id': through}, token, key=key))
        unread = self.expect('GET', '/api/notifications?read=unread', token=token)
        self.assertEqual(unread['unread_count'], 1)
        self.assertTrue(all(x['id'] > through for x in unread['items']))
        self.assertEqual(self.expect('GET', '/api/notifications', token=other_token), other_before)
        read = self.expect('GET', '/api/notifications?read=read&limit=1', token=token)
        self.assertEqual(len(read['items']), 1)
        self.assertEqual(read['items'][0]['is_read'], 1)
        self.expect('GET', '/api/notifications?read=wrong', token=token, status=422)
        self.expect('POST', '/api/notifications/read-all', {'through_id': 0}, token, status=422)
        self.expect('POST', '/api/notifications/read-all', {'through_id': through, 'user_id': other}, token, status=422)

    def test_week5_report_queries_review_dedup_and_restore_notice(self):
        owner, ot, member, mt, ride = self.group()
        stranger, st = self.account()
        report = self.expect('POST', '/api/reports', dict(ride_id=ride['id'], target_user_id=member,
                            category='LATE', description='Arrived late at the meeting point'), ot)
        rid = report['id']
        self.assertEqual(self.expect('GET', f'/api/reports/{rid}', token=ot)['id'], rid)
        self.expect('GET', f'/api/reports/{rid}', token=mt, status=404)
        self.expect('GET', f'/api/reports/{rid}', token=st, status=404)
        self.expect('GET', '/api/admin/reports?status=PENDING', token=ot, status=403)
        pending = self.expect('GET', '/api/reports?status=PENDING&limit=1', token=ot)
        self.assertEqual([x['id'] for x in pending], [rid])
        self.expect('GET', '/api/reports?status=wrong', token=ot, status=422)
        self.expect('GET', '/api/admin/reports?limit=-1', token=self.admin, status=422)
        body = dict(action='RESTRICT', note='Evidence reviewed', restriction_hours=1)
        key = str(uuid.uuid4())
        path = f'/api/admin/reports/{rid}/review'
        self.expect('POST', path, body, self.admin, key=key)
        notices = self.expect('GET', '/api/notifications', token=mt)
        self.expect('POST', path, body, self.admin, key=key)
        self.expect('POST', path, body, self.admin, status=409)
        self.assertEqual(notices, self.expect('GET', '/api/notifications', token=mt))
        self.assertEqual(self.expect('GET', '/api/reports?status=PENDING', token=ot), [])
        reviewed = self.expect('GET', '/api/reports?status=REVIEWED', token=ot)
        self.assertEqual(reviewed[0]['decision'], 'RESTRICT')
        audit = self.expect('GET', '/api/admin/audit?action=review_report&limit=200', token=self.admin)
        self.assertEqual(sum(x['entity_id']==rid for x in audit), 1)
        self.expect('GET', '/api/admin/audit', token=ot, status=403)
        key = str(uuid.uuid4())
        self.expect('POST', f'/api/admin/users/{member}/restore', {'reason':'Review complete'}, self.admin, key=key)
        self.expect('POST', f'/api/admin/users/{member}/restore', {'reason':'Review complete'}, self.admin, key=key)
        messages = self.expect('GET', '/api/notifications', token=mt)['items']
        self.assertEqual(sum(x['content']=='Your account restrictions have been lifted' for x in messages), 1)

    def test_week5_contact_revocation_and_withdrawal(self):
        owner, ot, member, mt, ride = self.group()
        stranger, st = self.account()
        settings = dict(nickname='Member', phone='+853 12345678', contact_share=True, history_visible=False)
        self.expect('PATCH', '/api/me', settings, mt)
        public = self.expect('GET', f'/api/users/{member}', token=st)
        for field in ['phone', 'real_name', 'student_id', 'password_hash', 'restriction_until']:
            self.assertNotIn(field, public)
        self.assertIn('phone', self.expect('GET', f'/api/users/{member}', token=ot))
        self.expect('PATCH', '/api/me', dict(settings, contact_share=False), mt)
        self.assertNotIn('phone', self.expect('GET', f'/api/users/{member}', token=ot))
        self.expect('PATCH', '/api/me', settings, mt)
        self.action(mt, ride, 'withdraw')
        self.assertNotIn('phone', self.expect('GET', f'/api/users/{member}', token=ot))

    def test_week5_history_visibility_and_route_reuse(self):
        owner, ot, member, mt, ride = self.group()
        stranger, st = self.account()
        self.set_clock(ride['departure_at'])
        ride = self.action(ot, ride, 'start')
        ride = self.action(mt, ride, 'arrive')
        ride = self.action(ot, ride, 'arrive')
        settings = dict(nickname='Member', phone='', contact_share=False, history_visible=True)
        self.expect('PATCH', '/api/me', settings, mt)
        history = self.expect('GET', f'/api/users/{member}/history', token=st)
        self.assertEqual(len(history), 1)
        self.assertEqual(set(history[0]), {'origin_name','destination_name','departure_at','status'})
        self.expect('PATCH', '/api/me', dict(settings, history_visible=False), mt)
        self.expect('GET', f'/api/users/{member}/history', token=st, status=404)
        self.assertTrue(self.expect('GET', '/api/me/history', token=mt))
        self.expect('POST', f"/api/rides/{ride['id']}/reuse", {}, st, status=403)
        draft = self.expect('POST', f"/api/rides/{ride['id']}/reuse", {}, mt)
        self.assertNotIn('departure_at', draft)
        self.assertNotIn('members', draft)
        fresh = self.create(mt, departure=ride['departure_at']+7200,
                            origin_id=draft['origin_id'], destination_id=draft['destination_id'])
        self.assertNotEqual(fresh['id'], ride['id'])
        self.assertEqual(fresh['member_count'], 1)
        self.assertEqual(self.ride(ot, ride['id'])['status'], 'COMPLETED')

    def test_week5_pending_rejected_and_expired_session(self):
        uid, token = self.account(False)
        target, tt = self.account()
        self.expect('GET', f'/api/users/{target}', token=token, status=403)
        self.expect('POST', f'/api/admin/users/{uid}/verify', dict(decision='REJECTED', reason='Invalid enrollment'), self.admin)
        self.expect('POST', '/api/rides', {}, token, status=403)
        self.expect('GET', '/api/me', token=token)
        self.expect('POST', '/api/logout', {}, token)
        self.expect('GET', '/api/me', token=token, status=401)
        try:
            self.set_clock(self.base_time+28800)
            self.expect('GET', '/api/me', token=tt, status=401)
        finally:
            self.set_clock(self.base_time)
            type(self).admin = self.login('admin')

    def test_week5_suspension_blocks_private_access(self):
        owner, ot, member, mt, ride = self.group()
        self.expect('PATCH', '/api/me', dict(nickname='Owner', phone='12345678', contact_share=True, history_visible=False), ot)
        report = self.expect('POST', '/api/reports', dict(ride_id=ride['id'], target_user_id=member,
                            category='OTHER', description='Submitted incident evidence'), ot)
        self.expect('POST', f"/api/admin/reports/{report['id']}/review", dict(action='SUSPEND', note='Reviewed incident'), self.admin)
        self.expect('GET', f'/api/users/{owner}', token=mt, status=403)
        self.expect('POST', '/api/rides', {}, mt, status=403)
        self.expect('GET', '/api/me', token=mt)
        self.expect('GET', '/api/notifications', token=mt)
        self.expect('POST', f'/api/admin/users/{member}/restore', {'reason':'Issue resolved'}, self.admin)
        self.assertIn('phone', self.expect('GET', f'/api/users/{owner}', token=mt))

    def test_week5_moderation_transaction_rollback(self):
        owner, ot, member, mt, ride = self.group()
        report = self.expect('POST', '/api/reports', dict(ride_id=ride['id'], target_user_id=member,
                            category='OTHER', description='Evidence for transaction test'), ot)
        before = self.expect('GET', '/api/notifications', token=mt)
        with sqlite3.connect(self.folder/'test.db') as conn:
            conn.execute("CREATE TRIGGER fail_review_audit BEFORE INSERT ON audit_logs WHEN NEW.action='review_report' BEGIN SELECT RAISE(ABORT, 'test failure'); END")
        try:
            self.expect('POST', f"/api/admin/reports/{report['id']}/review", dict(action='SUSPEND', note='Test review'), self.admin, status=503)
            self.assertEqual(self.expect('GET', '/api/me', token=mt)['suspended'], 0)
            self.assertEqual(self.expect('GET', f"/api/reports/{report['id']}", token=ot)['status'], 'PENDING')
            self.assertEqual(before, self.expect('GET', '/api/notifications', token=mt))
        finally:
            with sqlite3.connect(self.folder/'test.db') as conn:
                conn.execute('DROP TRIGGER fail_review_audit')

    def test_week5_retention_preview_and_pending_report(self):
        owner, ot, member, mt, protected = self.group()
        report = self.expect('POST', '/api/reports', dict(ride_id=protected['id'], target_user_id=member,
                            category='OTHER', description='Keep evidence until review'), ot)
        self.action(ot, protected, 'cancel')
        free = self.create(ot, departure=self.base_time+7200)
        self.action(ot, free, 'cancel')
        old = self.base_time-100*86400
        with sqlite3.connect(self.folder/'test.db') as conn:
            conn.execute('UPDATE rides SET closed_at=? WHERE id IN (?,?)', (old,protected['id'],free['id']))
        self.expect('GET', '/api/admin/maintenance/preview', token=ot, status=403)
        preview = self.expect('GET', '/api/admin/maintenance/preview', token=self.admin)
        self.assertEqual(preview['eligible_rides'], 1)
        self.assertEqual(self.ride(ot,free['id'])['id'], free['id'])
        result = self.expect('POST', '/api/admin/maintenance', {}, self.admin)
        self.assertEqual(result['deleted_rides'], preview['eligible_rides'])
        self.expect('GET', f"/api/rides/{free['id']}", token=ot, status=404)
        self.assertEqual(self.ride(ot,protected['id'])['id'],protected['id'])
        self.expect('POST', f"/api/admin/reports/{report['id']}/review", dict(action='DISMISS', note='Review closed'), self.admin)
        with sqlite3.connect(self.folder/'test.db') as conn:
            conn.execute('UPDATE reports SET reviewed_at=? WHERE id=?',(old,report['id']))
        preview = self.expect('GET', '/api/admin/maintenance/preview', token=self.admin)
        self.assertEqual(preview['eligible_rides'],1)
        result = self.expect('POST', '/api/admin/maintenance', {}, self.admin)
        self.assertEqual(result['deleted_rides'],1)
        self.assertEqual(result['deleted_reports'],1)
        self.assertEqual(self.expect('POST', '/api/admin/maintenance', {}, self.admin)['deleted_rides'],0)
        with sqlite3.connect(self.folder/'test.db') as conn:
            self.assertEqual(conn.execute('PRAGMA foreign_key_check').fetchall(), [])

    def test_week5_admin_config_audit_and_permissions(self):
        uid, token = self.account()
        self.expect('GET', '/api/admin/config', token=token, status=403)
        rows = self.expect('GET', '/api/admin/config', token=self.admin)
        settings = {x['key']:x['value'] for x in rows}
        self.expect('PATCH', '/api/admin/config', dict(settings, retention_days=0), self.admin, status=422)
        self.assertEqual(rows,self.expect('GET', '/api/admin/config', token=self.admin))
        self.expect('PATCH', '/api/admin/config', settings, self.admin)
        audit = self.expect('GET', '/api/admin/audit?action=configure_system&limit=1', token=self.admin)
        self.assertEqual(len(audit),1)
        self.assertEqual(json.loads(audit[0]['details']),settings)
        actor = audit[0]['actor_id']
        self.assertTrue(all(x['actor_id']==actor for x in self.expect('GET', f'/api/admin/audit?actor_id={actor}&limit=2', token=self.admin)))
        self.expect('GET', '/api/admin/audit?offset=-1', token=self.admin, status=422)

    def match_ids(self, token, departure, **options):
        from urllib.parse import urlencode
        params = dict(origin_id=1, destination_id=2, departure_at=departure, tolerance_minutes=15)
        params.update(options)
        rows = self.expect('GET', '/api/matches?' + urlencode(params), token=token)
        return [r['id'] for r in rows]

    def test_stage2_match_window_and_macau_date(self):
        seeker, st = self.account()
        day = ((self.base_time+28800)//86400+3)*86400-28800
        noon = day+12*3600
        owners = [self.account() for _ in range(5)]
        inside = self.create(owners[0][1], departure=noon+900)
        outside = self.create(owners[1][1], departure=noon+960)
        reverse = self.create(owners[2][1], departure=noon, origin_id=2, destination_id=1)
        prior = self.create(owners[3][1], departure=day+86400-300)
        next_day = self.create(owners[4][1], departure=day+86400+300)
        ids = self.match_ids(st,noon)
        self.assertIn(inside['id'],ids)
        self.assertNotIn(outside['id'],ids)
        self.assertNotIn(reverse['id'],ids)
        self.assertNotIn(inside['id'],self.match_ids(st,noon,tolerance_minutes=14))
        ids = self.match_ids(st,day+86400+300)
        self.assertIn(next_day['id'],ids)
        self.assertNotIn(prior['id'],ids)
        self.assertNotIn(next_day['id'],self.match_ids(st,day+300,tolerance_minutes=120))

    def test_stage2_match_all_sort_tiebreakers(self):
        seeker, st = self.account()
        owners = [self.account() for _ in range(5)]
        departure = self.base_time+10*86400
        rides = [self.create(t, departure=departure+(0 if i==0 else 60)) for i,(_,t) in enumerate(owners)]
        with sqlite3.connect(self.folder/'test.db') as conn:
            for i,ride in enumerate(rides):
                conn.execute('UPDATE rides SET created_at=? WHERE id=?',
                             (self.base_time+(20 if i==1 else 0),ride['id']))
            for i in [0,1,2,3]:
                count = 2 if i==1 else 1 if i in (2,3) else 0
                for j in range(count):
                    cur = conn.execute("INSERT INTO rides(organiser_id,origin_id,destination_id,origin_name,destination_name,departure_at,meeting_point,capacity,status,created_at,updated_at,closed_at) VALUES(?,1,2,'Campus','Port',?,'Gate',3,'COMPLETED',?,?,?)",
                                       (owners[i][0],self.base_time-86400,self.base_time-86400,self.base_time-3600,self.base_time-3600))
                    hid = cur.lastrowid
                    for uid,kind in [(owners[i][0],'ORGANISER'),(seeker,'MEMBER')]:
                        conn.execute('INSERT INTO memberships(ride_id,user_id,member_type,consent_version,arrival_flag,joined_at) VALUES(?,?,?,1,1,?)', (hid,uid,kind,self.base_time-86400))
            conn.execute('UPDATE rides SET created_at=? WHERE id IN (?,?)',(self.base_time+10,rides[2]['id'],rides[3]['id']))
        expected = [x['id'] for x in rides]
        self.assertEqual(self.match_ids(st,departure),expected)
        self.assertEqual(self.match_ids(st,departure,limit=2,offset=2),expected[2:4])
        self.assertEqual(self.match_ids(st,departure),expected)
        # Publish time precedes ID when the first two ranking keys tie.
        with sqlite3.connect(self.folder/'test.db') as conn:
            conn.execute('UPDATE rides SET created_at=? WHERE id=?',(self.base_time-1,rides[3]['id']))
        self.assertEqual(self.match_ids(st,departure),[expected[0],expected[1],expected[3],expected[2],expected[4]])
        self.assertEqual(self.match_ids(st,departure,sort='departure'),expected)
        self.expect('GET', f'/api/matches?origin_id=1&destination_id=2&departure_at={departure}&sort=unknown',token=st,status=422)

    def test_stage2_match_capacity_and_group_state(self):
        seeker, st = self.account()
        owner, ot, member, mt, ride = self.group(capacity=2)
        self.assertNotIn(ride['id'],self.match_ids(st,ride['departure_at']))
        ride = self.action(mt,ride,'withdraw')
        self.assertIn(ride['id'],self.match_ids(st,ride['departure_at']))
        self.assertNotIn(ride['id'],self.match_ids(st,ride['departure_at'],min_seats=2))
        ride = self.accept(mt,self.invite(ot,ride,member),ride)
        fields = {k:ride[k] for k in ('origin_id','destination_id','departure_at','meeting_point','capacity','tolerance_minutes','luggage_note')}
        fields.update(expected_version=ride['version'],capacity=3,meeting_point='Changed gate')
        ride = self.expect('PATCH',f"/api/rides/{ride['id']}",fields,ot)
        self.assertEqual(ride['status'],'RECONFIRMATION_REQUIRED')
        self.assertNotIn(ride['id'],self.match_ids(st,ride['departure_at']))
        ride = self.action(mt,ride,'reconfirm')
        self.assertIn(ride['id'],self.match_ids(st,ride['departure_at']))
        self.action(ot,ride,'cancel')
        self.assertNotIn(ride['id'],self.match_ids(st,ride['departure_at']))

    def test_stage2_match_excludes_ineligible_organisers(self):
        seeker, st = self.account()
        owner, ot, member, mt, ride = self.group()
        report = self.expect('POST','/api/reports',dict(ride_id=ride['id'],target_user_id=owner,category='OTHER',description='Incident evidence'),mt)
        self.expect('POST',f"/api/admin/reports/{report['id']}/review",dict(action='RESTRICT',note='Reviewed',restriction_hours=1),self.admin)
        self.assertNotIn(ride['id'],self.match_ids(st,ride['departure_at']))
        self.expect('POST',f'/api/admin/users/{owner}/restore',{'reason':'Restriction removed'},self.admin)
        self.assertIn(ride['id'],self.match_ids(st,ride['departure_at']))
        self.expect('POST',f'/api/admin/users/{owner}/verify',dict(decision='REJECTED',reason='Verification withdrawn'),self.admin)
        self.assertNotIn(ride['id'],self.match_ids(st,ride['departure_at']))
        self.expect('POST',f'/api/admin/users/{owner}/verify',dict(decision='APPROVED',reason='Verified again'),self.admin)
        report = self.expect('POST','/api/reports',dict(ride_id=ride['id'],target_user_id=owner,category='LATE',description='Another incident'),mt)
        self.expect('POST',f"/api/admin/reports/{report['id']}/review",dict(action='SUSPEND',note='Reviewed'),self.admin)
        self.assertNotIn(ride['id'],self.match_ids(st,ride['departure_at']))

    def acceptance_counts(self, rid, iid, member, owner):
        with sqlite3.connect(self.folder / 'test.db') as db:
            self.assertEqual(db.execute('SELECT count(*) FROM memberships WHERE ride_id=? AND user_id=? AND active=1', (rid, member)).fetchone()[0], 1)
            self.assertEqual(db.execute('SELECT count(*) FROM memberships WHERE ride_id=? AND active=1', (rid,)).fetchone()[0], 2)
            self.assertEqual(db.execute('SELECT user_id FROM notifications WHERE event_key=? ORDER BY user_id', (f'invite:{iid}:ACCEPTED',)).fetchall(), [(x,) for x in sorted([member, owner])])
            self.assertEqual(db.execute("SELECT count(*) FROM audit_logs WHERE action='invitation_decision' AND entity_id=?", (iid,)).fetchone()[0], 1)

    def test_stage2_same_key_concurrent_accept(self):
        owner, ot = self.account()
        member, mt = self.account()
        ride = self.create(ot, 2)
        invitation = self.invite(ot, ride, member)
        key = str(uuid.uuid4())
        barrier = threading.Barrier(8)
        path = f"/api/invitations/{invitation['id']}/respond"
        body = {'action': 'accept', 'expected_version': ride['version']}
        def submit(_):
            barrier.wait(timeout=10)
            return self.call('POST', path, body, mt, key=key)
        with concurrent.futures.ThreadPoolExecutor(8) as pool:
            results = list(pool.map(submit, range(8)))
        self.assertTrue(all(code == 200 for code, _ in results), results)
        self.assertTrue(all(result == results[0][1] for _, result in results))
        self.acceptance_counts(ride['id'], invitation['id'], member, owner)
        self.assertEqual(self.ride(ot, ride['id'])['available_seats'], 0)
        self.expect('POST', path, dict(body, action='reject'), mt, status=409, key=key)
        self.acceptance_counts(ride['id'], invitation['id'], member, owner)

    def test_stage2_distinct_keys_concurrent_accept(self):
        owner, ot = self.account()
        member, mt = self.account()
        ride = self.create(ot, 2)
        invitation = self.invite(ot, ride, member)
        barrier = threading.Barrier(8)
        def submit(_):
            barrier.wait(timeout=10)
            return self.call('POST', f"/api/invitations/{invitation['id']}/respond",
                             {'action': 'accept', 'expected_version': ride['version']}, mt)
        with concurrent.futures.ThreadPoolExecutor(8) as pool:
            results = list(pool.map(submit, range(8)))
        self.assertEqual(sorted(code for code, _ in results), [200] + [409] * 7)
        self.acceptance_counts(ride['id'], invitation['id'], member, owner)

    def test_stage2_accept_retry_after_restart(self):
        owner, ot = self.account()
        member, mt = self.account()
        ride = self.create(ot, 2)
        invitation = self.invite(ot, ride, member)
        key = str(uuid.uuid4())
        path = f"/api/invitations/{invitation['id']}/respond"
        body = {'action': 'accept', 'expected_version': ride['version']}
        committed = self.expect('POST', path, body, mt, key=key)
        self.server.terminate()
        self.server.wait(timeout=10)
        type(self).start_server()
        replay = self.expect('POST', path, body, mt, key=key)
        self.assertEqual(committed, replay)
        self.acceptance_counts(ride['id'], invitation['id'], member, owner)
        outsider, xt = self.account()
        self.expect('POST', path, body, xt, status=403, key=key)

    def test_stage2_accept_cache_failure_rolls_back(self):
        owner, ot = self.account()
        member, mt = self.account()
        ride = self.create(ot, 2)
        invitation = self.invite(ot, ride, member)
        key = str(uuid.uuid4())
        path = f"/api/invitations/{invitation['id']}/respond"
        body = {'action': 'accept', 'expected_version': ride['version']}
        with sqlite3.connect(self.folder / 'test.db') as db:
            db.execute("CREATE TRIGGER fail_accept_cache BEFORE INSERT ON idempotency BEGIN SELECT RAISE(ABORT, 'injected cache failure'); END")
        try:
            self.expect('POST', path, body, mt, key=key, status=503)
            with sqlite3.connect(self.folder / 'test.db') as db:
                self.assertEqual(db.execute('SELECT status FROM invitations WHERE id=?', (invitation['id'],)).fetchone()[0], 'PENDING')
                self.assertEqual(db.execute('SELECT count(*) FROM memberships WHERE ride_id=? AND active=1', (ride['id'],)).fetchone()[0], 1)
                self.assertEqual(db.execute('SELECT count(*) FROM notifications WHERE event_key=?', (f"invite:{invitation['id']}:ACCEPTED",)).fetchone()[0], 0)
        finally:
            with sqlite3.connect(self.folder / 'test.db') as db:
                db.execute('DROP TRIGGER fail_accept_cache')
        self.expect('POST', path, body, mt, key=key)
        self.acceptance_counts(ride['id'], invitation['id'], member, owner)

    def test_week6_cancel_after_departure_before_start(self):
        owner, ot, member, mt, ride = self.group()
        self.set_clock(ride['departure_at'] + 1)
        result = self.action(ot, ride, 'cancel', reason='No departure yet')
        self.assertEqual(result['status'], 'CANCELLED')
        self.assertEqual(result['late_cancellation_warning'], 1)
        with sqlite3.connect(self.folder / 'test.db') as db:
            rows = db.execute("SELECT user_id FROM notifications WHERE ride_id=? AND content='Ride cancelled'", (ride['id'],)).fetchall()
        self.assertEqual(sorted(x[0] for x in rows), sorted([owner, member]))

    def test_week6_withdraw_warning_boundary_and_retry(self):
        for seconds, warning in [(601, 0), (600, 1), (0, 1), (-1, 1)]:
            with self.subTest(seconds=seconds):
                self.set_clock(self.base_time)
                owner, ot, member, mt, ride = self.group()
                self.set_clock(ride['departure_at'] - seconds)
                body = {'expected_version': ride['version'], 'reason': 'Plans changed'}
                key = str(uuid.uuid4())
                path = f"/api/rides/{ride['id']}/withdraw"
                result = self.expect('POST', path, body, mt, key=key)
                self.assertEqual(result['late_cancellation_warning'], warning)
                self.assertEqual(result['member_count'], 1)
                self.assertEqual(result['available_seats'], ride['capacity'] - 1)
                self.assertEqual(result, self.expect('POST', path, body, mt, key=key))
                with sqlite3.connect(self.folder / 'test.db') as db:
                    self.assertEqual(db.execute("SELECT count(*) FROM notifications WHERE ride_id=? AND user_id=? AND content='A member withdrew'", (ride['id'], owner)).fetchone()[0], 1)

    def test_week6_started_ride_cannot_cancel_or_withdraw(self):
        owner, ot, member, mt, ride = self.group()
        self.set_clock(ride['departure_at'])
        ride = self.action(ot, ride, 'start')
        for token, action in [(ot, 'cancel'), (mt, 'withdraw')]:
            self.expect('POST', f"/api/rides/{ride['id']}/{action}", {'expected_version': ride['version']}, token, status=409)
        self.assertEqual(self.ride(ot, ride['id'])['member_count'], 2)

    def test_week6_backup_restore_http(self):
        owner, ot, member, mt, ride = self.group()
        backup = self.folder / 'restore-drill.db'
        subprocess.run([BINARY, '--backup', str(backup)], env=self.env, check=True, capture_output=True)
        self.server.terminate()
        self.server.wait(timeout=10)
        original = self.env['RIDE_DB']
        self.env['RIDE_DB'] = str(backup)
        try:
            type(self).start_server()
            restored = self.ride(ot, ride['id'])
            self.assertEqual(restored, ride)
            self.assertEqual(self.expect('GET', '/api/me', token=mt)['id'], member)
            self.assertTrue(self.expect('GET', '/api/notifications', token=mt)['items'])
            with sqlite3.connect(backup) as db:
                self.assertEqual(db.execute('PRAGMA integrity_check').fetchone()[0], 'ok')
        finally:
            self.server.terminate()
            self.server.wait(timeout=10)
            self.env['RIDE_DB'] = original
            type(self).start_server()

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
