//imita las respuestas de los tres ESP32 de la etapa 2 pa probar la pagina sin los equipos
// uso: node equipo_falso.js <ruta a pagina.h> [puerto]
// si no se indica el puerto, usa la variable de entorno PORT y,si tampoco existe, el 8765
//el equipo 1 (ajolote) queda en ese puerto, el 2 (nutria) en el siguiente y el 3 (pinguino) en el q sigue
// para cambiar lo q le pasa a un equipo: /__estado?x=phbajo&n=2
//   x: normal | alerta (agua turbia) | phbajo | phalto | caido (no contesta) | sinsensor   n: 1, 2 o 3 (si falta, el 1)
//en /demo entrega index.html tal cual, con sus datos simulados (etapa 1)
const http = require("http");
const fs = require("fs");
const path = require("path");

//pagina.h trae la pagina comprimida (gzip), como una lista de bytes: se entrega igual que lo hace el equipo
const h = fs.readFileSync(process.argv[2], "utf8");
const pagina = Buffer.from(h.slice(h.indexOf("{") + 1, h.lastIndexOf("}")).match(/0x[0-9a-f]{2}/g).map(b => parseInt(b, 16)));
const puerto = Number(process.argv[3] || process.env.PORT || 8765);
const inicio = Date.now();
const t = x => (x > -100 ? x.toFixed(1) : "null");
const num = (x, dec) => (x === null ? "null" : x.toFixed(dec));  // lo que no se mide viaja como null

const equipos = [0, 1, 2].map(i => ({
  i, estado: "normal", n: 0, hist: [],
  v: { temp: [10.8, 11.5, 9.9][i], ph: [7.6, 7.3, 7.9][i], ntu: [22, 30, 15][i], ce: [350, 420, 290][i] }
}));

function medir(e) {
  const paso = (x, d, min, max) => Math.min(max, Math.max(min, x + (Math.random() - 0.5) * d));
  const v = e.v;
  v.temp = paso(v.temp, 0.15, 8, 14);
  v.ph = paso(v.ph, 0.05, 7.1, 8.1);
  v.ce = paso(v.ce, 6, 250, 480);
  v.ntu = e.estado === "alerta" ? paso(180, 30, 140, 230) : paso(Math.min(v.ntu, 45), 4, 8, 45);
  const ph = e.estado === "phbajo" ? paso(5.3, 0.3, 5, 5.6) : e.estado === "phalto" ? paso(9.3, 0.3, 9, 9.6) : v.ph;
  const sin = e.estado === "sinsensor";
  e.hist.push({ ms: Date.now(), temp: sin ? -127 : v.temp, ph: sin ? null : ph, ntu: sin ? null : v.ntu, ce: sin ? null : v.ce });
  if (e.hist.length > 60) e.hist.shift();
  e.n++;
}
equipos.forEach(e => {
  for (let i = 0; i < 45; i++) { medir(e); e.hist[e.hist.length - 1].ms = Date.now() - (45 - i) * 2000; }
  setInterval(() => medir(e), 2000);
});

// cada equipo sabe donde estan los otros mientras no esten caidos, igual que los de verdad
const direcciones = () => equipos.map(o => (o.estado === "caido" ? '""' : `"localhost:${puerto + o.i}"`)).join(",");

equipos.forEach(e => http.createServer((req, res) => {
  const url = new URL(req.url, "http://x");
  if (url.pathname === "/__estado") {
    const cual = equipos[(Number(url.searchParams.get("n")) || 1) - 1] || e;
    cual.estado = url.searchParams.get("x");
    res.writeHead(200, { "Access-Control-Allow-Origin": "*" });
    res.end("equipo " + (cual.i + 1) + ": " + cual.estado);
    return;
  }
  if (e.estado === "caido") { req.socket.destroy(); return; }
  const l = e.hist[e.hist.length - 1];
  if (url.pathname === "/") {
    res.writeHead(200, { "Content-Type": "text/html; charset=utf-8", "Content-Encoding": "gzip" }); res.end(pagina);
  } else if (url.pathname === "/demo") {
    res.writeHead(200, { "Content-Type": "text/html; charset=utf-8" });
    res.end(fs.readFileSync(path.join(path.dirname(process.argv[2]), "..", "index.html")));
  } else if (url.pathname === "/datos") {
    res.writeHead(200, { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" });
    res.end(`{"equipo":"equipo${e.i + 1}","nombre":"Equipo ${e.i + 1}","n":${e.n},"temp":${t(l.temp)},"ph":${num(l.ph, 2)},"ntu":${num(l.ntu, 0)},"ce":${num(l.ce, 0)},` +
            `"wifi":true,"internet":true,"encendido":${Math.floor((Date.now() - inicio) / 1000)},"respaldo":true,"kb":128,"sd":false,"reloj":true,"gps":"si",` +
            `"lat":${(-10.6847 - e.i * 0.004).toFixed(6)},"lng":${(-76.3105 + e.i * 0.003).toFixed(6)},"ip":"192.168.43.${57 + e.i}","nodo":${e.i + 1},"ips":[${direcciones()}]}`);
  } else if (url.pathname === "/historial") {
    res.writeHead(200, { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" });
    const ahora = Date.now();
    res.end(`{"n":${e.n},"lecturas":[` + e.hist.map(x => `[${Math.floor((ahora - x.ms) / 1000)},${t(x.temp)},${num(x.ph, 2)},${num(x.ntu, 0)}]`).join(",") + "]}");
  } else if (url.pathname === "/datos.csv") {
    //lo "guardado": las lecturas q tiene en memoria,con el mismo formato que el equipo
    res.writeHead(200, { "Content-Type": "text/csv", "Access-Control-Allow-Origin": "*",
                         "Content-Disposition": `attachment; filename=datos_rio_san_juan_equipo${e.i + 1}.csv` });
    res.end("fecha,hora,temperatura_C,pH,turbidez_NTU,turbidez_V,pH_mV,conductividad_uS,conductividad_mV,latitud,longitud,origen\r\n" +
            e.hist.map(x => {
              const d = new Date(x.ms - 5 * 3600000).toISOString();  //hora de Peru
              return [d.slice(0, 10), d.slice(11, 19), x.temp.toFixed(1), x.ph === null ? "15.34" : x.ph.toFixed(2), x.ntu === null ? "3000" : x.ntu.toFixed(0),
                      x.ntu === null ? "0.21" : "1.90", "1600", x.ce === null ? "" : x.ce.toFixed(0), "300", "", "", "auto"].join(",");
            }).join("\r\n") + "\r\n");
  } else if (url.pathname === "/resumen.csv") {
    // un mes de resumenes por hora, siempre los mismos para cada equipo
    res.writeHead(200, { "Content-Type": "text/csv", "Access-Control-Allow-Origin": "*" });
    let semilla = 11 + e.i * 97;
    const azar = () => (semilla = (semilla * 16807) % 2147483647) / 2147483647;
    const ahora = Math.floor(Date.now() / 3600000) * 3600000, filas = [];
    for (let k = 30 * 24; k >= 0; k--) {
      const d = new Date(ahora - k * 3600000 - 5 * 3600000).toISOString();
      const ph = [7.6, 7.3, 7.9][e.i] + 0.25 * Math.sin(k / 37 + e.i) + (azar() - 0.5) * 0.15;
      const ntu = [22, 30, 15][e.i] + 8 * Math.sin(k / 53) + azar() * 6 + (azar() > 0.985 ? 150 : 0);
      filas.push([d.slice(0, 10), d.slice(11, 13) + ":00", 1800, (10 + e.i + 2 * Math.sin(k / 3.82)).toFixed(1), ph.toFixed(2), (ph - 0.08).toFixed(2),
                  (ph + 0.08).toFixed(2), ntu.toFixed(0), (ntu * 1.2).toFixed(0), ([350, 420, 290][e.i] + 20 * Math.sin(k / 90)).toFixed(0)].join(","));
    }
    res.end("fecha,hora,lecturas,temperatura_C,pH,pH_min,pH_max,turbidez_NTU,turbidez_max,conductividad_uS\r\n" + filas.join("\r\n") + "\r\n");
  } else { res.writeHead(404); res.end("Not found"); }
}).listen(puerto + e.i, () => console.log("equipo falso", e.i + 1, "en http://localhost:" + (puerto + e.i))));
