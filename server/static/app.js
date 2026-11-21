'use strict';
const $ = id => document.getElementById(id);
let cloudDashboard = null;
let csrf = '', state = null, timer = null, map = null, marker = null, route = null, chart = null, refreshing = false;
let unsubscribe = null, liveTimer = null, refreshQueued = false;
const fmt = (v, unit, digits=1) => v == null ? '—' : `${Number(v).toFixed(digits)} ${unit}`;
const when = epoch => new Date(epoch * 1000).toLocaleString('vi-VN');
const modes = {driving:'Đang chạy',parked:'Đang đỗ',tracking:'Vừa có chuyển động',low_battery:'Bình yếu',bench:'Thử trên bàn'};
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
  }, 150);
}
function setUpdateMode(realtime) {
  clearInterval(timer);
  timer = setInterval(refresh, realtime ? 15000 : 5000);
  const label = $('update-mode');
  label.hidden = !cloudDashboard;
  label.textContent = realtime ? 'Cập nhật trực tiếp' : 'Tự cập nhật mỗi 5 giây';
}
async function openDashboard(){
  $('login-view').hidden=true;$('dashboard').hidden=false;
  if (!map && window.L) {
    $('map').textContent=''; map=L.map('map').setView([10.7769,106.7009],12);
    L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'© OpenStreetMap contributors',referrerPolicy:'strict-origin-when-cross-origin'}).addTo(map);
    route=L.polyline([],{color:'#b5d854',weight:3}).addTo(map);
  } else if (!window.L) $('map').textContent='Không tải được thư viện bản đồ. Kiểm tra Internet rồi tải lại trang.';
  if (!chart && window.Chart) chart=new Chart($('history-chart'),{type:'line',data:{labels:[],datasets:[
    {label:'Điện áp (V)',data:[],borderColor:'#c6f467',yAxisID:'v',pointRadius:0,borderWidth:2,spanGaps:false},
    {label:'Nhiệt độ (°C)',data:[],borderColor:'#78b8ff',yAxisID:'t',pointRadius:0,borderWidth:2,spanGaps:false}
  ]},options:{animation:false,responsive:true,maintainAspectRatio:false,interaction:{mode:'index',intersect:false},plugins:{legend:{labels:{color:'#aab7c7',usePointStyle:true}}},scales:{x:{ticks:{color:'#8997aa',maxTicksLimit:5},grid:{display:false}},v:{position:'left',ticks:{color:'#c6f467'},grid:{color:'#293342'}},t:{position:'right',ticks:{color:'#78b8ff'},grid:{drawOnChartArea:false}}}}});
  await refresh();
  if ($('dashboard').hidden) return;
  setUpdateMode(false);
  try {
    unsubscribe = cloudDashboard?.subscribe(scheduleLiveRefresh, realtime => {
      setUpdateMode(realtime);
      if (realtime) scheduleLiveRefresh();
    });
  } catch {
    // Keep periodic reads if this browser cannot open a Realtime client.
  }
  if (map) map.invalidateSize();
}
async function refresh(){
  if ($('dashboard').hidden) return;
  if (refreshing) { refreshQueued = true; return; }
  refreshing = true;
  try{
    const start=Date.now()/1000-Number($('history-range').value)*3600;
    const [s,history,alerts]=await Promise.all([api('status'),api(`history?start=${start}`),api('alerts')]);state=s;notice('');
    const p=s.telemetry;
    $('device-id').textContent=s.device_id;
    $('connection').textContent=({online:'Đã kết nối',sleeping:'Đang ngủ',offline:'Mất kết nối',never_seen:'Chưa có dữ liệu'})[s.connection];
    $('connection').classList.toggle('offline',['offline','never_seen'].includes(s.connection));
    $('last-update').textContent=p?`Cập nhật: ${when(s.received_at)} · ${s.age_s} giây trước${p.is_replay?' · dữ liệu gửi bù':''}`:'Chưa nhận được dữ liệu từ thiết bị.';
    const errs=p?.errors||[];
    $('battery').textContent=fmt(p?.battery_v,'V',2);
    const batteryNote = errs.includes('ADC_SATURATED_OR_INVALID') ? 'Tín hiệu đo không hợp lệ · kiểm tra dây GPIO34'
      : 'Chưa nhận số đo điện áp từ thiết bị';
    $('battery-note').textContent=p?.battery_v==null?batteryNote:errs.includes('SIMULATED_DATA')?'Dữ liệu mô phỏng':errs.includes('ADC_ABOVE_ACCURATE_RANGE')?'Vượt vùng ADC chính xác · giá trị tham khảo':errs.includes('ADC_NOT_CALIBRATED')?'Chưa hiệu chuẩn ADC · giá trị tham khảo':'Đã hiệu chuẩn trên thiết bị';
    $('signal').textContent=p?.signal_dbm!=null?`Sóng 4G: ${p.signal_dbm} dBm`:'';$('temperature').textContent=fmt(p?.temperature_c,'°C');$('speed').textContent=fmt(p?.speed_kmh,'km/h');
    $('vehicle-mode').textContent=p?(p.fallen?(p.tilt_deg>0?'NGÃ BÊN TRÁI':p.tilt_deg<0?'NGÃ BÊN PHẢI':'XE BỊ NGÃ'):(modes[p.mode]||p.mode)):'—';$('vehicle-mode').classList.toggle('danger',!!p?.fallen);
    $('motion-state').textContent=p?`${p.motion?'Có chuyển động':'Chưa phát hiện chuyển động'} · Nghiêng: ${p.tilt_deg==null?'—':Math.abs(Math.round(p.tilt_deg))<2?'thẳng':Math.abs(Math.round(p.tilt_deg))+'° '+(p.tilt_deg>0?'trái':'phải')}${p.ignition==null?'':' · Khóa: '+(p.ignition?'bật':'tắt')}`:'Chưa có dữ liệu chuyển động';
    $('gps-state').textContent=p?.gps_valid?'GPS đã có định vị':'GPS chưa có vị trí hợp lệ';$('satellites').textContent=`${p?.satellites??'—'} vệ tinh`;
    $('guard-state').textContent=`Chống trộm ${s.settings.armed?'đang bật':'đang tắt'}`;$('arm-toggle').textContent=s.settings.armed?'Tắt chống trộm':'Bật chống trộm';
    $('guard-applied').textContent=s.config_applied?'Thiết bị đã xác nhận.':'Đang chờ thiết bị nhận và xác nhận; khi ngủ có thể mất tới 15 phút.';
    const visibleErrors = errs.filter(code => code !== 'IGNITION_NOT_CONNECTED');
    const notes=[];if(!s.smtp_configured&&!cloudDashboard)notes.push('Email chưa cấu hình SMTP.');if(s.low_battery_voltage==null&&p?.battery_v!=null)notes.push('Chưa đặt ngưỡng bình yếu.');if(visibleErrors.length)notes.push('Thiết bị: '+visibleErrors.join(', '));
    $('configuration-note').textContent=notes.join(' ');$('configuration-note').hidden=!notes.length;
    $('email-status').textContent=cloudDashboard?'Cảnh báo từ Supabase':s.smtp_configured?`${s.email_pending} email chờ gửi`:'Email chưa bật';
    const fixes=history.filter(x=>x.gps_valid);if(route)route.setLatLngs(fixes.map(x=>[x.latitude,x.longitude]));
    const fix=p?.gps_valid?p:fixes.at(-1);
    if(fix&&map){const position=[fix.latitude,fix.longitude];if(!marker){marker=L.marker(position).addTo(map);map.setView(position,15);}else marker.setLatLng(position);}
    $('position-info').textContent=fix?`${fix.latitude.toFixed(6)}, ${fix.longitude.toFixed(6)}${p?.gps_valid?' · vị trí từ bản tin gần nhất':' · vị trí cũ trong lịch sử'}`:'Chưa có vị trí GPS hợp lệ trong khoảng thời gian chọn.';
    if(!fix&&marker&&map){map.removeLayer(marker);marker=null;}
    if(chart){chart.data.labels=history.map(x=>new Date(x.sample_at*1000).toLocaleTimeString('vi-VN'));chart.data.datasets[0].data=history.map(x=>x.battery_v);chart.data.datasets[1].data=history.map(x=>x.temperature_c);const hasV=history.some(x=>x.battery_v!=null);chart.data.datasets[0].hidden=!hasV;chart.options.scales.v.display=hasV;chart.update();}
    if (!cloudDashboard) $('csv-link').href=`/api/v1/history.csv?start=${start}`;
    const estimated=history.filter(x=>x.time_source==='received').length;
    $('time-note').textContent=estimated?`${estimated} mẫu không có giờ thiết bị: thời gian là lúc server nhận (mẫu gửi bù có thể bị lệch).`:'';$('time-note').hidden=!estimated;
    $('test-email').hidden=!s.smtp_configured;
    $('alerts').replaceChildren();
    if(!alerts.length){const el=document.createElement('p');el.className='muted';el.textContent='Chưa có cảnh báo.';$('alerts').append(el);}
    for(const a of alerts){const row=document.createElement('div');row.className='alert';const date=document.createElement('time');date.textContent=when(a.created_at);const detail=document.createElement('p');detail.textContent=a.message;const mail=document.createElement('span');mail.className='mail';mail.textContent=({sent:'Email đã gửi',pending:'Đang chờ gửi',failed:'Gửi thất bại',disabled:'Email chưa bật'})[a.mail_status]+(a.last_error?` (${a.last_error})`:'');row.append(date,detail,mail);$('alerts').append(row);}
  }catch(e){notice(e.message+' Dữ liệu đang hiển thị có thể đã cũ.');$('connection').textContent='Không kết nối server';$('connection').classList.add('offline');}finally{
    refreshing = false;
    if (refreshQueued) { refreshQueued = false; scheduleLiveRefresh(); }
  }
}
$('login-form').addEventListener('submit',async e=>{e.preventDefault();const form=new FormData(e.target);const button=e.target.querySelector('button');button.disabled=true;try{const r=await api('login',{method:'POST',body:JSON.stringify(Object.fromEntries(form))});csrf=r.csrf;e.target.password.value='';$('login-error').textContent='';await openDashboard();}catch(err){$('login-error').textContent=err.message;}finally{button.disabled=false;}});
$('logout').onclick=async()=>{try{await api('logout',{method:'POST'});csrf='';showLogin();}catch(e){notice(e.message);}};
$('arm-toggle').onclick=async()=>{if(!state)return;$('arm-toggle').disabled=true;try{await api('settings',{method:'PUT',body:JSON.stringify({armed:!state.settings.armed})});await refresh();}catch(e){notice(e.message);}finally{$('arm-toggle').disabled=false;}};
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
$('test-email').onclick=async()=>{$('test-email').disabled=true;try{await api('test-email',{method:'POST'});notice('Đã đưa email thử vào hàng đợi; trạng thái gửi hiện trong nhật ký cảnh báo.');}catch(e){notice(e.message);}finally{$('test-email').disabled=false;}};
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
