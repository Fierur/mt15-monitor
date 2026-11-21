"""Opt-in real-browser acceptance tests against an isolated temporary database."""
import json
import os
import socket
import threading
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path
from urllib.parse import urlsplit

import httpx
import pytest
import uvicorn

from server.app import Config, create_app

pytestmark = pytest.mark.skipif(os.getenv('RUN_BROWSER_TESTS') != '1', reason='Set RUN_BROWSER_TESTS=1 and install requirements-dev.txt')


def test_dashboard_desktop_mobile_and_control(tmp_path):
    from playwright.sync_api import sync_playwright, expect
    cfg=Config(str(tmp_path/'browser.sqlite3'),'mt15-browser-demo','demo-token-0123456789-abcdef','admin','browser-test-password')
    app=create_app(cfg,worker=False)
    sock=socket.socket();sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    server=uvicorn.Server(uvicorn.Config(app,log_level='error'))
    thread=threading.Thread(target=lambda:server.run(sockets=[sock]),daemon=True);thread.start()
    base=f'http://127.0.0.1:{port}'
    try:
        deadline=time.monotonic()+15
        while not server.started:
            if time.monotonic()>deadline: raise RuntimeError('Test server failed to start')
            time.sleep(.05)
        with httpx.Client(base_url=base) as client:
            for seq in range(12):
                payload=dict(device_id=cfg.device_id,boot_id='browser-demo',seq=seq,uptime_s=seq*5,
                             mode='bench',battery_v=12.6,temperature_c=31.2,gps_valid=True,
                             latitude=10.7769+seq*.00001,longitude=106.7009+seq*.00001,speed_kmh=8.5,satellites=9,
                             measured_at=datetime.fromtimestamp(time.time()-(12-seq)*5,timezone.utc).isoformat(),
                             errors=['SIMULATED_DATA'])
                assert client.post('/api/v1/telemetry',json=payload,headers={'Authorization':'Bearer '+cfg.device_token}).status_code==200
        with sync_playwright() as p:
            browser=p.chromium.launch(channel='msedge',headless=True)
            context=browser.new_context(viewport={'width':1440,'height':1100},accept_downloads=True)
            # No Internet dependency except map tiles: block every non-local request.
            context.route('**/*',lambda route:route.continue_() if route.request.url.startswith(base) else route.abort())
            page=context.new_page();errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
            page.on('console',lambda m:errors.append(m.text) if m.type=='error' and 'Content Security Policy' in m.text else None)
            page.goto(base,wait_until='load')
            assert page.evaluate('Boolean(window.L && window.Chart)'), 'vendored Leaflet/Chart.js not loaded'
            expect(page.locator('input[name=username]')).to_have_value('')
            page.locator('input[name=username]').fill(cfg.admin_user)
            page.locator('input[name=password]').fill(cfg.admin_password)
            page.get_by_role('button',name='Vào bảng điều khiển →').click()
            expect(page.locator('#dashboard')).to_be_visible()
            expect(page.locator('#battery')).to_have_count(0)
            expect(page.locator('#temperature')).to_have_text('31.2 °C')
            expect(page.locator('#configuration-note')).to_contain_text('SIMULATED_DATA')
            assert page.evaluate("Chart.getChart('history-chart').data.datasets.map(item => item.label)") == ['Nhiệt độ (°C)']
            expect(page.locator('#test-email')).to_have_count(0)
            expect(page.locator('#arm-toggle')).to_have_count(0)
            expect(page.locator('.leaflet-marker-icon')).to_have_count(1)
            expect(page.locator('#position-current')).to_contain_text('10.777010')
            expect(page.locator('#position-old')).to_contain_text('10.777000')
            with httpx.Client(base_url=base) as client:
                payload.update(seq=12, fallen=True, tilt_deg=70)
                assert client.post('/api/v1/telemetry',json=payload,headers={'Authorization':'Bearer '+cfg.device_token}).status_code==200
            expect(page.locator('#alerts')).to_contain_text('Xe bị ngã', timeout=10000)
            expect(page.locator('.alert-locations p').first).to_contain_text('Vị trí hiện tại (lúc cảnh báo)')
            expect(page.locator('.alert-locations p').first.get_by_role('link')).to_be_visible()
            output=Path('data/qa');output.mkdir(parents=True,exist_ok=True)
            page.screenshot(path=str(output/'dashboard-desktop.png'),full_page=True)
            page.set_viewport_size({'width':390,'height':844})
            page.wait_for_timeout(300)  # let ResizeObservers (Chart.js, Leaflet) settle
            overflow=page.evaluate('''[...document.querySelectorAll('body *')].filter(e=>!e.closest('.leaflet-pane')&&e.getBoundingClientRect().right>window.innerWidth+0.5).map(e=>e.tagName+'#'+e.id+'.'+e.className).slice(0,8)''')
            assert page.evaluate('document.documentElement.scrollWidth <= window.innerWidth'), overflow
            page.screenshot(path=str(output/'dashboard-mobile.png'),full_page=True)
            with page.expect_download() as download:
                page.locator('#csv-link').click()
            assert download.value.suggested_filename=='mt15-history.csv'
            page.get_by_role('button',name='Đăng xuất').click()
            expect(page.locator('#login-view')).to_be_visible()
            assert errors==[], errors
            context.close();browser.close()
    finally:
        server.should_exit=True;thread.join(timeout=10);sock.close()


def test_supabase_dashboard_auth_telemetry_settings_and_csv(tmp_path):
    from playwright.sync_api import expect, sync_playwright

    cloud = 'https://mt15-test.supabase.co'
    cfg = Config(str(tmp_path / 'cloud.sqlite3'), 'mt15-001', 'device-token-test-0123456789',
                 'admin', 'local-test-password', supabase_url=cloud,
                 supabase_publishable_key='sb_publishable_test')
    app = create_app(cfg, worker=False)
    sock = socket.socket()
    sock.bind(('127.0.0.1', 0))
    base = f'http://127.0.0.1:{sock.getsockname()[1]}'
    server = uvicorn.Server(uvicorn.Config(app, log_level='error'))
    thread = threading.Thread(target=lambda: server.run(sockets=[sock]), daemon=True)
    thread.start()
    state = {'admin': False, 'refreshes': 0, 'logouts': 0, 'history_reads': 0, 'status_reads': 0}
    timestamp = datetime.now(timezone.utc).isoformat()
    sample = dict(id=1, boot_id='cloud-test', seq=1, is_replay=False,
                  sample_at=timestamp, received_at=timestamp, temperature_c=31.2, battery_v=12.5,
                  mode='bench', armed=False, config_revision=0, next_report_s=5,
                  errors=['IGNITION_NOT_CONNECTED', 'GPS_NO_FRESH_FIX', 'ADC_NOT_CALIBRATED'])
    sockets = []

    def mock_realtime(socket):
        def respond_message(message):
            join_ref, ref, topic, event, payload = json.loads(message)
            response = {}
            if event == 'phx_join':
                assert payload['access_token'] == 'user-token'
                bindings = payload['config']['postgres_changes']
                assert all(binding['filter'] == 'device_id=eq.mt15-001' for binding in bindings)
                response['postgres_changes'] = [dict(binding, id=index) for index, binding in enumerate(bindings, 1)]
                telemetry_id = next(binding['id'] for binding in response['postgres_changes'] if binding['table'] == 'telemetry')
                sockets.append((socket, join_ref, topic, telemetry_id))
            socket.send(json.dumps([join_ref, ref, topic, 'phx_reply', dict(status='ok', response=response)]))

        socket.on_message(respond_message)

    def emit_telemetry(record):
        socket, join_ref, topic, telemetry_id = sockets[-1]
        socket.send(json.dumps([join_ref, None, topic, 'postgres_changes', {
            'ids': [telemetry_id], 'data': dict(schema='public', table='telemetry', type='INSERT',
                                              commit_timestamp=timestamp, columns=[], record=record),
        }]))

    def respond(route):
        request = route.request
        path = urlsplit(request.url).path
        headers = {
            'Access-Control-Allow-Origin': base,
            'Access-Control-Allow-Headers': 'apikey, authorization, content-type',
            'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
        }
        if request.method == 'OPTIONS':
            route.fulfill(status=204, headers=headers)
            return
        assert request.headers['apikey'] == cfg.supabase_publishable_key
        if path.startswith('/rest/'):
            assert request.headers['authorization'] == 'Bearer user-token'
        if path == '/auth/v1/token':
            if urlsplit(request.url).query == 'grant_type=refresh_token':
                assert request.post_data_json == {'refresh_token': 'refresh-token'}
                state['refreshes'] += 1
            else:
                assert request.post_data_json['email'] == 'owner@example.test'
            result = {'access_token': 'user-token', 'refresh_token': 'refresh-token', 'expires_in': 3600}
            if state['refreshes']:
                result['refresh_token'] = 'rotated-refresh-token'
        elif path == '/auth/v1/logout':
            state['logouts'] += 1
            route.fulfill(status=204, headers=headers)
            return
        elif path == '/rest/v1/admins':
            result = [{'user_id': 'admin-id'}] if state['admin'] else []
        elif path == '/rest/v1/device_latest':
            state['status_reads'] += 1
            result = [{'telemetry': sample}]
        elif path == '/rest/v1/settings':
            result = [{'low_battery_v': None}]
        elif path == '/rest/v1/telemetry':
            state['history_reads'] += 1
            result = [sample]
        elif path == '/rest/v1/alerts':
            result = [{
                'id': 1, 'created_at': timestamp, 'mail_status': 'pending',
                'message': 'Cloud movement\nGửi bù: có (dữ liệu cũ)\n'
                           'Vị trí cuối (13:16:04 21/11/2026): https://maps.google.com/?q=10.777,106.701',
            }]
        else:
            raise AssertionError(path)
        route.fulfill(json=result, headers=headers)

    try:
        deadline = time.monotonic() + 15
        while not server.started:
            if time.monotonic() > deadline:
                raise RuntimeError('Test server failed to start')
            time.sleep(.05)
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(channel='msedge', headless=True)
            context = browser.new_context(accept_downloads=True)
            context.route('**/*', lambda route: route.continue_()
                          if route.request.url.startswith(base) else route.abort())
            context.route(cloud + '/**', respond)
            context.route_web_socket('wss://mt15-test.supabase.co/**', mock_realtime)
            page = context.new_page()
            page.goto(base)
            expect(page.locator('input[name=username]')).to_have_attribute('type', 'email')
            page.locator('input[name=username]').fill('owner@example.test')
            page.locator('input[name=password]').fill('cloud-test-password')
            page.locator('#login-form button').click()
            expect(page.locator('#login-error')).to_contain_text('quản trị MT15')
            state['admin'] = True
            with page.expect_response(lambda response: '/rest/v1/telemetry?' in response.url and state['history_reads'] >= 2):
                page.locator('#login-form button').click()
            expect(page.locator('#temperature')).to_have_text('31.2 °C')
            expect(page.locator('#update-mode')).to_have_text('Cập nhật trực tiếp')
            expect(page.locator('#battery')).to_have_count(0)
            reads_before_push = (state['status_reads'], state['history_reads'])
            sample.update(id=2, seq=2, temperature_c=33.1)
            emit_telemetry(sample)
            expect(page.locator('#temperature')).to_have_text('33.1 °C', timeout=1000)
            replay = dict(sample, id=3, seq=0, is_replay=True, temperature_c=28,
                          sample_at=(datetime.fromisoformat(timestamp) - timedelta(seconds=10)).isoformat())
            emit_telemetry(replay)
            page.wait_for_function("Chart.getChart('history-chart').data.datasets[0].data.includes(28)")
            expect(page.locator('#temperature')).to_have_text('33.1 °C')
            assert (state['status_reads'], state['history_reads']) == reads_before_push
            expect(page.locator('#alerts')).to_contain_text('Cloud movement')
            expect(page.locator('#alerts')).to_contain_text('Gửi lại từ bộ nhớ ESP32')
            expect(page.locator('#alerts .mail')).to_have_count(0)
            expect(page.locator('#arm-toggle')).to_have_count(0)
            locations = page.locator('.alert-locations p')
            expect(locations.nth(0)).to_have_text('Vị trí hiện tại (lúc cảnh báo): GPS chưa có tọa độ')
            expect(locations.nth(1)).to_contain_text('Vị trí cũ (13:16:04 21/11/2026)')
            expect(locations.nth(1).get_by_role('link')).to_have_attribute('href', 'https://maps.google.com/?q=10.777,106.701')
            expect(page.locator('#gps-state')).to_have_text('GPS chưa có vị trí hợp lệ')
            expect(page.locator('#configuration-note')).not_to_contain_text('GPS_NO_FRESH_FIX')
            expect(page.locator('#configuration-note')).not_to_contain_text('IGNITION_NOT_CONNECTED')
            page.reload()
            expect(page.locator('#temperature')).to_have_text('33.1 °C')
            expect(page.locator('#login-view')).to_be_hidden()
            assert state['refreshes'] == 0
            key = 'mt15:auth:' + cloud + ':' + cfg.device_id
            page.evaluate("""key => {
                const session = JSON.parse(sessionStorage.getItem(key));
                session.expiresAt = Date.now() - 1000;
                sessionStorage.setItem(key, JSON.stringify(session));
            }""", key)
            page.reload()
            expect(page.locator('#temperature')).to_have_text('33.1 °C')
            assert state['refreshes'] == 1
            assert page.evaluate('key => JSON.parse(sessionStorage.getItem(key)).refresh_token',
                                 key) == 'rotated-refresh-token'
            with page.expect_download() as download:
                page.locator('#csv-link').click()
            assert '33.1' in Path(download.value.path()).read_text()
            page.locator('#logout').click()
            expect(page.locator('#login-view')).to_be_visible()
            assert state['logouts'] == 1
            page.reload()
            expect(page.locator('#login-form button')).to_be_enabled()
            expect(page.locator('#dashboard')).to_be_hidden()
            assert page.evaluate('key => sessionStorage.getItem(key)', key) is None
            browser.close()
    finally:
        server.should_exit = True
        thread.join(timeout=10)
        sock.close()
