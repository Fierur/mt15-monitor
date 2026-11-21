import time
from unittest.mock import patch

import pytest
from fastapi.testclient import TestClient

from server.app import Config, create_app


@pytest.fixture
def setup(tmp_path):
    cfg = Config(str(tmp_path / 'test.sqlite3'), 'mt15-001', 'test-device-token-0123456789', 'admin', 'test-admin-password', low_battery=11.8)
    app = create_app(cfg, worker=False)
    with TestClient(app) as client:
        yield cfg, app, client


def sample(seq=1, **kw):
    return {'device_id':'mt15-001','boot_id':'boot-test','seq':seq,'uptime_s':10,'mode':'bench',**kw}


def send(setup, payload):
    cfg, _, client = setup
    return client.post('/api/v1/telemetry', json=payload, headers={'Authorization': 'Bearer '+cfg.device_token})


def login(setup):
    cfg, _, client = setup
    r = client.post('/api/v1/login', json={'username':cfg.admin_user,'password':cfg.admin_password})
    assert r.status_code == 200
    return {'X-CSRF-Token':r.json()['csrf']}


def test_auth_and_csrf(setup):
    _, _, c = setup
    assert c.get('/api/v1/status').status_code == 401
    assert c.post('/api/v1/telemetry',json=sample()).status_code == 401
    headers = login(setup)
    assert c.put('/api/v1/settings',json={'armed':True}).status_code == 403
    assert c.put('/api/v1/settings',json={'armed':True},headers=headers).json()['revision']==1
    assert c.put('/api/v1/settings',json={'armed':True},headers=headers).json()['revision']==1
    assert c.post('/api/v1/logout',headers=headers).status_code==200
    assert c.get('/api/v1/status').status_code==401


def test_duplicate_alerts_and_pending_settings(setup):
    _, _, c = setup
    headers=login(setup)
    c.put('/api/v1/settings',json={'armed':True},headers=headers)
    first=send(setup,sample(motion=True,armed=True,battery_v=11.5))
    assert first.json()['settings']=={'armed':True,'revision':1}
    assert send(setup,sample(motion=True,armed=True,battery_v=11.5)).json()['duplicate']
    assert len(c.get('/api/v1/history').json())==1
    # One low sample is not enough for a battery alert (cranking transient).
    assert [a['kind'] for a in c.get('/api/v1/alerts').json()]==['movement']
    assert not c.get('/api/v1/status').json()['config_applied']
    send(setup,sample(2,armed=True,config_revision=1,motion=True,battery_v=11.5))
    assert c.get('/api/v1/status').json()['config_applied']
    # Second consecutive low sample alerts; movement is rate-limited for 5 minutes.
    assert sorted(a['kind'] for a in c.get('/api/v1/alerts').json())==['low_battery','movement']


def test_gps_validation_and_wrong_device(setup):
    assert send(setup,sample(gps_valid=True)).status_code==422
    assert send(setup,sample(latitude=1,longitude=2)).status_code==422
    assert send(setup,sample(gps_valid=True,latitude=91,longitude=2)).status_code==422
    assert send(setup,{**sample(),'device_id':'another'}).status_code==403
    assert send(setup,sample(gps_valid=True,latitude=10.1,longitude=106.1)).status_code==200


def test_replay_does_not_overwrite_current(setup):
    _,_,c=setup
    login(setup)
    send(setup,sample(5,battery_v=12.4))
    send(setup,sample(2,battery_v=11.7,is_replay=True))
    assert c.get('/api/v1/status').json()['telemetry']['seq']==5
    assert len(c.get('/api/v1/history').json())==2
    send(setup,sample(3,battery_v=12.1))
    assert c.get('/api/v1/status').json()['telemetry']['seq']==5


def test_sleep_offline_and_csv(setup):
    _, app, c=setup
    login(setup)
    send(setup,{**sample(), 'mode':'parked','next_report_s':900})
    assert c.get('/api/v1/status').json()['connection']=='sleeping'
    with app.state.store.db() as db:
        db.execute('UPDATE telemetry SET received_at=?',(time.time()-1000,))
    assert c.get('/api/v1/status').json()['connection']=='offline'
    export=c.get('/api/v1/history.csv')
    assert export.status_code==200 and 'battery_v' in export.text
    assert c.get('/api/v1/history?start=10&end=1').status_code==422


def test_email_failure_keeps_alert_then_retry(setup):
    cfg,_,_=setup
    cfg.smtp_host='smtp.example.test';cfg.smtp_from='iot@example.test';cfg.alert_email='owner@example.test'
    app=create_app(cfg,worker=False)
    with TestClient(app) as client:
        client.post('/api/v1/telemetry',json=sample(motion=True,armed=True),headers={'Authorization':'Bearer '+cfg.device_token})
        with patch('server.app.smtplib.SMTP',side_effect=OSError('test failure')):
            app.state.maintenance()
        with app.state.store.db() as db:
            row=db.execute('SELECT * FROM alerts').fetchone()
            assert row['mail_status']=='pending' and row['attempts']==1
            db.execute('UPDATE alerts SET next_attempt=0')
        with patch('server.app.smtplib.SMTP') as smtp:
            app.state.maintenance()
            smtp.return_value.__enter__.return_value.starttls.assert_called_once()
        with app.state.store.db() as db:
            assert db.execute('SELECT mail_status FROM alerts').fetchone()[0]=='sent'


def test_alert_respects_applied_arming_and_ignition(setup):
    _,_,c=setup;login(setup)
    send(setup,sample(motion=True,armed=False))
    send(setup,sample(2,motion=True,armed=True,ignition=True))
    assert c.get('/api/v1/alerts').json()==[]


def test_server_restart_preserves_data(setup):
    cfg,_,_=setup
    send(setup,sample())
    app=create_app(cfg,worker=False)
    with TestClient(app) as c:
        c.post('/api/v1/login',json={'username':cfg.admin_user,'password':cfg.admin_password})
        assert len(c.get('/api/v1/history').json())==1


def test_session_expiry_and_rate_limit(setup):
    _,app,c=setup;login(setup)
    with app.state.store.db() as db: db.execute('UPDATE sessions SET expires=0')
    assert c.get('/api/v1/status').status_code==401
    for _ in range(4): assert c.post('/api/v1/login',json={'username':'admin','password':'wrong'}).status_code==401
    assert c.post('/api/v1/login',json={'username':'admin','password':'wrong'}).status_code==429


def test_low_battery_ignores_driving_replay_and_single_dip(setup):
    _,_,c=setup;login(setup)
    send(setup,sample(1,battery_v=11.0,mode='driving'))
    send(setup,sample(2,battery_v=11.0,mode='driving'))
    send(setup,sample(3,battery_v=11.0,is_replay=True))
    send(setup,sample(4,battery_v=11.0,is_replay=True))
    send(setup,sample(5,battery_v=12.6))
    send(setup,sample(6,battery_v=11.0))
    send(setup,sample(7,battery_v=12.5))
    assert c.get('/api/v1/alerts').json()==[]
    send(setup,sample(8,battery_v=11.6))
    send(setup,sample(9,battery_v=11.6))
    assert [a['kind'] for a in c.get('/api/v1/alerts').json()]==['low_battery']


def test_time_source_and_old_timestamp(setup):
    _,_,c=setup;login(setup)
    assert send(setup,sample(1,measured_at='2024-06-01T00:00:00Z')).status_code==422
    send(setup,sample(2,measured_at='2026-01-01T00:00:00Z'))
    send(setup,sample(3))
    rows=c.get('/api/v1/history?start=0').json()
    assert [r['time_source'] for r in rows]==['device','received']
    assert 'time_source' in c.get('/api/v1/history.csv?start=0').text.splitlines()[0]


def test_test_email_endpoint(setup):
    cfg,_,c=setup;headers=login(setup)
    assert c.post('/api/v1/test-email',headers=headers).status_code==409
    cfg.smtp_host='smtp.example.test';cfg.smtp_from='iot@example.test';cfg.alert_email='owner@example.test'
    app=create_app(cfg,worker=False)
    with TestClient(app) as client:
        r=client.post('/api/v1/login',json={'username':cfg.admin_user,'password':cfg.admin_password})
        h={'X-CSRF-Token':r.json()['csrf']}
        assert client.post('/api/v1/test-email').status_code==403
        assert client.post('/api/v1/test-email',headers=h).json()=={'queued':True}
        assert client.post('/api/v1/test-email',headers=h).status_code==429
        with patch('server.app.smtplib.SMTP') as smtp:
            app.state.maintenance()
            smtp.return_value.__enter__.return_value.send_message.assert_called_once()
        assert client.get('/api/v1/alerts').json()[0]['mail_status']=='sent'


def test_security_headers_and_chunked_rejected(setup):
    cfg,_,c=setup
    r=c.get('/')
    assert "default-src 'self'" in r.headers['content-security-policy']
    chunks=iter([b'{}'])
    r=c.post('/api/v1/telemetry',content=chunks,headers={'Authorization':'Bearer '+cfg.device_token,'Content-Type':'application/json'})
    assert r.status_code==411


def test_fall_alert_once_and_regardless_of_arming(setup):
    _,_,c=setup;login(setup)
    send(setup,sample(1,mode='parked',tilt_deg=8.0))
    send(setup,sample(2,mode='parked',tilt_deg=-82.0,fallen=True))
    send(setup,sample(3,mode='parked',tilt_deg=-83.0,fallen=True))
    alerts=c.get('/api/v1/alerts').json()
    assert [a['kind'] for a in alerts]==['fall'] and 'Xe bị ngã sang phải (nghiêng 82°)' in alerts[0]['message']
    status=c.get('/api/v1/status').json()
    assert status['telemetry']['fallen'] and status['connection']=='online'  # awake "parked" is not sleeping
    assert 'tilt_deg' in c.get('/api/v1/history.csv').text.splitlines()[0]


def test_weak_password_needs_explicit_flag(tmp_path):
    cfg = Config(str(tmp_path / 'w.sqlite3'), 'mt15-001', 'test-device-token-0123456789', 'admin', '123456')
    with pytest.raises(RuntimeError):
        create_app(cfg, worker=False)
    cfg.allow_weak_password = True
    app = create_app(cfg, worker=False)
    with TestClient(app) as c:
        assert c.post('/api/v1/login', json={'username': 'admin', 'password': '123456'}).status_code == 200
