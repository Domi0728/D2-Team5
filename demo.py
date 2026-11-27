import argparse
import concurrent.futures
import functools
import http.server
import json
import platform
from pathlib import Path
import sqlite3
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from test_api import ApiTests


def run_demo():
    steps = []
    ApiTests.setUpClass()
    t = ApiTests()
    t.setUp()
    def record(title, value):
        steps.append({'title': title, 'response': value})
    try:
        owner, ot = t.account()
        member, mt = t.account()
        record('注册与管理员审核', {'organiser_id': owner, 'passenger_id': member, 'verification': 'APPROVED'})
        ride = t.create(ot, capacity=2)
        rid = ride['id']
        record('发布拼车 · 等待同伴', ride)
        matches = t.expect('GET', f"/api/matches?origin_id=1&destination_id=2&departure_at={ride['departure_at']}", token=mt)
        t.assertIn(rid, [x['id'] for x in matches])
        record('按路线与时间匹配', matches)
        invitation = t.invite(ot, ride, member)
        record('发送邀请 · 此时不占座', invitation)
        ride = t.accept(mt, invitation, ride)
        record('接受邀请 · 成团并分配座位', ride)
        fields = ('origin_id', 'destination_id', 'departure_at', 'meeting_point', 'capacity', 'tolerance_minutes', 'luggage_note')
        revised = {k: ride[k] for k in fields}
        revised.update(expected_version=ride['version'], meeting_point='Library entrance')
        ride = t.expect('PATCH', f'/api/rides/{rid}', revised, ot)
        record('修改集合点 · 等待重新确认', ride)
        ride = t.action(mt, ride, 'reconfirm')
        record('乘客确认新计划', ride)
        t.set_clock(ride['departure_at'])
        ride = t.action(ot, ride, 'start')
        record('到达出发时间 · 发起人开始行程', ride)
        ride = t.action(mt, ride, 'arrive')
        record('乘客确认到达 · 等待其他成员', ride)
        ride = t.action(ot, ride, 'arrive')
        t.assertEqual(ride['status'], 'COMPLETED')
        record('所有成员到达 · 行程结束', ride)
        history = t.expect('GET', '/api/me/history', token=mt)
        record('历史记录与通知', {'history': history, 'notifications': t.expect('GET', '/api/notifications', token=mt)})
        return {'ok': True, 'mode': 'Real C HTTP backend; isolated demo database and simulated clock', 'steps': steps}
    finally:
        ApiTests.tearDownClass()


def run_load():
    ApiTests.setUpClass()
    t = ApiTests(); t.setUp()
    try:
        uid, token = t.account()
        for i in range(30):
            _, owner_token = t.account()
            t.create(owner_token, departure=t.base_time+3600+i*10)
        path = f'/api/matches?origin_id=1&destination_id=2&departure_at={t.base_time+3600}'
        def request(_):
            start = time.perf_counter()
            code, data = t.call('GET', path, token=token)
            return (time.perf_counter()-start)*1000, code == 200 and len(data) == 30
        start = time.perf_counter()
        with concurrent.futures.ThreadPoolExecutor(8) as pool:
            results = list(pool.map(request, range(400)))
        elapsed = time.perf_counter()-start
        times = sorted(x[0] for x in results)
        result = {'requests': 400, 'clients': 8, 'candidate_rides': 30,
                  'successful': sum(x[1] for x in results), 'elapsed_seconds': round(elapsed, 3),
                  'requests_per_second': round(400/elapsed, 2), 'p50_ms': round(times[199], 2),
                  'p95_ms': round(times[379], 2), 'p99_ms': round(times[395], 2), 'max_ms': round(times[-1], 2),
                  'environment': platform.platform(), 'scope': 'Local loopback matching reads, single server, no TLS; not production capacity'}
        if result['successful'] != 400:
            raise AssertionError(result)
        return result
    finally:
        ApiTests.tearDownClass()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--serve', action='store_true')
    parser.add_argument('--load', action='store_true')
    args = parser.parse_args()
    if args.serve:
        lock = threading.Lock()
        class Handler(http.server.SimpleHTTPRequestHandler):
            def do_POST(self):
                if self.path != '/run' or self.headers.get('X-Demo-Request') != '1':
                    self.send_error(403); return
                origin = self.headers.get('Origin', '')
                if origin and origin not in ('http://127.0.0.1:5173', 'http://localhost:5173'):
                    self.send_error(403); return
                if not lock.acquire(blocking=False):
                    self.send_error(409, 'Demo already running'); return
                try:
                    try:
                        result = run_demo(); status = 200
                    except Exception as exc:
                        result = {'ok': False, 'error': str(exc)}; status = 500
                    body = json.dumps(result, ensure_ascii=False).encode()
                    self.send_response(status);self.send_header('Content-Type', 'application/json; charset=utf-8')
                    self.send_header('Content-Length', str(len(body)));self.end_headers();self.wfile.write(body)
                finally:
                    lock.release()
        print('Open http://127.0.0.1:5173 . Ctrl+C stops the demo.', flush=True)
        http.server.ThreadingHTTPServer(('127.0.0.1', 5173), functools.partial(Handler, directory=str(ROOT/'demo'))).serve_forever()
    else:
        result = run_load() if args.load else run_demo()
        name = 'LoadResults_Week6.json' if args.load else 'DemoResults_Week6.json'
        (ROOT/'docs'/name).write_text(json.dumps(result, ensure_ascii=False, indent=2))
        print(json.dumps(result, ensure_ascii=False, indent=2))
