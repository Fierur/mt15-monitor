'use strict';
const $ = id => document.getElementById(id);
let cloudDashboard = null;
let csrf = '', timer = null, map = null, marker = null, route = null, chart = null, refreshing = false;
let unsubscribe = null, liveTimer = null, refreshQueued = false;
let latestStatus = null, recentHistory = [], fixes = [], historyFrame = null, dashboardSession = 0;
const fmt = (v, unit, digits=1) => v == null ? '—' : `${Number(v).toFixed(digits)} ${unit}`;
const when = epoch => new Date(epoch * 1000).toLocaleString('vi-VN');
const modes = {driving:'Đang chạy',parked:'Đang đỗ',tracking:'Vừa có chuyển động',low_battery:'Bình yếu',bench:'Thử trên bàn'};
const handledWarnings = new Set([
  'IGNITION_NOT_CONNECTED', 'GPS_NO_FRESH_FIX', 'ADC_SATURATED_OR_INVALID',
  'ADC_NOT_CALIBRATED', 'ADC_ABOVE_ACCURATE_RANGE',
]);
async function api(path, options={}) {
  if (cloudDashboard) {
    try {
      return await cloudDashboard.api(path, options);
    } catch (error) {
      if (error.status === 401 && path !== 'login') {
        cloudDashboard.clearSession();
        showLogin();
      }
      throw error;
    }
  }
  const r = await fetch('/api/v1/'+path, {...options, headers:{'Content-Type':'application/json','X-CSRF-Token':csrf,...options.headers}});
  if (r.status === 401) { showLogin(); throw new Error('Vui lòng đăng nhập.'); }
  if (!r.ok) { const body = await r.json().catch(()=>({})); throw new Error(typeof body.detail==='string'?body.detail:`Lỗi máy chủ (${r.status})`); }
  return r.json();
}
function showLogin(){
  clearInterval(timer);
  clearTimeout(liveTimer);
  liveTimer = null;
  refreshQueued = false;
  dashboardSession++;
  cancelAnimationFrame(historyFrame);
  historyFrame = null;
  latestStatus = null;
  recentHistory = [];
  fixes = [];
  unsubscribe?.();
  unsubscribe = null;
  $('dashboard').hidden = true;
  $('login-view').hidden = false;
}
function notice(message){$('page-error').textContent=message;$('page-error').hidden=!message;}
function scheduleLiveRefresh() {
  if (liveTimer !== null) return;
  liveTimer = setTimeout(() => {
    liveTimer = null;
    void refresh();
  }, 0);
}
function setUpdateMode(realtime) {
  clearInterval(timer);
  timer = setInterval(refresh, realtime ? 15000 : cloudDashboard ? 2000 : 5000);
  const label = $('update-mode');
  label.hidden = !cloudDashboard;
  label.textContent = realtime ? 'Cập nhật trực tiếp' : 'Tự cập nhật mỗi 2 giây';
}
function renderAlert(alert) {
  const row = document.createElement('div');
  row.className = 'alert';
  const date = document.createElement('time');
  date.textContent = when(alert.created_at);
  const detail = document.createElement('div');
  const summary = document.createElement('p');
  const lines = alert.message.split(/\r?\n/);
  const lastLine = lines.at(-1);
  // Stored alerts include either an event fix, a dated fallback fix or a GPS message.
  const position = lastLine.match(/^(?:Vị trí cuối \((.+)\): )?(https:\/\/maps\.google\.com\/\?q=-?\d+(?:\.\d+)?,-?\d+(?:\.\d+)?)$/);
  const hasPositionInfo = Boolean(position) || lastLine === 'GPS chưa có vị trí hợp lệ.';
  if (hasPositionInfo) lines.pop();
  summary.textContent = lines.map(line => {
    if (line === 'Gửi bù: không') return 'Bản tin: Gửi trực tiếp';
    if (line === 'Gửi bù: có (dữ liệu cũ)') return 'Bản tin: Gửi lại từ bộ nhớ ESP32 (gửi bù)';
    return line;
  }).join('\n');
  detail.append(summary);
  if (hasPositionInfo) {
    const locations = document.createElement('div');
    locations.className = 'alert-locations';
    const oldTime = position?.[1];
    const url = position?.[2];
    for (const [label, link, fallback] of [
      ['Vị trí hiện tại (lúc cảnh báo)', !oldTime && url, 'GPS chưa có tọa độ'],
      [oldTime ? `Vị trí cũ (${oldTime})` : 'Vị trí cũ', oldTime && url, 'Chưa ghi nhận trong cảnh báo này'],
    ]) {
      const line = document.createElement('p');
      const title = document.createElement('strong');
      title.textContent = label + ': ';
      line.append(title);
      if (link) {
        const anchor = document.createElement('a');
        anchor.href = link;
        anchor.target = '_blank';
        anchor.rel = 'noopener noreferrer';
        anchor.textContent = 'Mở bản đồ';
        line.append(anchor);
      } else line.append(fallback);
      locations.append(line);
    }
    detail.append(locations);
  }
  row.append(date, detail);
  return row;
}
async function openDashboard(){
  $('login-view').hidden=true;$('dashboard').hidden=false;
  if (!map && window.L) {
    $('map').textContent=''; map=L.map('map').setView([10.7769,106.7009],12);
    L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'© OpenStreetMap contributors',referrerPolicy:'strict-origin-when-cross-origin'}).addTo(map);
    route=L.polyline([],{color:'#b5d854',weight:3}).addTo(map);
  } else if (!window.L) $('map').textContent='Không tải được thư viện bản đồ. Kiểm tra Internet rồi tải lại trang.';
  if (!chart && window.Chart) {
    chart = new Chart($('history-chart'), {
      type: 'line',
      data: { labels: [], datasets: [{
        label: 'Nhiệt độ (°C)', data: [], borderColor: '#78b8ff',
        pointRadius: 0, borderWidth: 2, spanGaps: false,
      }] },
      options: {
        animation: false, responsive: true, maintainAspectRatio: false,
        interaction: { mode: 'index', intersect: false },
        plugins: { legend: { labels: { color: '#aab7c7', usePointStyle: true } } },
        scales: {
          x: { ticks: { color: '#8997aa', maxTicksLimit: 5 }, grid: { display: false } },
          y: { ticks: { color: '#78b8ff' }, grid: { color: '#293342' } },
        },
      },
    });
  }
  setUpdateMode(false);
  try {
    unsubscribe = cloudDashboard?.subscribe(receiveRealtime, realtime => {
      setUpdateMode(realtime);
      if (realtime) scheduleLiveRefresh();
    });
  } catch {
    // Keep periodic reads if this browser cannot open a Realtime client.
  }
  if (map) map.invalidateSize();
  await refresh();
}
function isLater(status, previous) {
  return status.received_at > previous.received_at ||
    (status.received_at === previous.received_at && status.telemetry.id > previous.telemetry.id);
}
function receiveRealtime(packet) {
  if (!packet || !Number.isFinite(packet.received_at) || !Number.isFinite(packet.sample_at)) {
    scheduleLiveRefresh();
    return;
  }

  const previous = latestStatus?.telemetry;
  if (!packet.is_replay && (!previous || packet.boot_id !== previous.boot_id || packet.seq > previous.seq)) {
    const status = cloudDashboard.statusFor(packet, latestStatus?.low_battery_voltage);
    if (!previous || isLater(status, latestStatus)) renderStatus(status);
  }
  updateHistory([packet]);
}
function renderStatus(s) {
    latestStatus = s;
    const p=s.telemetry;
    $('device-id').textContent=s.device_id;
    $('connection').textContent=({online:'Đã kết nối',sleeping:'Đang ngủ',offline:'Mất kết nối',never_seen:'Chưa có dữ liệu'})[s.connection];
    $('connection').classList.toggle('offline',['offline','never_seen'].includes(s.connection));
    $('last-update').textContent=p?`Cập nhật: ${when(s.received_at)} · ${s.age_s} giây trước${p.is_replay?' · bản tin gửi lại từ bộ nhớ ESP32':''}`:'Chưa nhận được dữ liệu từ thiết bị.';
    const errs=p?.errors||[];
    $('signal').textContent=p?.signal_dbm!=null?`Sóng 4G: ${p.signal_dbm} dBm`:'';$('temperature').textContent=fmt(p?.temperature_c,'°C');$('speed').textContent=fmt(p?.speed_kmh,'km/h');
    $('vehicle-mode').textContent=p?(p.fallen?(p.tilt_deg>0?'NGÃ BÊN TRÁI':p.tilt_deg<0?'NGÃ BÊN PHẢI':'XE BỊ NGÃ'):(modes[p.mode]||p.mode)):'—';$('vehicle-mode').classList.toggle('danger',!!p?.fallen);
    $('motion-state').textContent=p?`${p.motion?'Có chuyển động':'Chưa phát hiện chuyển động'} · Nghiêng: ${p.tilt_deg==null?'—':Math.abs(Math.round(p.tilt_deg))<2?'thẳng':Math.abs(Math.round(p.tilt_deg))+'° '+(p.tilt_deg>0?'trái':'phải')}${p.ignition==null?'':' · Khóa: '+(p.ignition?'bật':'tắt')}`:'Chưa có dữ liệu chuyển động';
    $('gps-state').textContent=p?.gps_valid?'GPS đã có định vị':'GPS chưa có vị trí hợp lệ';$('satellites').textContent=`${p?.satellites??'—'} vệ tinh`;
    const visibleErrors = errs.filter(code => !handledWarnings.has(code));
    const notes=[];if(s.low_battery_voltage===null&&p?.battery_v!=null)notes.push('Chưa đặt ngưỡng bình yếu.');if(visibleErrors.length)notes.push('Thiết bị: '+visibleErrors.join(', '));
    $('configuration-note').textContent=notes.join(' ');$('configuration-note').hidden=!notes.length;
    renderPosition();
}
function renderPosition() {
    if (!latestStatus) return;
    const { telemetry: p, connection } = latestStatus;
    const latestFix = p?.gps_valid ? p : null;
    const currentFix = connection === 'online' && !p?.is_replay ? latestFix : null;
    let oldFix = fixes.at(-1);
    if (currentFix) oldFix = fixes.findLast(item => item.sample_at < currentFix.sample_at);
    else if (latestFix && (!oldFix || latestFix.sample_at > oldFix.sample_at)) oldFix = latestFix;
    const fix = currentFix ?? oldFix;
    if(fix&&map){const position=[fix.latitude,fix.longitude];if(!marker){marker=L.marker(position).addTo(map);map.setView(position,15);}else marker.setLatLng(position);}
    const formatFix = item => `${item.latitude.toFixed(6)}, ${item.longitude.toFixed(6)} · ${when(item.sample_at)}`;
    $('position-current').textContent = currentFix ? formatFix(currentFix) : connection === 'online' ? 'Đang chờ GPS định vị' : 'Đang chờ bản tin mới';
    $('position-old').textContent = oldFix ? formatFix(oldFix) : 'Chưa có tọa độ trong lịch sử đang xem';
    if(!fix&&marker&&map){map.removeLayer(marker);marker=null;}
}
function historyStart(range = $('history-range').value) {
  return Date.now() / 1000 - Number(range) * 3600;
}
function updateHistory(rows) {
  const start = historyStart();
  if (cloudDashboard) {
    const known = new Map(recentHistory.map(item => [item.id, item]));
    for (const row of rows) known.set(row.id, row);
    recentHistory = [...known.values()].filter(item => item.sample_at >= start)
      .sort((a, b) => a.sample_at - b.sample_at || a.id - b.id).slice(-1000);
  } else recentHistory = rows;
  if (historyFrame !== null) return;
  historyFrame = requestAnimationFrame(() => {
    historyFrame = null;
    renderHistory(recentHistory, start);
  });
}
function renderHistory(history, start) {
    fixes=history.filter(x=>x.gps_valid);if(route)route.setLatLngs(fixes.map(x=>[x.latitude,x.longitude]));
    renderPosition();
    if (chart) {
      chart.data.labels = history.map(x => new Date(x.sample_at * 1000).toLocaleTimeString('vi-VN'));
      chart.data.datasets[0].data = history.map(x => x.temperature_c);
      chart.update();
    }
    if (!cloudDashboard) $('csv-link').href=`/api/v1/history.csv?start=${start}`;
    const estimated=history.filter(x=>x.time_source==='received').length;
    $('time-note').textContent=estimated?`${estimated} mẫu dùng giờ máy chủ nhận. Với bản tin gửi lại từ bộ nhớ ESP32, giờ nhận có thể muộn hơn giờ đo.`:'';$('time-note').hidden=!estimated;
}
function renderAlerts(alerts) {
    $('alerts').replaceChildren();
    if(!alerts.length){const el=document.createElement('p');el.className='muted';el.textContent='Chưa có cảnh báo.';$('alerts').append(el);}
    for (const alert of alerts) $('alerts').append(renderAlert(alert));
}
async function refresh(){
  if ($('dashboard').hidden) return;
  if (refreshing) { refreshQueued = true; return; }
  refreshing = true;
  const session = dashboardSession;
  const range = $('history-range').value;
  const statusBeforeRefresh = latestStatus;
  try {
    const requests = [
      api('status').then(status => {
        if (session !== dashboardSession) return;
        if (cloudDashboard && latestStatus?.telemetry && (!status.telemetry || isLater(latestStatus, status))) {
          status = cloudDashboard.statusFor(latestStatus.telemetry, status.low_battery_voltage);
        }
        renderStatus(status);
      }),
      api(`history?start=${historyStart(range)}`).then(rows => {
        if (session === dashboardSession && range === $('history-range').value) updateHistory(rows);
      }),
      api('alerts').then(alerts => {
        if (session === dashboardSession) renderAlerts(alerts);
      }),
    ];
    const results = await Promise.allSettled(requests);
    if (session !== dashboardSession) return;
    const failed = results.find(result => result.status === 'rejected');
    notice(failed ? failed.reason.message + ' Dữ liệu đang hiển thị có thể đã cũ.' : '');
    if (results[0].status === 'rejected' && latestStatus === statusBeforeRefresh) {
      $('connection').textContent = 'Không kết nối server';
      $('connection').classList.add('offline');
    }
  } finally {
    refreshing = false;
    if (refreshQueued) { refreshQueued = false; scheduleLiveRefresh(); }
  }
}
$('login-form').addEventListener('submit',async e=>{e.preventDefault();const form=new FormData(e.target);const button=e.target.querySelector('button');button.disabled=true;try{const r=await api('login',{method:'POST',body:JSON.stringify(Object.fromEntries(form))});csrf=r.csrf;e.target.password.value='';$('login-error').textContent='';await openDashboard();}catch(err){$('login-error').textContent=err.message;}finally{button.disabled=false;}});
$('logout').onclick=async()=>{try{await api('logout',{method:'POST'});csrf='';showLogin();}catch(e){notice(e.message);}};
$('history-range').onchange=refresh;
$('csv-link').addEventListener('click', async event => {
  if (!cloudDashboard) return;
  event.preventDefault();
  try {
    const start = Date.now() / 1000 - Number($('history-range').value) * 3600;
    const csv = await cloudDashboard.csv(start);
    const url = URL.createObjectURL(new Blob([csv], { type: 'text/csv;charset=utf-8' }));
    const link = document.createElement('a');
    link.href = url;
    link.download = 'mt15-history.csv';
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  } catch (error) {
    notice(error.message);
  }
});
async function initDashboard() {
  const form = $('login-form');
  const button = form.querySelector('button');
  button.disabled = true;
  try {
    const response = await fetch('/dashboard-config');
    if (!response.ok) throw new Error('Không tải được cấu hình dashboard.');
    const config = await response.json();
    if (config.mode === 'supabase') {
      cloudDashboard = createSupabaseDashboard(config);
      form.elements.username.type = 'email';
      form.elements.username.labels[0].childNodes[0].textContent = 'Email Supabase';
      $('csv-link').href = '#';
      $('session-note').textContent = 'Dữ liệu riêng tư · giữ phiên trong tab này';
      if (await cloudDashboard.resume()) await openDashboard();
    } else {
      const current = await api('me').catch(() => null);
      if (current) {
        csrf = current.csrf;
        await openDashboard();
      }
    }
  } catch (error) {
    $('login-error').textContent = error.message;
    showLogin();
  } finally {
    button.disabled = false;
  }
}
initDashboard();
