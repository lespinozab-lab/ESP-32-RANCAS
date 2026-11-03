# mete la pagina (el index.html de la carpeta de arriba) dentro del codigo del equipo (pagina.h)
# correrlo cada vez q cambie la pagina y dsp volver a subir el codigo a los ESP32

import gzip
import re
from pathlib import Path

aqui = Path(__file__).parent
html = (aqui.parent / "index.html").read_text(encoding="utf-8")

assert html.count('fuente: "demo",') == 1, 'Se esperaba una sola linea  fuente: "demo",  en index.html'
html = html.replace('fuente: "demo",', 'fuente: "local",')

# la copia va sin sangria, sin lineas en blanco y sin comentarios - solo se quitan los comentarios q
# ocupan toda la linea y los q van al final con 2 espacios o mas antes (asi no se toca un "http://"
# q este dentro de un texto), el index.html queda igual con todo pa leerlo
lineas = []
for linea in html.split("\n"):
    linea = linea.strip()
    if not linea or linea.startswith("//") or (linea.startswith("/*") and linea.endswith("*/")):
        continue
    lineas.append(re.sub(r"\s{2,}//.*$", "", linea))
compacta = ("\n".join(lineas) + "\n").encode("utf-8")
assert compacta.count(b"://") == html.count("://"), "Se borro por error algo que no era un comentario"

# la pagina va comprimida (gzip) pa q ocupe menos, el navegador la descomprime solito
comprimida = gzip.compress(compacta, 9, mtime=0)
filas = [", ".join("0x%02x" % b for b in comprimida[i:i + 20]) for i in range(0, len(comprimida), 20)]
(aqui / "pagina.h").write_text(
    "// copia comprimida (gzip) de index.html, no editar aca sino correr actualizar_pagina.py\n"
    "const uint8_t PAGINA_GZ[] PROGMEM = {\n  " + ",\n  ".join(filas) + "\n};\n"
    "const size_t PAGINA_GZ_LARGO = sizeof(PAGINA_GZ);\n",
    encoding="utf-8", newline="\n")
print("pagina.h actualizado: index.html tiene", len(html.encode("utf-8")), "bytes; sin comentarios,", len(compacta),
      "; comprimida,", len(comprimida), "bytes")
