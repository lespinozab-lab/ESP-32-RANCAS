# deja todo listo pa la etapa 3: los datos de los 3 equipos en una base de datos de supabase
# y una pagina publica q cualquiera puede abrir, genera:
#   - ../equipo_iot_etapa2/claves_nube.h  direccion del proyecto, clave publica y una clave secreta por equipo
#   - 1_estructura_supabase.sql           lo que se pega una sola vez en Supabase (tablas,seguridad y consultas)
#   - publicar/index.html                 la pagina publica: esa carpeta es la q se sube a internet
#
# uso: python preparar_nube.py                        pide la direccion y la clave (Enter = dejar las q ya estan)
#      python preparar_nube.py URL CLAVE_PUBLICA
#      python preparar_nube.py --prueba URL CLAVE SALIDA.html   nomas arma una pagina de prueba (no toca nada mas)
import re
import secrets
import sys
from pathlib import Path

AQUI = Path(__file__).resolve().parent
WEB = AQUI.parent
CLAVES = WEB / "equipo_iot_etapa2" / "claves_nube.h"
MASCOTAS = ["Ajolote", "Nutria", "Pinguino"]


def leer_claves():
    if not CLAVES.exists():
        return {"url": "", "publica": "", "equipos": [secrets.token_urlsafe(18) for _ in MASCOTAS]}
    t = CLAVES.read_text(encoding="utf-8")
    return {"url": re.search(r'NUBE_URL = "([^"]*)"', t).group(1),
            "publica": re.search(r'NUBE_CLAVE_PUBLICA = "([^"]*)"', t).group(1),
            "equipos": re.findall(r'"([^"]+)"', re.search(r"NUBE_CLAVES\[3\] = \{([^}]*)\}", t).group(1))}


def guardar_claves(c):
    CLAVES.write_text(
        "// datos pa subir las lecturas a la nube (supabase), lo arma nube/preparar_nube.py asi q no editar a mano\n"
        "// ojo no se sube a ningun lado, tiene la clave secreta de cada equipo\n"
        "//mientras NUBE_URL este vacia el equipo no manda nada a la nube\n"
        f'const char* NUBE_URL = "{c["url"]}";\n'
        f'const char* NUBE_CLAVE_PUBLICA = "{c["publica"]}";\n'
        "const char* NUBE_CLAVES[3] = { " + ", ".join(f'"{k}"' for k in c["equipos"]) + " };   // la misma de cada equipo en la base\n",
        encoding="utf-8", newline="\n")


def guardar_sql(c):
    valores = ",\n  ".join(f"({i + 1}, '{m}', '{k}')" for i, (m, k) in enumerate(zip(MASCOTAS, c["equipos"])))
    (AQUI / "1_estructura_supabase.sql").write_text(f"""-- monitoreo del rio San Juan de Rancas: la base de datos en supabase
-- pegar TODO en Supabase > SQL Editor > New query y darle Run, se puede correr otra vez sin perder datos
-- ojo este archivo trae la clave secreta de cada equipo, no subirlo a internet ni pasarlo

-- los 3 equipos y su clave secreta (la misma q tiene su programa en claves_nube.h)
create table if not exists public.equipos (
  numero smallint primary key,
  mascota text not null,
  clave text not null
);
insert into public.equipos (numero, mascota, clave) values
  {valores}
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
""", encoding="utf-8", newline="\n")


def armar_pagina(url, publica, salida):
    html = (WEB / "index.html").read_text(encoding="utf-8")
    for viejo, nuevo in (('fuente: "demo",', 'fuente: "nube",'), ('urlNube: "",', f'urlNube: "{url}",'), ('clavePublica: "",', f'clavePublica: "{publica}",')):
        assert html.count(viejo) == 1, "No se encontro en index.html: " + viejo
        html = html.replace(viejo, nuevo)
    salida.parent.mkdir(parents=True, exist_ok=True)
    salida.write_text(html, encoding="utf-8", newline="\n")


def limpiar_url(url):
    url = url.strip().rstrip("/")
    if url and not re.fullmatch(r"https://[a-z0-9-]+\.supabase\.co|http://localhost:\d+", url):
        sys.exit("La direccion no parece de Supabase: debe ser como https://abcdefgh.supabase.co")
    return url


if __name__ == "__main__":
    if sys.argv[1:2] == ["--prueba"]:
        armar_pagina(limpiar_url(sys.argv[2]), sys.argv[3], Path(sys.argv[4]))
        sys.exit(0)
    c = leer_claves()
    if len(sys.argv) >= 3:
        c["url"], c["publica"] = limpiar_url(sys.argv[1]), sys.argv[2].strip()
    else:
        print("Datos del proyecto de Supabase (Project Settings > API). Enter = dejar lo que ya hay.")
        url = input(f"  Direccion del proyecto (Project URL) [{c['url'] or 'sin poner'}]: ").strip()
        publica = input("  Clave publica (anon public o publishable) [" + ("ya puesta" if c["publica"] else "sin poner") + "]: ").strip()
        if url: c["url"] = limpiar_url(url)
        if publica: c["publica"] = publica
    guardar_claves(c)
    guardar_sql(c)
    armar_pagina(c["url"], c["publica"], AQUI.parents[1] / "rio-san-juan" / "index.html")
    print("Listo:")
    print("  - 1_estructura_supabase.sql (pegar en Supabase si todavia no se hizo)")
    print("  - publicar/index.html", "(conectada a " + c["url"] + ")" if c["url"] else "(todavia SIN conectar: falta la direccion)")
    print("  - claves de los equipos actualizadas: falta volver a subir el programa a los tres")
