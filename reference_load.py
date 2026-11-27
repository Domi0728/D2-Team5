import concurrent.futures
import json
import platform
import sqlite3
import threading
import time
from demo import ApiTests, ROOT


def stats(values):
    values = sorted(values)
    return {'count': len(values), 'p95_ms': round(values[int((len(values)-1)*.95)], 2),
            'max_ms': round(values[-1], 2), 'within_3_seconds': sum(x <= 3000 for x in values)}


ApiTests.setUpClass()
t = ApiTests(); t.setUp()
try:
    readers = [t.account() for _ in range(20)]
    owners = [t.account() for _ in range(30)]
    for i, (_, token) in enumerate(owners):
        t.create(token, departure=t.base_time+3600+i*10)
    with sqlite3.connect(t.folder / 'test.db') as db:
        for i in range(449):
            db.execute("INSERT INTO users(student_id,real_name,nickname,password_hash,verify_status,created_at) SELECT ?, 'Fixture Student','Fixture',password_hash,'APPROVED',? FROM users WHERE id=?",
                       (f'9{i:09d}', t.base_time, readers[0][0]))
        for i in range(1970):
            departure = t.base_time - (1970-i)*7200
            cursor = db.execute("INSERT INTO rides(organiser_id,origin_id,destination_id,origin_name,destination_name,departure_at,meeting_point,capacity,status,created_at,updated_at,closed_at) VALUES(?,1,2,'Campus','Destination',?,'Fixture',2,'COMPLETED',?,?,?)",
                                (owners[0][0], departure, departure-3600, departure+1800, departure+1800))
            rid = cursor.lastrowid
            for user, role in [(owners[0][0], 'ORGANISER'), (owners[1][0], 'MEMBER')]:
                db.execute('INSERT INTO memberships(ride_id,user_id,member_type,consent_version,arrival_flag,joined_at) VALUES(?,?,?,1,1,?)', (rid,user,role,departure-600))
        assert db.execute('SELECT count(*) FROM users').fetchone()[0] == 500
        assert db.execute('SELECT count(*) FROM rides').fetchone()[0] == 2000
        assert not db.execute('PRAGMA foreign_key_check').fetchall()
    barrier = threading.Barrier(20)
    def read_work(pair):
        barrier.wait(timeout=10)
        timings = []
        for _ in range(25):
            start = time.perf_counter()
            code, result = t.call('GET', f'/api/matches?origin_id=1&destination_id=2&departure_at={t.base_time+3600}', token=pair[1])
            assert code == 200 and len(result) == 30, (code, result)
            timings.append((time.perf_counter()-start)*1000)
        return timings
    start = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(20) as pool:
        reads = sum(pool.map(read_work, readers), [])
    read_elapsed = time.perf_counter()-start
    barrier = threading.Barrier(20)
    def writes(i):
        barrier.wait(timeout=10)
        _, ot = owners[i]; member, mt = readers[i]
        results = {}
        start = time.perf_counter(); ride = t.create(ot, departure=t.base_time+10800); results['publish']=(time.perf_counter()-start)*1000
        body = {k:ride[k] for k in ('origin_id','destination_id','departure_at','meeting_point','capacity','tolerance_minutes','luggage_note')}
        body.update(expected_version=ride['version'], luggage_note='One bag')
        start=time.perf_counter();ride=t.expect('PATCH',f"/api/rides/{ride['id']}",body,ot);results['edit']=(time.perf_counter()-start)*1000
        start=time.perf_counter();inv=t.invite(ot,ride,member);results['invite']=(time.perf_counter()-start)*1000
        start=time.perf_counter();t.accept(mt,inv,ride);results['confirm']=(time.perf_counter()-start)*1000
        for token in [ot, mt]:
            rows=t.expect('GET','/api/notifications',token=token)['items']
            assert any(x['ride_id']==ride['id'] and x['content']=='ACCEPTED' for x in rows)
        results['notification_visible_ms']=(time.perf_counter()-start)*1000
        return results
    with concurrent.futures.ThreadPoolExecutor(20) as pool:
        writes_result=list(pool.map(writes, range(20)))
    report={'accounts':500,'initial_rides':2000,'active_candidates':30,'historical_rides':1970,'concurrent_users':20,
            'matching':stats(reads),'matching_elapsed_seconds':round(read_elapsed,3),
            'core_actions':{name:stats([r[name] for r in writes_result]) for name in ['publish','edit','invite','confirm']},
            'notification_max_ms':round(max(r['notification_visible_ms'] for r in writes_result),2),
            'scope':'Single localhost C process, SQLite, simulated clock, no TLS or real frontend. 449 accounts and 1970 history rides inserted as declared synthetic fixtures; other setup and all measured actions use HTTP.',
            'environment':platform.platform()}
    report['thresholds_passed']=(report['matching']['within_3_seconds']>=475 and all(r['within_3_seconds']>=19 for r in report['core_actions'].values()) and report['notification_max_ms']<=5000)
    (ROOT/'docs/ReferenceLoad_Week6.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))
    assert report['thresholds_passed']
finally:
    ApiTests.log.flush()
    (ROOT/'docs/ReferenceLoad_Server.log').write_text((ApiTests.folder/'server.log').read_text())
    ApiTests.tearDownClass()
