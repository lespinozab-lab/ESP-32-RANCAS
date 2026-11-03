-- monitoreo del rio San Juan de Rancas: la base de datos en supabase
-- pegar TODO en Supabase > SQL Editor > New query y darle Run, se puede correr otra vez sin perder datos
-- ojo este archivo trae la clave secreta de cada equipo, no subirlo a internet ni pasarlo

-- los 3 equipos y su clave secreta (la misma q tiene su programa en claves_nube.h)
create table if not exists public.equipos (
  numero smallint primary key,
  mascota text not null,
  clave text not null
);
insert into public.equipos (numero, mascota, clave) values
  (1, 'Ajolote', 'wyIYiEFKyxjvzOqOTNLfCV-Q'),
  (2, 'Nutria', 'moOVfJijnSPFKxkCnCVdNESw'),
  (3, 'Pinguino', 'hiuMZ6St1iu-QUPa5P3eyzHT')
on conflict (numero) do update set mascota = excluded.mascota, clave = excluded.clave;

-- una fila x lectura, la fecha la pone la base cuando le llega
create table if not exists public.lecturas (
  id bigint generated always as identity primary key,
  equipo smallint not null references public.equipos (numero),
  fecha timestamptz not null default now(),
  temp real,
  ph real,
  ntu real,
  ce real,
  lat double precision,
  lng double precision,
  gps text,
  encendido integer
);
create index if not exists lecturas_equipo_fecha on public.lecturas (equipo, fecha desc);

-- seguridad: cualquiera puede LEER las lecturas (la pagina es publica) pero nadie las escribe
-- directo y nadie ve las claves de los equipos
alter table public.equipos enable row level security;
alter table public.lecturas enable row level security;
revoke all on public.equipos from anon, authenticated;
revoke all on public.lecturas from anon, authenticated;
grant select on public.lecturas to anon, authenticated;
drop policy if exists lectura_publica on public.lecturas;
create policy lectura_publica on public.lecturas for select to anon, authenticated using (true);

--los equipos escriben solo con esta funcion y solo si mandan su clave
create or replace function public.registrar_lectura(
  p_equipo smallint, p_clave text,
  p_temp real default null, p_ph real default null, p_ntu real default null, p_ce real default null,
  p_lat double precision default null, p_lng double precision default null,
  p_gps text default null, p_encendido integer default null)
returns void
language plpgsql
security definer
set search_path = public
as $$
begin
  if not exists (select 1 from public.equipos where numero = p_equipo and clave = p_clave) then
    raise exception 'equipo o clave no validos' using errcode = '28000';
  end if;
  insert into public.lecturas (equipo, temp, ph, ntu, ce, lat, lng, gps, encendido)
  values (p_equipo, p_temp, p_ph, p_ntu, p_ce, p_lat, p_lng, p_gps, p_encendido);
end;
$$;
revoke all on function public.registrar_lectura(smallint, text, real, real, real, real, double precision, double precision, text, integer) from public;
grant execute on function public.registrar_lectura(smallint, text, real, real, real, real, double precision, double precision, text, integer) to anon, authenticated;

-- la ultima lectura de cada equipo (la pagina la pide cada 10 seg), se busca equipo x equipo
-- en el indice asi tarda igual con mil filas q con millones
create or replace view public.ultima_lectura with (security_invoker = true) as
  select l.id, l.equipo, l.fecha, l.temp, l.ph, l.ntu, l.ce, l.lat, l.lng, l.gps, l.encendido
  from (values (1::smallint), (2::smallint), (3::smallint)) as e (numero)
  cross join lateral (
    select * from public.lecturas where equipo = e.numero order by fecha desc limit 1
  ) l;
grant select on public.ultima_lectura to anon, authenticated;

-- promedios de cada hora en hora de peru, de aca sale el registro x dias, semanas y meses de la pagina
create or replace function public.resumen_por_hora(p_equipo smallint, p_desde timestamptz default now() - interval '400 days')
returns table (hora timestamp, lecturas bigint, temp double precision, ph double precision, ph_min real, ph_max real,
               ntu double precision, ntu_max real, ce double precision)
language sql
stable
set search_path = public
as $$
  select date_trunc('hour', l.fecha at time zone 'America/Lima'), count(*), avg(l.temp), avg(l.ph), min(l.ph), max(l.ph),
         avg(l.ntu), max(l.ntu), avg(l.ce)
  from public.lecturas l
  where l.equipo = p_equipo and l.fecha >= p_desde
  group by 1
  order by 1;
$$;
grant execute on function public.resumen_por_hora(smallint, timestamptz) to anon, authenticated;

-- pa q la base vuelva a leer lo q se acaba de crear
notify pgrst, 'reload schema';
