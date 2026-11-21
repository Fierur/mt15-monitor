-- =====================================================================
-- MT15 Monitor – Supabase schema
-- Run once in Supabase Dashboard → SQL Editor → New query → Run.
-- Safe to re-run: every object uses IF NOT EXISTS / CREATE OR REPLACE.
--
-- Data path (no always-on server needed):
--   ESP32/A7680C --HTTPS--> POST /rest/v1/rpc/ingest_telemetry  (anon key + device token)
--   Website (Vercel) --supabase-js--> tables, read-only for the logged-in admin (RLS)
-- =====================================================================

create extension if not exists pgcrypto with schema extensions;

-- ---------------------------------------------------------------------
-- Tables
-- ---------------------------------------------------------------------
create table if not exists public.devices (
  id          text primary key check (id ~ '^[A-Za-z0-9_-]{1,64}$'),
  name        text not null default '',
  token_hash  text not null,              -- sha256 (hex) of the device token; the token itself is never stored
  created_at  timestamptz not null default now()
);

create table if not exists public.settings (
  device_id      text primary key references public.devices(id) on delete cascade,
  armed          boolean not null default false,
  revision       integer not null default 0,   -- bumped by trigger whenever "armed" changes
  low_battery_v  real check (low_battery_v is null or low_battery_v between 9 and 14),  -- null = no battery alerts
  updated_at     timestamptz not null default now()
);

create table if not exists public.telemetry (
  id               bigint generated always as identity primary key,
  device_id        text not null references public.devices(id) on delete cascade,
  boot_id          text not null,
  seq              bigint not null,
  received_at      timestamptz not null default now(),
  sample_at        timestamptz not null,
  time_source      text not null check (time_source in ('device', 'received')),
  is_replay        boolean not null default false,
  mode             text not null check (mode in ('driving', 'parked', 'tracking', 'low_battery', 'bench')),
  armed            boolean not null default false,
  config_revision  integer not null default 0,
  ignition         boolean,
  battery_v        real,
  temperature_c    real,
  gps_valid        boolean not null default false,
  latitude         double precision,
  longitude        double precision,
  speed_kmh        real,
  satellites       integer,
  acceleration_g   real,
  gyro_dps         real,
  motion           boolean not null default false,
  tilt_deg         real,                  -- signed left/right lean: + left, - right
  fallen           boolean not null default false,
  signal_dbm       integer,
  next_report_s    integer not null default 5,
  errors           text[] not null default '{}',
  unique (device_id, boot_id, seq)        -- replays and retries are de-duplicated
);
create index if not exists telemetry_device_time on public.telemetry (device_id, sample_at desc);

-- Latest live (non-replay) sample per device, so the dashboard never shows an older replayed packet as "now".
create table if not exists public.device_latest (
  device_id     text primary key references public.devices(id) on delete cascade,
  telemetry_id  bigint references public.telemetry(id) on delete set null,
  boot_id       text,
  seq           bigint,
  updated_at    timestamptz not null default now()
);

create table if not exists public.alerts (
  id            bigint generated always as identity primary key,
  device_id     text not null references public.devices(id) on delete cascade,
  created_at    timestamptz not null default now(),
  kind          text not null check (kind in ('movement', 'fall', 'low_battery', 'test')),
  telemetry_id  bigint references public.telemetry(id) on delete set null,
  message       text not null,
  mail_status   text not null default 'pending' check (mail_status in ('pending', 'sent', 'failed', 'disabled')),
  attempts      integer not null default 0,
  last_error    text not null default ''
);
create index if not exists alerts_device_time on public.alerts (device_id, created_at desc);

-- Accounts allowed to see the dashboard (one admin for this project).
create table if not exists public.admins (
  user_id  uuid primary key references auth.users(id) on delete cascade
);

-- ---------------------------------------------------------------------
-- Helpers
-- ---------------------------------------------------------------------
create or replace function public.is_admin()
returns boolean
language sql stable security definer
set search_path = ''
as $$
  select exists (select 1 from public.admins where user_id = (select auth.uid()));
$$;

-- The website may only flip "armed" (and the battery threshold); the revision is managed here.
create or replace function public.bump_settings_revision()
returns trigger
language plpgsql
set search_path = ''
as $$
begin
  if new.armed is distinct from old.armed then
    new.revision := old.revision + 1;
  else
    new.revision := old.revision;
  end if;
  new.updated_at := now();
  return new;
end;
$$;

drop trigger if exists settings_revision on public.settings;
create trigger settings_revision
  before update on public.settings
  for each row execute function public.bump_settings_revision();

-- ---------------------------------------------------------------------
-- Device ingest: called by the ESP32 over HTTPS (PostgREST RPC).
-- Errors: wrong token -> HTTP 403 (device keeps the data and retries);
--         invalid payload -> HTTP 400 (device quarantines that packet).
-- ---------------------------------------------------------------------
create or replace function public.ingest_telemetry(p_device_id text, p_token text, p_payload jsonb)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_now        timestamptz := now();
  v_dev        public.devices%rowtype;
  v_set        public.settings%rowtype;
  v_latest     public.device_latest%rowtype;
  v_old        public.telemetry%rowtype;
  v_prev_low   public.telemetry%rowtype;
  v_fix        public.telemetry%rowtype;
  v_id         bigint;
  v_boot       text    := p_payload->>'boot_id';
  v_seq        bigint;
  v_mode       text    := p_payload->>'mode';
  v_replay     boolean := coalesce((p_payload->>'is_replay')::boolean, false);
  v_gps        boolean := coalesce((p_payload->>'gps_valid')::boolean, false);
  v_lat        double precision := (p_payload->>'latitude')::double precision;
  v_lon        double precision := (p_payload->>'longitude')::double precision;
  v_speed      real    := (p_payload->>'speed_kmh')::real;
  v_batt       real    := (p_payload->>'battery_v')::real;
  v_temp       real    := (p_payload->>'temperature_c')::real;
  v_tilt       real    := (p_payload->>'tilt_deg')::real;
  v_ign        boolean := (p_payload->>'ignition')::boolean;
  v_motion     boolean := coalesce((p_payload->>'motion')::boolean, false);
  v_fallen     boolean := coalesce((p_payload->>'fallen')::boolean, false);
  v_armed      boolean := coalesce((p_payload->>'armed')::boolean, false);
  v_sample     timestamptz;
  v_source     text;
  v_kinds      text[]  := '{}';
  v_kind       text;
  v_label      text;
  v_position   text;
  v_message    text;
begin
  -- Authenticate the device
  select * into v_dev from public.devices where id = p_device_id;
  if not found or v_dev.token_hash <> encode(extensions.digest(coalesce(p_token, ''), 'sha256'), 'hex') then
    raise exception 'invalid device credentials' using errcode = '28000';
  end if;

  -- Validate the payload (same rules as the original FastAPI server)
  if p_payload->>'device_id' is distinct from p_device_id then
    raise exception 'device_id mismatch' using errcode = '22023';
  end if;
  if v_boot is null or v_boot !~ '^[A-Za-z0-9_-]{1,64}$' then
    raise exception 'invalid boot_id' using errcode = '22023';
  end if;
  v_seq := (p_payload->>'seq')::bigint;
  if v_seq is null or v_seq < 0 or v_seq > 4294967295 then
    raise exception 'invalid seq' using errcode = '22023';
  end if;
  if v_mode is null or v_mode not in ('driving', 'parked', 'tracking', 'low_battery', 'bench') then
    raise exception 'invalid mode' using errcode = '22023';
  end if;
  if v_gps and (v_lat is null or v_lon is null) then
    raise exception 'gps_valid requires coordinates' using errcode = '22023';
  end if;
  if not v_gps and (v_lat is not null or v_lon is not null or v_speed is not null) then
    raise exception 'invalid GPS must use null coordinates and speed' using errcode = '22023';
  end if;
  if v_lat not between -90 and 90 or v_lon not between -180 and 180 or v_speed not between 0 and 350
     or v_batt not between 0 and 30 or v_temp not between -55 and 125 or v_tilt not between -180 and 180 then
    raise exception 'value out of range' using errcode = '22023';
  end if;
  if p_payload ? 'measured_at' and p_payload->>'measured_at' is not null then
    v_sample := (p_payload->>'measured_at')::timestamptz;
    if v_sample > v_now + interval '5 minutes' or v_sample < timestamptz '2025-01-01 00:00:00+00' then
      raise exception 'measured_at outside the valid range' using errcode = '22023';
    end if;
    v_source := 'device';
  else
    v_sample := v_now;                    -- no device clock yet: receive time (late for replays)
    v_source := 'received';
  end if;

  insert into public.settings (device_id) values (p_device_id) on conflict (device_id) do nothing;
  select * into v_set from public.settings where device_id = p_device_id;

  -- Previous live sample, needed for fall / low-battery transitions
  select * into v_latest from public.device_latest where device_id = p_device_id;
  if found and v_latest.telemetry_id is not null then
    select * into v_old from public.telemetry where id = v_latest.telemetry_id;
  end if;

  insert into public.telemetry (
    device_id, boot_id, seq, received_at, sample_at, time_source, is_replay, mode, armed, config_revision,
    ignition, battery_v, temperature_c, gps_valid, latitude, longitude, speed_kmh, satellites,
    acceleration_g, gyro_dps, motion, tilt_deg, fallen, signal_dbm, next_report_s, errors)
  values (
    p_device_id, v_boot, v_seq, v_now, v_sample, v_source, v_replay, v_mode, v_armed,
    coalesce((p_payload->>'config_revision')::integer, 0),
    v_ign, v_batt, v_temp, v_gps, v_lat, v_lon, v_speed, (p_payload->>'satellites')::integer,
    (p_payload->>'acceleration_g')::real, (p_payload->>'gyro_dps')::real, v_motion, v_tilt, v_fallen,
    (p_payload->>'signal_dbm')::integer, coalesce((p_payload->>'next_report_s')::integer, 5),
    coalesce(array(select jsonb_array_elements_text(p_payload->'errors')), '{}'))
  on conflict (device_id, boot_id, seq) do nothing
  returning id into v_id;

  if v_id is not null then
    -- Move the "current state" pointer only forward, and never for replayed packets
    if not v_replay and (v_latest.device_id is null or v_latest.boot_id is distinct from v_boot or v_latest.seq < v_seq) then
      insert into public.device_latest (device_id, telemetry_id, boot_id, seq, updated_at)
      values (p_device_id, v_id, v_boot, v_seq, v_now)
      on conflict (device_id) do update
        set telemetry_id = excluded.telemetry_id, boot_id = excluded.boot_id,
            seq = excluded.seq, updated_at = excluded.updated_at;
    end if;

    -- Alerts
    if v_motion and v_armed and v_ign is not true then
      v_kinds := array_append(v_kinds, 'movement');
    end if;
    if v_fallen and not (v_old.id is not null and v_old.boot_id = v_boot and v_old.fallen) then
      v_kinds := array_append(v_kinds, 'fall');  -- regardless of anti-theft, once per fall
    end if;
    if v_set.low_battery_v is not null and not v_replay and v_batt is not null and v_batt < v_set.low_battery_v
       and v_mode <> 'driving' and v_ign is not true then
      select * into v_prev_low from public.telemetry
        where device_id = p_device_id and boot_id = v_boot and seq < v_seq and not is_replay
        order by seq desc limit 1;
      if v_prev_low.id is not null and v_prev_low.battery_v < v_set.low_battery_v
         and v_prev_low.mode <> 'driving' and v_prev_low.ignition is not true then
        v_kinds := array_append(v_kinds, 'low_battery');  -- two consecutive engine-off samples, not a cranking dip
      end if;
    end if;

    foreach v_kind in array v_kinds loop
      continue when exists (select 1 from public.alerts
                            where device_id = p_device_id and kind = v_kind and created_at > v_now - interval '5 minutes');
      if v_gps then
        v_position := format('https://maps.google.com/?q=%s,%s', round(v_lat::numeric, 6), round(v_lon::numeric, 6));
      else
        select * into v_fix from public.telemetry
          where device_id = p_device_id and gps_valid and sample_at <= v_sample
          order by sample_at desc limit 1;
        v_position := case when v_fix.id is null then 'GPS chưa có vị trí hợp lệ.'
          else format('Vị trí cuối (%s): https://maps.google.com/?q=%s,%s',
                      to_char(v_fix.sample_at at time zone 'Asia/Ho_Chi_Minh', 'HH24:MI:SS DD/MM/YYYY'),
                      round(v_fix.latitude::numeric, 6), round(v_fix.longitude::numeric, 6)) end;
      end if;
      v_label := case v_kind
        when 'movement' then 'Chuyển động khi đang bật chống trộm'
        when 'fall' then case when v_tilt is null then 'Xe bị ngã'
                              else format('Xe bị ngã sang %s (nghiêng %s°)',
                                          case when v_tilt > 0 then 'trái' else 'phải' end, round(abs(v_tilt)::numeric)) end
        when 'low_battery' then 'Bình yếu'
      end;
      v_message := format(E'Xe: %s\nSự kiện: %s (%s)\nThời điểm đo: %s\nNhận lúc: %s\nGửi bù: %s\n%s',
        p_device_id, v_label, v_kind,
        case when v_source = 'device' then to_char(v_sample at time zone 'Asia/Ho_Chi_Minh', 'HH24:MI:SS DD/MM/YYYY (UTC+7)')
             else 'thiết bị chưa đồng bộ giờ' end,
        to_char(v_now at time zone 'Asia/Ho_Chi_Minh', 'HH24:MI:SS DD/MM/YYYY (UTC+7)'),
        case when v_replay then 'có (dữ liệu cũ)' else 'không' end,
        v_position);
      insert into public.alerts (device_id, kind, telemetry_id, message) values (p_device_id, v_kind, v_id, v_message);
    end loop;
  end if;

  -- Same reply shape the firmware already parses
  return jsonb_build_object(
    'accepted', true,
    'duplicate', v_id is null,
    'ack', jsonb_build_object('boot_id', v_boot, 'seq', v_seq),
    'server_time', to_char(v_now at time zone 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'),
    'settings', jsonb_build_object('armed', v_set.armed, 'revision', v_set.revision));
end;
$$;

-- Website button "Gửi email thử" (admins only)
create or replace function public.queue_test_alert(p_device_id text)
returns void
language plpgsql security definer
set search_path = ''
as $$
begin
  if not public.is_admin() then
    raise exception 'admins only' using errcode = '42501';
  end if;
  if exists (select 1 from public.alerts where device_id = p_device_id and kind = 'test'
             and created_at > now() - interval '1 minute') then
    raise exception 'test email sent less than a minute ago' using errcode = '22023';
  end if;
  insert into public.alerts (device_id, kind, message)
  values (p_device_id, 'test', format('Email thử từ MT15 Monitor (%s). Không phải cảnh báo thật.', p_device_id));
end;
$$;

-- ---------------------------------------------------------------------
-- Permissions and Row Level Security
-- ---------------------------------------------------------------------
revoke all on function public.ingest_telemetry(text, text, jsonb) from public;
grant execute on function public.ingest_telemetry(text, text, jsonb) to anon, authenticated;
revoke all on function public.queue_test_alert(text) from public;
grant execute on function public.queue_test_alert(text) to authenticated;
revoke all on function public.is_admin() from public;
grant execute on function public.is_admin() to authenticated;

alter table public.devices       enable row level security;
alter table public.settings      enable row level security;
alter table public.telemetry     enable row level security;
alter table public.device_latest enable row level security;
alter table public.alerts        enable row level security;
alter table public.admins        enable row level security;

-- Nobody reads the token hash through the API
revoke all on public.devices from anon, authenticated;
grant select (id, name, created_at) on public.devices to authenticated;
drop policy if exists "admin reads devices" on public.devices;
create policy "admin reads devices" on public.devices for select to authenticated using ((select public.is_admin()));

revoke all on public.settings from anon, authenticated;
grant select on public.settings to authenticated;
grant update (armed, low_battery_v) on public.settings to authenticated;
drop policy if exists "admin reads settings" on public.settings;
create policy "admin reads settings" on public.settings for select to authenticated using ((select public.is_admin()));
drop policy if exists "admin updates settings" on public.settings;
create policy "admin updates settings" on public.settings for update to authenticated
  using ((select public.is_admin())) with check ((select public.is_admin()));

revoke all on public.telemetry, public.device_latest, public.alerts, public.admins from anon, authenticated;
grant select on public.telemetry, public.device_latest, public.alerts, public.admins to authenticated;
drop policy if exists "admin reads telemetry" on public.telemetry;
create policy "admin reads telemetry" on public.telemetry for select to authenticated using ((select public.is_admin()));
drop policy if exists "admin reads latest" on public.device_latest;
create policy "admin reads latest" on public.device_latest for select to authenticated using ((select public.is_admin()));
drop policy if exists "admin reads alerts" on public.alerts;
create policy "admin reads alerts" on public.alerts for select to authenticated using ((select public.is_admin()));
drop policy if exists "user reads own admin row" on public.admins;
create policy "user reads own admin row" on public.admins for select to authenticated using (user_id = (select auth.uid()));

-- ---------------------------------------------------------------------
-- Realtime: the dashboard updates the moment a packet arrives
-- ---------------------------------------------------------------------
do $$
declare t text;
begin
  foreach t in array array['telemetry', 'device_latest', 'alerts', 'settings'] loop
    if not exists (select 1 from pg_publication_tables
                   where pubname = 'supabase_realtime' and schemaname = 'public' and tablename = t) then
      execute format('alter publication supabase_realtime add table public.%I', t);
    end if;
  end loop;
end;
$$;

-- ---------------------------------------------------------------------
-- Optional: keep 30 days of history (enable the pg_cron extension first,
-- Dashboard → Database → Extensions → pg_cron), then run:
--
-- select cron.schedule('mt15-retention', '17 3 * * *', $job$
--   delete from public.telemetry t
--    where t.received_at < now() - interval '30 days'
--      and not exists (select 1 from public.device_latest l where l.telemetry_id = t.id);
--   delete from public.alerts where created_at < now() - interval '30 days' and mail_status <> 'pending';
-- $job$);
-- ---------------------------------------------------------------------
