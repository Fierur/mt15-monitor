'use strict';

// The publishable key identifies the project. Supabase Auth and RLS protect rows.
function createSupabaseDashboard(config) {
  const storageKey = 'mt15:auth:' + config.url + ':' + config.device_id;
  let session = restoreSession();
  let refreshInFlight = null;
  let realtimeClient = null;
  const device = { device_id: 'eq.' + config.device_id };
  const time = value => Date.parse(value) / 1000;
  const sample = row => ({ ...row, sample_at: time(row.sample_at), received_at: time(row.received_at) });

  function restoreSession() {
    try {
      const saved = JSON.parse(sessionStorage.getItem(storageKey));
      if (saved && typeof saved.access_token === 'string' && saved.access_token
          && typeof saved.refresh_token === 'string' && saved.refresh_token
          && Number.isFinite(saved.expiresAt)) return saved;
      sessionStorage.removeItem(storageKey);
    } catch {
      // The dashboard can still sign in if browser storage is unavailable.
    }
    return null;
  }

  function saveSession(updated) {
    session = updated;
    if (updated && realtimeClient) {
      void realtimeClient.setAuth(updated.access_token).catch(() => {});
    }
    try {
      if (updated) sessionStorage.setItem(storageKey, JSON.stringify(updated));
      else sessionStorage.removeItem(storageKey);
    } catch {
      // Keep the live session in memory when browser storage is unavailable.
    }
  }

  async function request(path, { method = 'GET', body, token } = {}) {
    const headers = { apikey: config.key, 'Content-Type': 'application/json' };
    if (token) headers.Authorization = 'Bearer ' + token;
    const response = await fetch(config.url + path, {
      method, headers, body: body === undefined ? undefined : JSON.stringify(body),
      signal: AbortSignal.timeout(12000)
    });
    if (!response.ok) {
      const message = response.status === 401 || response.status === 403
        ? 'Phiên đăng nhập không hợp lệ hoặc tài khoản không có quyền.'
        : 'Lỗi Supabase (' + response.status + ').';
      throw Object.assign(new Error(message), { status: response.status });
    }
    return response.status === 204 ? null : response.json();
  }

  async function signIn(grant, credentials) {
    const result = await request('/auth/v1/token?grant_type=' + grant, {
      method: 'POST', body: credentials
    });
    return {
      access_token: result.access_token,
      refresh_token: result.refresh_token,
      expiresAt: Date.now() + result.expires_in * 1000
    };
  }

  async function accessToken() {
    if (!session) throw Object.assign(new Error('Vui lòng đăng nhập.'), { status: 401 });
    if (Date.now() < session.expiresAt - 30000) return session.access_token;
    if (!refreshInFlight) {
      const current = session;
      refreshInFlight = signIn('refresh_token', { refresh_token: current.refresh_token })
        .then(updated => {
          if (session !== current) throw new Error('Phiên đăng nhập đã thay đổi.');
          saveSession(updated);
          return updated.access_token;
        }).catch(error => {
          if (session === current && [400, 401, 403].includes(error.status)) {
            saveSession(null);
            error.status = 401;
          }
          throw error;
        }).finally(() => { refreshInFlight = null; });
    }
    return refreshInFlight;
  }

  async function query(table, params, options = {}) {
    return request('/rest/v1/' + table + '?' + new URLSearchParams(params), {
      ...options, token: await accessToken()
    });
  }

  async function requireAdmin() {
    const admins = await query('admins', { select: 'user_id', limit: 1 });
    if (!admins.length) {
      throw Object.assign(new Error('Tài khoản chưa được cấp quyền quản trị MT15.'), { status: 403 });
    }
  }

  async function history(start, limit) {
    const rows = await query('telemetry', {
      ...device, select: '*', sample_at: 'gte.' + new Date(start * 1000).toISOString(),
      order: 'sample_at.desc,id.desc', limit
    });
    return rows.reverse().map(sample);
  }

  function statusFor(telemetry, lowBatteryVoltage) {
    const age = telemetry ? Math.max(0, Date.now() / 1000 - telemetry.received_at) : null;
    const online = telemetry && age <= Math.max(30, telemetry.next_report_s + 60);
    return {
      device_id: config.device_id, telemetry,
      received_at: telemetry?.received_at, age_s: age === null ? null : Math.floor(age),
      connection: !telemetry ? 'never_seen' : !online ? 'offline'
        : telemetry.mode === 'parked' && telemetry.next_report_s >= 60 ? 'sleeping' : 'online',
      low_battery_voltage: lowBatteryVoltage
    };
  }

  async function status() {
    const [[latest], [settings]] = await Promise.all([
      query('device_latest', { ...device, select: 'telemetry(*)', limit: 1 }),
      query('settings', { ...device, select: 'low_battery_v', limit: 1 })
    ]);
    if (!settings) throw new Error('Không tìm thấy cấu hình thiết bị trên Supabase.');
    return statusFor(latest?.telemetry ? sample(latest.telemetry) : null, settings.low_battery_v);
  }

  return {
    statusFor,
    clearSession() { saveSession(null); },
    subscribe(onChange, onStatus) {
      if (!window.supabase?.RealtimeClient || !session) return null;
      const endpoint = config.url.replace(/^https:\/\//, 'wss://') + '/realtime/v1';
      const client = new window.supabase.RealtimeClient(endpoint, { params: { apikey: config.key } });
      realtimeClient = client;
      const channel = client.channel('mt15:' + config.device_id);
      for (const table of ['telemetry', 'settings', 'alerts']) {
        channel.on('postgres_changes', {
          event: table === 'telemetry' ? 'INSERT' : '*',
          schema: 'public', table, filter: `device_id=eq.${config.device_id}`
        }, change => {
          if (realtimeClient !== client) return;
          onChange(table === 'telemetry' ? sample(change.new) : null);
        });
      }
      void accessToken().then(token => client.setAuth(token)).then(() => {
        if (realtimeClient !== client) return;
        channel.subscribe(status => {
          if (realtimeClient === client) onStatus(status === 'SUBSCRIBED');
        });
      }).catch(() => {
        if (realtimeClient === client) onStatus(false);
      });
      return () => {
        if (realtimeClient === client) realtimeClient = null;
        void client.removeChannel(channel).catch(() => {});
      };
    },
    async resume() {
      if (!session) return false;
      try {
        await requireAdmin();
        return true;
      } catch (error) {
        if ([401, 403].includes(error.status)) saveSession(null);
        if (error.status === 401) return false;
        throw error;
      }
    },
    async api(path, options = {}) {
      const [route, search] = path.split('?');
      if (route === 'login') {
        const { username, password } = JSON.parse(options.body);
        session = await signIn('password', { email: username.trim(), password });
        try {
          await requireAdmin();
          saveSession(session);
        } catch (error) {
          saveSession(null);
          throw error;
        }
        return { csrf: '' };
      }
      if (route === 'logout') {
        const current = session;
        saveSession(null);
        if (current) await request('/auth/v1/logout?scope=local', {
          method: 'POST', token: current.access_token
        }).catch(() => {});
        return { ok: true };
      }
      if (route === 'status') return status();
      if (route === 'history') return history(Number(new URLSearchParams(search).get('start')), 1000);
      if (route === 'alerts') {
        const rows = await query('alerts', { ...device, select: '*', order: 'id.desc', limit: 100 });
        return rows.map(row => ({ ...row, created_at: time(row.created_at) }));
      }
      throw new Error('API dashboard không được hỗ trợ.');
    },
    async csv(start) {
      const fields = ['sample_at', 'received_at', 'battery_v', 'temperature_c', 'latitude',
        'longitude', 'gps_valid', 'speed_kmh', 'motion', 'mode', 'is_replay', 'time_source'];
      const rows = await history(start, 1000);
      return [fields.join(','), ...rows.map(row => fields.map(field => row[field] ?? '').join(','))].join('\r\n');
    }
  };
}
