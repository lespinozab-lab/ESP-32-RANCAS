// imita la base de datos de Supabase (nomas las consultas que usa la pagina publica) para probarla sin internet
// uso: node nube_falsa.js [puerto]      (por defecto 8799)
// despues: python nube/preparar_nube.py --prueba http://localhost:8799 clave-de-prueba salida.html
//para dejar callado a un equipo (q no "suba" nada): /__callar?n=2
const http = require("http");

const puerto = Number(process.argv[2] || 8799);
const inicio = Date.now();
const callados = new Set();
let siguienteId = 1;

// lecturas inventadas cada 20 s de los ultimos 2 dias, como las que subirian los equipos
const lecturas = [];
const base = [{ temp: 10.8, ph: 7.6, ntu: 22, ce: 350 }, { temp: 11.5, ph: 7.3, ntu: 30, ce: 420 }, { temp: 9.9, ph: 7.9, ntu: 15, ce: 290 }];
function lectura(equipo, ms) {
  const b = base[equipo - 1], k = ms / 60000;
  return { id: siguienteId++, equipo, fecha: new Date(ms).toISOString(),
           temp: +(b.temp + 1.5 * Math.sin(k / 240) + Math.random() * 0.2).toFixed(1),
           ph: +(equipo === 2 && k % 600 < 40 ? 5.4 : b.ph + 0.2 * Math.sin(k / 90 + equipo) + Math.random() * 0.05).toFixed(2),
           ntu: Math.round(b.ntu + 6 * Math.sin(k / 70) + Math.random() * 4), ce: Math.round(b.ce + 15 * Math.sin(k / 300)),
           lat: -10.6847 - (equipo - 1) * 0.004, lng: -76.3105 + (equipo - 1) * 0.003, gps: "si", encendido: Math.round((ms - inicio) / 1000) + 3600 };
}
for (let ms = inicio - 2 * 86400000; ms <= inicio; ms += 20000) [1, 2, 3].forEach(e => lecturas.push(lectura(e, ms)));
setInterval(() => [1, 2, 3].forEach(e => { if (!callados.has(e)) lecturas.push(lectura(e, Date.now())); }), 20000);

// filtros de la forma ?equipo=eq.1&fecha=gte.2026-...&order=fecha.asc&limit=1000&offset=0
function filtrar(filas, q) {
  let r = filas;
  if (q.get("equipo")) r = r.filter(f => f.equipo === Number(q.get("equipo").replace("eq.", "")));
  if (q.get("fecha")) { const desde = new Date(q.get("fecha").replace("gte.", "")).getTime(); r = r.filter(f => new Date(f.fecha).getTime() >= desde); }
  const desde = Number(q.get("offset") || 0), cuantas = Math.min(1000, Number(q.get("limit") || 1000));
  return r.slice(desde, desde + cuantas);
}

function resumenPorHora(equipo) {
  const horas = new Map();
  lecturas.filter(f => f.equipo === equipo).forEach(f => {
    const lima = new Date(new Date(f.fecha).getTime() - 5 * 3600000).toISOString().slice(0, 13) + ":00:00";
    if (!horas.has(lima)) horas.set(lima, []);
    horas.get(lima).push(f);
  });
  const prom = (g, k) => g.reduce((a, f) => a + f[k], 0) / g.length;
  return [...horas].map(([hora, g]) => ({ hora, lecturas: g.length, temp: prom(g, "temp"), ph: prom(g, "ph"), ph_min: Math.min(...g.map(f => f.ph)),
                                          ph_max: Math.max(...g.map(f => f.ph)), ntu: prom(g, "ntu"), ntu_max: Math.max(...g.map(f => f.ntu)), ce: prom(g, "ce") }));
}

http.createServer((req, res) => {
  const url = new URL(req.url, "http://x"), q = url.searchParams;
  const cabeceras = { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Headers": "apikey, authorization, content-type" };
  if (req.method === "OPTIONS") { res.writeHead(204, cabeceras); res.end(); return; }
  if (url.pathname === "/__callar") { callados.add(Number(q.get("n"))); res.writeHead(200, cabeceras); res.end("ok"); return; }
  if (!req.headers.apikey) { res.writeHead(401, cabeceras); res.end('{"message":"No API key found in request"}'); return; }
  let cuerpo;
  if (url.pathname === "/rest/v1/ultima_lectura") {
    cuerpo = [1, 2, 3].map(e => lecturas.filter(f => f.equipo === e).pop());
  } else if (url.pathname === "/rest/v1/lecturas") {
    cuerpo = filtrar(lecturas, q);
  } else if (url.pathname === "/rest/v1/rpc/resumen_por_hora") {
    const desde = Number(q.get("offset") || 0);
    cuerpo = resumenPorHora(Number(q.get("p_equipo"))).slice(desde, desde + Math.min(1000, Number(q.get("limit") || 1000)));
  } else { res.writeHead(404, cabeceras); res.end("{}"); return; }
  res.writeHead(200, { ...cabeceras, "Content-Type": "application/json" });
  res.end(JSON.stringify(cuerpo));
}).listen(puerto, () => console.log("nube falsa en http://localhost:" + puerto));
