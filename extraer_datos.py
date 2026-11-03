# baja todo lo guardado en los 3 equipos y lo deja en la carpeta datos/
#   - el CSV de cada equipo, tal como sale de su memoria
#   - un solo Excel con una hoja por punto,una de comparacion, otra de graficos y una de notas
# uso: python extraer_datos.py                      (baja de los equipos x WiFi)
#      python extraer_datos.py p1.csv p2.csv p3.csv (arma el Excel con CSV ya descargados, en el orden de los puntos)
#      python extraer_datos.py archivo.csv          (un solo punto)

import csv
import json
import sys
import urllib.request
from datetime import datetime
from pathlib import Path

from openpyxl import Workbook
from openpyxl.chart import Reference, ScatterChart, Series
from openpyxl.styles import Alignment, Font, PatternFill
from openpyxl.utils import get_column_letter

PUNTOS = ["Punto 1 - Ajolote", "Punto 2 - Nutria", "Punto 3 - Pinguino"]
# donde buscar cada equipo: su nombre en el wifi del celular, el 1 tmb se prueba por su red propia
DIRECCIONES = [["http://riosanjuan.local", "http://192.168.4.1"], ["http://riosanjuan2.local"], ["http://riosanjuan3.local"]]
COLORES = ["D6336C", "9A6234", "3F5677"]   # los mismos de la pagina

#calibracion vigente (los mismos valores q en equipo_iot_etapa2.ino, hoy es una sola pa los tres equipos)
PH7_MV = 1598
PH4_MV = 2122
PH_CALIBRADO_DESDE = datetime(2026, 10, 8, 10, 17, 0)
# antes de esta fecha el modulo de turbidez del equipo 1 estaba en D y sin calibrar -> esas lecturas no sirven
TURBIDEZ_VALIDA_DESDE = datetime(2026, 10, 9, 23, 57, 30)

CARPETA = Path(__file__).resolve().parents[2] / "datos" / "esp32-csv"
FUENTE = "Arial"
MEDIDAS = ["temperatura_C", "pH", "turbidez_NTU", "turbidez_V", "pH_mV",
           "conductividad_uS", "conductividad_mV", "latitud", "longitud"]


def pedir(url, espera):
    with urllib.request.urlopen(url, timeout=espera) as r:
        return r.read()


def bajar():
    """baja el CSV de cada equipo q conteste - devuelve una lista de tres rutas (None donde no hubo respuesta)"""
    CARPETA.mkdir(exist_ok=True)
    sello = datetime.now().strftime("%Y-%m-%d_%H%M")
    direcciones = [list(d) for d in DIRECCIONES]
    for equipo in sum(DIRECCIONES, []):  # cualquiera de los tres sabe la direccion de los otros
        try:
            for i, ip in enumerate(json.loads(pedir(equipo + "/datos", 6)).get("ips", [])):
                if ip:
                    direcciones[i].insert(0, "http://" + ip)
            break
        except (OSError, ValueError):
            pass
    archivos = []
    for i, candidatos in enumerate(direcciones):
        destino = None
        for equipo in candidatos:
            try:
                datos = pedir(equipo + "/datos.csv", 180)
                destino = CARPETA / f"datos_equipo{i + 1}_{sello}.csv"
                destino.write_bytes(datos)
                print(f"{PUNTOS[i]}: descargado de {equipo} ({len(datos) // 1024} KB)")
                break
            except OSError as e:
                print(f"{PUNTOS[i]}: sin respuesta en {equipo} - {e}")
        archivos.append(destino)
    if not any(archivos):
        sys.exit("No se pudo conectar con ningun equipo. Revise que la laptop este en su misma red WiFi.")
    return archivos


def leer(archivo_csv):
    # el archivo puede traer dos tramos con titulos distintos: las lecturas de antes y de despues
    # de instalar el sensor de conductividad, cada fila se lee con los titulos de su tramo
    filas, titulos = [], None
    with open(archivo_csv, encoding="utf-8", errors="replace", newline="") as f:
        for campos in csv.reader(f):
            if not campos:
                continue
            if campos[0] == "fecha":
                titulos = campos
            elif titulos:
                filas.append(dict(zip(titulos, campos)))
    return filas


def numero(texto):
    try:
        return float(texto)
    except (TypeError, ValueError):
        return None


def letra(c, tam=10, negrita=False, color=None):
    c.font = Font(name=FUENTE, size=tam, bold=negrita, color=color)


def hoja_de_datos(libro, titulo, filas):
    """una hoja con las lecturas de un punto - columnas A-N como siempre y O = fecha y hora juntas (para los graficos)"""
    datos = libro.create_sheet(titulo)
    datos.append(["fecha", "hora"] + MEDIDAS + ["origen", "pH_recalculado", "turbidez_valida", "momento"])
    for i, f in enumerate(filas, start=2):
        try:
            momento = datetime.strptime(f["fecha"] + " " + f["hora"], "%Y-%m-%d %H:%M:%S")
            fecha, hora = momento.date(), momento.time()
        except ValueError:   #filas guardadas sin reloj ni internet
            fecha, hora = f["fecha"], f["hora"]
        datos.append([fecha, hora] + [numero(f.get(m)) for m in MEDIDAS] + [
                      f.get("origen"),
                      f'=IF(ISNUMBER(G{i}),ROUND(7+(Notas!$B$8-G{i})*3/(Notas!$B$9-Notas!$B$8),2),"")',
                      f'=IF(ISNUMBER(A{i}),IF(A{i}+B{i}>=Notas!$B$10,"si","no"),"sin hora")',
                      f'=IF(ISNUMBER(A{i}),A{i}+B{i},"")'])
    ultima = len(filas) + 1

    formatos = {"A": "yyyy-mm-dd", "B": "hh:mm:ss", "C": "0.0", "D": "0.00", "E": "0", "F": "0.00", "G": "0",
                "H": "0", "I": "0", "J": "0.000000", "K": "0.000000", "M": "0.00", "O": "yyyy-mm-dd hh:mm"}
    anchos = [12, 10, 15, 8, 14, 12, 9, 18, 18, 12, 12, 9, 16, 16, 18]
    for n, ancho in enumerate(anchos, start=1):
        datos.column_dimensions[get_column_letter(n)].width = ancho
    for fila in datos.iter_rows(min_row=1, max_row=ultima):
        for c in fila:
            letra(c, negrita=(c.row == 1))
            if c.row > 1 and c.column_letter in formatos:
                c.number_format = formatos[c.column_letter]
    for c in datos[1]:
        c.fill = PatternFill("solid", start_color="DDEBF7")
        c.alignment = Alignment(horizontal="center")
    datos.freeze_panes = "A2"
    datos.auto_filter.ref = f"A1:O{ultima}"
    return ultima


def a_excel(archivos):
    """archivos: hasta tres CSV, en el orden de los puntos (None para el punto q falte)"""
    puntos = [(PUNTOS[i], a, leer(a)) for i, a in enumerate(archivos) if a]
    puntos = [(titulo, a, filas) for titulo, a, filas in puntos if filas]
    if not puntos:
        sys.exit("Los archivos no tienen lecturas.")

    libro = Workbook()
    comparacion = libro.active
    comparacion.title = "Comparacion"
    ultimas = {titulo: hoja_de_datos(libro, titulo, filas) for titulo, _, filas in puntos}

    # ---- comparacion -> una fila por punto, todo con formulas sobre su hoja ----
    comparacion.append(["Rio San Juan de Rancas - comparacion de los puntos"])
    comparacion.append([])
    comparacion.append(["Punto", "Lecturas", "Primera lectura", "Ultima lectura", "Temperatura promedio (C)",
                        "pH promedio", "Lecturas con pH valido", "pH bajo 6.5", "pH sobre 8.5", "% de pH fuera de rango",
                        "Turbidez promedio (NTU)", "Lecturas con turbidez valida", "Turbidez sobre 100",
                        "% de turbidez sobre 100", "Conductividad promedio (uS/cm)"])
    for titulo, _, _ in puntos:
        h, u = f"'{titulo}'", ultimas[titulo]
        f = comparacion.max_row + 1
        rango = lambda col: f"{h}!{col}2:{col}{u}"
        valida = f'{rango("N")},"si",{rango("F")},">0.7"'   # turbidez medida con el modulo bien puesto
        comparacion.append([
            titulo,
            f"=COUNTA({rango('A')})",
            f"=MIN({rango('O')})",
            f"=MAX({rango('O')})",
            f'=IFERROR(AVERAGEIFS({rango("C")},{rango("C")},">-100"),"")',
            f'=IFERROR(AVERAGEIFS({rango("M")},{rango("M")},">=0",{rango("M")},"<=14"),"")',
            f'=COUNTIFS({rango("M")},">=0",{rango("M")},"<=14")',
            f'=COUNTIFS({rango("M")},">=0",{rango("M")},"<6.5")',
            f'=COUNTIFS({rango("M")},">8.5",{rango("M")},"<=14")',
            f'=IF(G{f}>0,(H{f}+I{f})/G{f},"")',
            f'=IFERROR(AVERAGEIFS({rango("E")},{valida}),"")',
            f"=COUNTIFS({valida})",
            f'=COUNTIFS({valida},{rango("E")},">100")',
            f'=IF(L{f}>0,M{f}/L{f},"")',
            f'=IFERROR(AVERAGEIFS({rango("H")},{rango("H")},">0"),"")',
        ])
    formatos = {3: "yyyy-mm-dd hh:mm", 4: "yyyy-mm-dd hh:mm", 5: "0.0", 6: "0.00", 10: "0.0%", 11: "0", 14: "0.0%", 15: "0"}
    for fila in comparacion.iter_rows(min_row=3, max_row=comparacion.max_row):
        for c in fila:
            letra(c, negrita=(c.row == 3))
            c.alignment = Alignment(horizontal="center", vertical="center", wrap_text=(c.row == 3))
            if c.row > 3 and c.column in formatos:
                c.number_format = formatos[c.column]
            if c.row == 3:
                c.fill = PatternFill("solid", start_color="DDEBF7")
    letra(comparacion["A1"], tam=13, negrita=True)
    comparacion.row_dimensions[3].height = 44
    comparacion.column_dimensions["A"].width = 22
    for n in range(2, 16):
        comparacion.column_dimensions[get_column_letter(n)].width = 17
    nota = comparacion.max_row + 2
    comparacion.cell(nota, 1, "Limites usados: pH de 6.5 a 8.5 (ECA, Categoria 3) y turbidez 100 NTU (referencial, Categoria 1-A2). "
                              "El pH es el recalculado (ver Notas) y solo cuenta entre 0 y 14; la turbidez solo cuenta si el sensor estaba midiendo.")
    letra(comparacion.cell(nota, 1), tam=9, color="595959")

    # ---- graficos: el pH y la turbidez de los tres puntos en el tiempo ----
    graficos = libro.create_sheet("Graficos")
    for n, (nombre, columna, eje) in enumerate((("pH de cada punto", 13, "pH"), ("Turbidez de cada punto", 5, "NTU"))):
        g = ScatterChart()
        g.title, g.style, g.height, g.width = nombre, 13, 9, 26
        g.x_axis.title, g.y_axis.title = "Fecha y hora", eje
        g.x_axis.number_format = "dd/mm hh:mm"
        g.x_axis.delete = g.y_axis.delete = False
        for titulo, _, _ in puntos:
            hoja, u = libro[titulo], ultimas[titulo]
            serie = Series(Reference(hoja, min_col=columna, min_row=2, max_row=u),
                           Reference(hoja, min_col=15, min_row=2, max_row=u), title=titulo)
            serie.marker.symbol = "none"
            serie.graphicalProperties.line.solidFill = COLORES[PUNTOS.index(titulo)]
            serie.graphicalProperties.line.width = 12000
            g.series.append(serie)
        graficos.add_chart(g, f"A{1 + n * 20}")

    # ---- notas ----
    notas = libro.create_sheet("Notas")
    notas.column_dimensions["A"].width = 46
    notas.column_dimensions["B"].width = 22
    notas.column_dimensions["C"].width = 70
    lineas = [
        ("Datos guardados en los equipos - Rio San Juan de Rancas", None, None),
        (None, None, None),
        ("Archivos de origen", ", ".join(Path(a).name for _, a, _ in puntos), "CSV tal como salieron de la memoria interna de cada equipo"),
        ("Puntos incluidos", len(puntos), "Una hoja por punto; las lecturas se guardan cada 20 segundos mientras el equipo esta encendido"),
        ("Hoja Comparacion", None, "Promedios y lecturas fuera de rango de cada punto, calculados con formulas sobre su hoja"),
        ("Hoja Graficos", None, "El pH y la turbidez de los puntos a lo largo del tiempo"),
        (None, None, None),
        ("pH 7: mV del sensor", PH7_MV, "Medido el 08/10/2026 con el sobre de pH 7 (estable 2 min). Se puede cambiar."),
        ("pH 4: mV del sensor", PH4_MV, "Medido el 08/10/2026 con el sobre de pH 4 (solo 1 min; conviene repetir). Se puede cambiar."),
        ("Turbidez valida desde", TURBIDEZ_VALIDA_DESDE, "Antes el modulo del equipo 1 estaba en D (no media) y sin calibrar. Se puede cambiar."),
        ("pH calibrado en el equipo desde", PH_CALIBRADO_DESDE, "Las filas anteriores traen en la columna pH el valor con la calibracion vieja"),
        (None, None, None),
        ("Como leer las hojas de cada punto", None, None),
        ("pH", None, "Valor que calculo el equipo en ese momento"),
        ("pH_recalculado", None, "pH calculado otra vez desde pH_mV con la calibracion de las celdas B8 y B9. Usar esta columna. "
                                 "La calibracion se hizo con la sonda del equipo 1: los otros equipos necesitan la suya."),
        ("turbidez_NTU", None, "Aproximada: calibracion de un solo punto (agua limpia = 0) con la curva del fabricante. Tope del sensor: 3000."),
        ("turbidez_valida", None, "si = lectura hecha con el modulo en A y calibrado; no = descartar la turbidez de esa fila"),
        ("conductividad_uS", None, "uS/cm llevada a 25 C con la formula del fabricante, SIN calibrar con solucion patron. Vacia antes de instalar el sensor; 0 = por debajo de lo que se puede medir (unos 120)."),
        ("conductividad_mV", None, "Lectura directa del sensor, para poder recalcular cuando se calibre"),
        ("origen", None, "auto = guardado cada 20 s; boton = guardado al presionar el boton del equipo"),
        ("latitud, longitud", None, "Vacias cuando el GPS todavia no tenia ubicacion"),
        ("momento", None, "Fecha y hora juntas, para los graficos"),
        ("Sensor sin conectar", None, "Temperatura -127, pH sobre 14 o turbidez_V por debajo de 0.7 no son mediciones: la hoja Comparacion no las cuenta"),
    ]
    for linea in lineas:
        notas.append(linea)
    for fila in notas.iter_rows(min_row=1, max_row=len(lineas)):
        for c in fila:
            letra(c)
            c.alignment = Alignment(vertical="top", wrap_text=(c.column == 3))
    letra(notas["A1"], tam=13, negrita=True)
    letra(notas["A13"], negrita=True)
    for celda in ("B8", "B9", "B10"):   # valores que se pueden cambiar: azul
        letra(notas[celda], color="0000FF")
    for celda in ("B10", "B11"):
        notas[celda].number_format = "yyyy-mm-dd hh:mm:ss"
        notas[celda].alignment = Alignment(horizontal="right", vertical="top")

    primero = Path(puntos[0][1])
    if len(puntos) > 1:
        destino = primero.with_name("datos_tres_puntos_" + datetime.now().strftime("%Y-%m-%d_%H%M") + ".xlsx")
    else:
        destino = primero.with_suffix(".xlsx")
    libro.save(destino)
    print("Excel:", destino, "-", ", ".join(f"{titulo}: {len(filas)} lecturas" for titulo, _, filas in puntos))
    return destino


if __name__ == "__main__":
    a_excel(sys.argv[1:4] if len(sys.argv) > 1 else bajar())
