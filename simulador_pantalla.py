# simulador de la pantallita OLED de los equipos (128x64, un solo color)
# dibuja las 3 mascotas (ajolote, nutria y pinguino) con las mismas funciones q
# equipo_iot_etapa2/pantalla.h, pa ver como quedan sin tener el ESP32 conectado
# ojo si se cambia algo aca toca pasarlo a pantalla.h (y a index.html q las dibuja a color)
#
#   python simulador_pantalla.py            -> capturas/pantalla_<mascota>_estados.png, pantalla_mascotas_estados.png y pantalla_mascotas.gif
#   python simulador_pantalla.py cerca.png  -> ademas guarda las mascotas ampliadas en ese archivo
import math
import re
import sys
from pathlib import Path
from PIL import Image

AQUI = Path(__file__).resolve().parent
W, H = 128, 64
buf = bytearray(W * H)
BIEN, PH_BAJO, PH_ALTO, TURBIA, SIN_DATO = range(5)
MENSAJES = ("AGUA BIEN", "PH BAJO", "PH ALTO", "AGUA TURBIA", "SIN DATO")

#las letras se leen de pantalla.h, para no tener dos copias
_h = (AQUI / "equipo_iot_etapa2" / "pantalla.h").read_text(encoding="utf-8")
LETRAS_ORDEN = re.search(r'LETRAS_ORDEN\[\] = "([^"]*)"', _h).group(1)
_t = _h[_h.index("LETRAS[][5]"):_h.index("// ---- Control")]
LETRAS = [[int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]{2}", g)] for g in re.findall(r"\{([^{}]*)\}", _t)]


# ---- operaciones de dibujo (las mismas de pantalla.h) ----
# "on": 0 apaga, 1 enciende, 2 tablero (un punto si y otro no: se ve gris), 3 sombrea (apaga uno de cada dos),
#4 invierte (sale oscuro sobre lo claro y claro sobre lo oscuro)

def punto(x, y, on):
    x = int(x); y = int(y)
    if 0 <= x < W and 0 <= y < H:
        if on == 2: on = (x + y) & 1
        elif on == 3:
            if (x + y) & 1: return
            on = 0
        elif on == 4: on = 1 - buf[y * W + x]
        buf[y * W + x] = on


def caja(x, y, w, h, on):
    for j in range(h):
        for i in range(w):
            punto(x + i, y + j, on)


def disco(cx, cy, r, on):
    for j in range(-r, r + 1):
        for i in range(-r, r + 1):
            if i * i + j * j <= r * r + r * 0.6:
                punto(cx + i, cy + j, on)


def elipse(cx, cy, rx, ry, on):
    for j in range(-int(ry), int(ry) + 1):
        for i in range(-int(rx), int(rx) + 1):
            if i * i / (rx * rx) + j * j / (ry * ry) <= 1:
                punto(cx + i, cy + j, on)


def linea(x0, y0, x1, y1, on):
    x0 = int(x0); y0 = int(y0); x1 = int(x1); y1 = int(y1)
    dx = abs(x1 - x0); dy = -abs(y1 - y0)
    sx = 1 if x0 < x1 else -1; sy = 1 if y0 < y1 else -1; e = dx + dy
    while True:
        punto(x0, y0, on)
        if x0 == x1 and y0 == y1:
            break
        e2 = 2 * e
        if e2 >= dy: e += dy; x0 += sx
        if e2 <= dx: e += dx; y0 += sy


def red(v):
    return int(math.floor(v + 0.5))


def texto(x, y, s, on):
    for ch in s:
        g = LETRAS[LETRAS_ORDEN.index(ch) if ch in LETRAS_ORDEN else 13]
        for c in range(5):
            for f in range(7):
                if g[c] >> f & 1: punto(x + c, y + f, on)
        x += 6


def ancho_texto(s):
    return len(s) * 6 - 1


#mancha redonda con centro y radio con decimales: asi las formas se mueven suave
def mancha(cx, cy, r, on):
    if r < 0.4: return
    if r < 1: punto(red(cx), red(cy), on); return
    for j in range(int(math.floor(cy - r)), int(math.ceil(cy + r)) + 1):
        for i in range(int(math.floor(cx - r)), int(math.ceil(cx + r)) + 1):
            if (i - cx) * (i - cx) + (j - cy) * (j - cy) <= r * r: punto(i, j, on)


# tira curva q va de (x0,y0) a (x2,y2) pasando cerca de (x1,y1), y cambia de grosor de r0 a r2
def tira(x0, y0, x1, y1, x2, y2, r0, r2, on, panza=0.0):
    n = int(abs(x2 - x0) + abs(y2 - y0) + abs(x1 - x0) + abs(y1 - y0)) + 2
    for k in range(n + 1):
        s = k / n; a = (1 - s) * (1 - s); b = 2 * s * (1 - s); c = s * s
        mancha(a * x0 + b * x1 + c * x2, a * y0 + b * y1 + c * y2,
               r0 + (r2 - r0) * s + panza * math.sin(math.pi * s), on)


# dibujito de pocos puntos: cada numero es una fila y el bit mas alto es el punto de la izquierda
def sello(x, y, filas, ancho, on, espejo=False, giro=0):
    for j in range(len(filas)):
        for i in range(ancho):
            if filas[j] >> (ancho - 1 - i) & 1:
                a, b = (ancho - 1 - i if espejo else i), j
                for _ in range(giro): a, b = ancho - 1 - b, a  # cuartos de vuelta (dibujos cuadrados)
                punto(x + a, y + b, on)


OJO_FELIZ = (0b0111110, 0b1100011, 0b1000001)  #arco hacia arriba -> ojo cerrado, sonriendo
OJO_TRISTE = (0b1000001, 0b1100011, 0b0111110) # arco hacia abajo
OJO_APRETADO = (0b11000, 0b00110, 0b00001, 0b00110, 0b11000)   # ">"
OJO_MAREADO = (0b0011100, 0b0100010, 0b1001101, 0b1010001, 0b1010010, 0b0001100, 0)  # espiral
BRAZO = (0b10000, 0b10000, 0b10001, 0b10001, 0b01110)  # bracito recogido, en contorno (el izquierdo)
DESTELLO = (0b0001000, 0b0001000, 0b0011100, 0b1110111, 0b0011100, 0b0001000, 0b0001000)
DESTELLO_CHICO = (0b010, 0b111, 0b010)
GOTA = (0b010, 0b010, 0b111, 0b111, 0b010)
BURBUJA = (0b01110, 0b10001, 0b10001, 0b10001, 0b01110)
BURBUJA_CHICA = (0b010, 0b101, 0b010)
OJO_FELIZ_CHICO = (0b01110, 0b10001, 0b10001)   # el arco de los ojos del pinguino
NARIZ = (0b0111110, 0b1111111, 0b0111110, 0b0011100) #la nariz ancha de la nutria
PATITAS = (0b0011100, 0b0100010, 0b0100010, 0b0100010)   # las patas de la nutria, recogidas sobre la barriga
PICO = (0b01110, 0b11111)  # la parte de arriba del pico

# branquias: base x,y, punta x,y y grosor, medidos desde el centro de la cabeza
# las de la izquierda son mas largas porque el ajolote esta un poco de lado, como en la foto
BRANQUIAS = ((-12, -7, -22, -13, 1.5), (-14, 0, -25, -1, 1.6), (-12, 7, -20, 10, 1.5),
             (11, -9, 15, -13, 1.4), (14, -3, 22, -6, 1.3), (14, 4, 20, 6, 1.3))

# ---- las tres mascotas: una por equipo ----
AJOLOTE, NUTRIA, PINGUINO = range(3)
NOMBRES = ("ajolote", "nutria", "pinguino")
CENTROS = ((32, 20), (24, 16), (29, 16))  # donde va el centro de la cabeza de cada una
# donde va la cara, contado desde el centro de la cabeza:
# ojo izquierdo x, ojo derecho x, ojos y, boca x, boca y, mejilla izquierda x, mejilla derecha x, mejillas y
CARAS = ((-8, 9, 0, 1, 6, -12, 10, 5), (-8, 8, -1, 1, 4, 0, 0, 0), (-7, 7, 0, 0, 1, -11, 8, 3))


class Pose:
    pass


# el movimiento de cada estado del agua es el mismo para las tres mascotas
def pose(est, cx, cy, t):
    p = Pose()
    tf = p.tf = t % 1048576   #el tiempo con decimales se reinicia cada 17 min: asi no pierde finura con los dias
    sube = 1.2 * math.sin(tf / 450.0) #flota
    mueve = 0.0
    p.inc = 0 #cabeza ladeada: +1 sube el lado derecho
    p.caida = 0.0   # branquias, orejas o aletas caidas
    p.onda = 0.22   # cuanto se mecen
    p.colea = math.sin(tf / 260.0) * 1.5   # punta de la cola
    p.paso = 1 #q pata estira (0: ninguna)
    p.saluda = False
    if est == BIEN:
        f = tf / 330.0
        sube = -abs(math.sin(f)) * 2.2 # baila a brinquitos
        p.inc = 1 if int(f / math.pi) % 2 == 0 else -1   # y ladea la cabeza a cada brinco
        p.paso = p.inc
        p.colea = math.sin(tf / 150.0) * 2.5
        p.saluda = t % 5200 < 1700
    elif est == PH_BAJO:
        mueve = red(math.sin(tf / 35.0)); sube = 0   # tiembla
        p.caida = 0.55; p.onda = 0.05; p.colea = 3 + math.sin(tf / 40.0) * 0.6; p.paso = 0
    elif est == PH_ALTO:
        mueve = math.sin(tf / 420.0) * 2.4 # se tambalea, mareado
        p.inc = -1 if mueve > 1 else 1 if mueve < -1 else 0
        p.onda = 0.32; p.paso = p.inc
    elif est == TURBIA:
        sube = 1 + 0.5 * math.sin(tf / 700.0)  # decaido
        p.caida = 0.6; p.onda = 0.04; p.colea = 4; p.paso = 0
    elif est == SIN_DATO:
        p.inc = 1 if (t // 2400) % 2 == 0 else 0  # ladea la cabeza, curioso
    p.cx = cx + red(mueve); p.cy = cy + red(sube)
    p.cg, p.sg = math.cos(p.inc * 0.10), math.sin(p.inc * 0.10)
    p.brazo_i = p.brazo_d = None # none: recogido,un numero -> estirado, con la mano a esa altura
    if p.saluda: p.brazo_d = -5 + math.sin(tf / 110.0) * 2
    if est == PH_BAJO: p.brazo_i = p.brazo_d = -2 + math.sin(tf / 40.0)
    if est == PH_ALTO: p.brazo_i = math.sin(tf / 200.0) * 4 - 1; p.brazo_d = -p.brazo_i - 2
    return p


def cuerpo_ajolote(est, p):
    cx, cy, tf = p.cx, p.cy, p.tf

    # cola: una hoja hacia la izquierda con su borde interior
    for dr, on in ((0.0, 1), (2.0, 0), (3.0, 1)):
        tira(cx - 7, cy + 25, cx - 15, cy + 25.5 + p.colea * 0.4, cx - 22, cy + 22.5 + p.colea, 4.6 - dr, 1.8 - dr, on, 0.9)

    # branquias: tres hojas por lado, en gris
    for k, (bx, by, tx, ty, r) in enumerate(BRANQUIAS):
        lado = -1 if bx < 0 else 1
        a = lado * p.caida + p.onda * math.sin(tf / 280.0 + k * 1.3)
        dx, dy = tx - bx, ty - by
        tx, ty = bx + dx * math.cos(a) - dy * math.sin(a), by + dx * math.sin(a) + dy * math.cos(a)
        bx, by = bx * p.cg + by * p.sg, by * p.cg - bx * p.sg  #la cabeza ladeada las lleva consigo
        tx, ty = tx * p.cg + ty * p.sg, ty * p.cg - tx * p.sg
        tira(cx + bx, cy + by, cx + (bx + tx) / 2, cy + (by + ty) / 2, cx + tx, cy + ty, r, r, 2, r)

    #patas: una estirada y la otra recogida,al bailar se turnan
    if p.paso > 0:
        tira(cx - 1, cy + 26, cx - 1, cy + 28, cx - 1, cy + 30, 1.9, 1.7, 1)
        tira(cx + 9, cy + 25, cx + 10, cy + 26, cx + 12, cy + 27.5, 2.0, 1.9, 1)
    elif p.paso < 0:
        tira(cx + 7, cy + 26, cx + 7, cy + 28, cx + 7, cy + 30, 1.9, 1.7, 1)
        tira(cx - 3, cy + 25, cx - 4, cy + 26, cx - 6, cy + 27.5, 2.0, 1.9, 1)
    else:
        mancha(cx - 1, cy + 28.5, 2.0, 1)
        mancha(cx + 7, cy + 28.5, 2.0, 1)

    # cuerpo y brazos
    elipse(cx + 3, cy + 18, 10.5, 9.5, 1)
    caja(cx - 3, cy + 24, 13, 4, 1)
    if p.brazo_i is None: sello(cx - 4, cy + 16, BRAZO, 5, 0)
    else: tira(cx - 4, cy + 18, cx - 7, cy + 18, cx - 10, cy + 18 + p.brazo_i, 2.0, 1.7, 1)
    if p.brazo_d is None: sello(cx + 5, cy + 15, BRAZO, 5, 0, espejo=True)
    else: tira(cx + 10, cy + 18, cx + 13, cy + 18, cx + 16, cy + 18 + p.brazo_d, 2.0, 1.7, 1)

    # cabeza, con un borde oscuro q la separa de branquias y cuerpo
    elipse(cx, cy, 16.5, 14.5, 0)
    elipse(cx, cy, 15.5, 13.5, 1)

    if est == TURBIA:  # manchas de suciedad
        for mx, my, r in ((9, 23, 2.8), (2, -9, 2.6), (10, 9, 2.3)):
            mancha(cx + mx, cy + my, r, 3)


def cuerpo_nutria(est, p):
    cx, cy, tf = p.cx, p.cy, p.tf

    # mano izquierda levantada (para saludar o del susto): va x detras de la cabeza
    mano = p.brazo_d if p.brazo_d is not None else p.brazo_i
    if mano is not None:
        for dr, on in ((0.0, 1), (1.0, 0)):
            tira(cx - 7, cy + 17, cx - 12, cy + 17, cx - 15, cy + 16 + mano, 2.8 - dr, 2.5 - dr, on)

    #cuerpo acurrucado y cola gruesa que sube por la derecha hasta una punta: en contorno, oscuros por dentro
    for dr, on in ((0.0, 1), (1.0, 0)):
        elipse(cx + 3, cy + 20, 14.3 - dr, 13.3 - dr, on)
        tira(cx + 14, cy + 27, cx + 27, cy + 28, cx + 23.5, cy + 12.5 + p.colea * 0.8, 5.5 - dr, 1.8 - dr, on, 1.2)

    # barriga clara
    elipse(cx + 8, cy + 21, 7.6, 7.2, 1)

    # orejitas en contorno: con el agua mal, se agachan
    for lado in (-1, 1):
        ex, ey = lado * 14.5, -6.0 + p.caida * 4
        ex, ey = ex * p.cg + ey * p.sg, ey * p.cg - ex * p.sg
        mancha(cx + ex, cy + ey, 2.9, 1)
        mancha(cx + ex, cy + ey, 1.9, 0)

    # cabeza grande y clara, con un borde oscuro q la separa de las orejas y del cuerpo
    elipse(cx, cy + 1, 16.5, 13.5, 0)
    elipse(cx, cy, 15.5, 12.5, 1)

    # manos sobre el pecho (la izquierda, si no esta levantada) y patas recogidas sobre la barriga
    if mano is None:
        elipse(cx - 2, cy + 18, 4.2, 3.7, 1)
        elipse(cx - 2, cy + 18, 3.2, 2.7, 0)
    tiembla = 0 if p.brazo_i is None else red(p.brazo_i + 2)
    mancha(cx + 10.5, cy + 16 + tiembla, 2.8, 1)
    mancha(cx + 10.5, cy + 16 + tiembla, 1.8, 0)
    mancha(cx + 4, cy + 27, 2.0, 0)
    caja(cx + 2, cy + 27, 5, 3, 0)
    sello(cx + 1, cy + 25, PATITAS, 7, 1)

    # bigotes: oscuros sobre la cabeza y claros al salir de ella
    cae = red(p.caida * 3)
    linea(cx - 11, cy + 4, cx - 19, cy + 2 + cae, 4)
    linea(cx - 11, cy + 6, cx - 19, cy + 8 + cae, 4)
    linea(cx + 11, cy + 3, cx + 19, cy + 1 + cae, 4)
    linea(cx + 11, cy + 5, cx + 19, cy + 6 + cae, 4)

    if est == TURBIA:
        for mx, my, r in ((11, 23, 2.0), (5, -8, 2.4), (-7, 9, 2.0)):
            mancha(cx + mx, cy + my, r, 3)


def cuerpo_pinguino(est, p):
    cx, cy, tf = p.cx, p.cy, p.tf
    ladea = -p.inc   # la cabeza se va hacia el lado que baja

    # aletas, en "agua bien" la derecha esta siempre arriba, saludando, si no, cuelgan y aletean
    bi, bd = p.brazo_i, p.brazo_d
    if est == BIEN and bd is None: bd = -4.5 + math.sin(tf / 400.0)
    if bi is None:
        a = 1.05 + p.onda * math.sin(tf / 280.0) - p.caida * 0.6
        ix, iy = -11 - 10.5 * math.sin(a), 13 + 8 * math.cos(a)
    else:
        ix, iy = -21, 13 + bi
    if bd is None:
        a = 0.9 + p.onda * math.sin(tf / 280.0 + 1) - p.caida * 0.5
        dx, dy = 13 + 9 * math.sin(a), 11 + 9 * math.cos(a)
    else:
        dx, dy = 19, 5 + bd

    # aletas anchas, cabeza y cuerpo: todo en contorno, oscuro por dentro
    for dr, on in ((0.0, 1), (1.0, 0)):
        tira(cx - 9, cy + 13, cx - 15, cy + 13, cx + ix, cy + iy, 5.2 - dr, 2.0 - dr, on)
        tira(cx + 12, cy + 9, cx + 16, cy + 7, cx + dx, cy + dy, 4.6 - dr, 2.5 - dr, on)
        elipse(cx - 1 + ladea, cy - 1, 14.5 - dr, 12.5 - dr, on)
        elipse(cx + 2, cy + 12, 15.0 - dr, 19.5 - dr, on)

    #frente clara: los dos redondeles de la cara, con un pico de plumas oscuras entre ellos, la barbilla y la barriga
    elipse(cx - 7 + ladea, cy, 4.8, 5.8, 1)
    elipse(cx + 7 + ladea, cy, 4.8, 5.8, 1)
    elipse(cx + 1, cy + 7, 9.5, 4.0, 1)
    elipse(cx + 3, cy + 17, 11.4, 12.1, 1)

    # patas: una apoyada y la otra levantada, al bailar se turnan
    for lado in (-1, 1):
        px, py = (cx + (13 if lado > 0 else -9), cy + 30) if p.paso == lado else (cx + (9 if lado > 0 else -4), cy + 32)
        elipse(px, py, 3.6, 2.5, 1)
        punto(px - 1, py + 2, 0); punto(px + 1, py + 2, 0) # los dedos

    if est == TURBIA:
        for mx, my, r in ((7, 23, 2.8), (-3, 15, 2.2), (9, 12, 1.8)):
            mancha(cx + mx, cy + my, r, 3)


CUERPOS = (cuerpo_ajolote, cuerpo_nutria, cuerpo_pinguino)


# ojos, boca, mejillas y lagrima - los ojos son los mismos para las tres, la boca cambia
def cara(cual, est, p, t):
    cx, cy, tf, inc = p.cx, p.cy, p.tf, p.inc
    oi, od, oy, bx, by, mi, md, mey = CARAS[cual]
    if cual == PINGUINO: cx -= inc # la cara va con la cabeza ladeada

    # con el agua bien, el ajolote y el pinguino sonrien con los ojos cerrados y de vez en cuando los abren;
    # la nutria los tiene abiertos y los cierra al parpadear o al saludar
    parpadea = t % 4200 < 300
    abiertos = (not parpadea and not p.saluda) if cual == NUTRIA else parpadea
    for lado in (-1, 1):
        ox = cx + (od if lado > 0 else oi); y = cy + oy - lado * inc
        if est == BIEN:
            if abiertos: disco(ox, y, 2, 0); punto(ox - 1, y - 1, 1)
            elif cual == PINGUINO: sello(ox - 2, y - 1, OJO_FELIZ_CHICO, 5, 0)
            else: sello(ox - 3, y - 1, OJO_FELIZ, 7, 0)
        elif est == SIN_DATO:
            mira = red(math.sin(tf / 600.0) * (2 if cual == AJOLOTE else 1))
            if t % 3200 < 120: caja(ox - 2, y, 5, 1, 0)
            else: disco(ox + mira, y, 2, 0); punto(ox + mira - 1, y - 1, 1)
        elif est == PH_BAJO: sello(ox - 2, y - 2, OJO_APRETADO, 5, 0, espejo=lado > 0)
        elif est == PH_ALTO: sello(ox - 3, y - 3, OJO_MAREADO, 7, 0, giro=(t // 130) % 4)
        elif est == TURBIA:
            sello(ox - 3, y, OJO_TRISTE, 7, 0)
            linea(ox + lado * 3, y - 2, ox - lado * 2, y - 4, 0)   # cejas caidas

    mx, my = cx + bx, cy + by
    if cual == PINGUINO:
        # pico claro en un hueco oscuro -> sonrie (una raya oscura lo parte), se abre del mareo o castanetea de frio
        abre = 2 if est == PH_ALTO else 1 if est == BIEN or (est == PH_BAJO and (t // 90) % 2 == 0) else 0
        caja(mx - 3, my - 1, 7, 4 + abre, 0); caja(mx - 2, my + 3 + abre, 5, 1, 0)
        sello(mx - 2, my, PICO, 5, 1)
        caja(mx - 1, my + 2 + abre, 3, 1, 1)
        if est == TURBIA: caja(mx - 2, my + 6, 5, 1, 0); punto(mx - 3, my + 7, 0); punto(mx + 3, my + 7, 0)
    elif cual == NUTRIA:
        sello(mx - 3, my - 4, NARIZ, 7, 0)  # nariz ancha y, debajo, la rayita y la boca
        punto(mx, my, 0)
        my += 1
        if est == BIEN: # boca en "w"
            punto(mx - 5, my, 0); punto(mx, my, 0); punto(mx + 5, my, 0)
            caja(mx - 4, my + 1, 4, 1, 0); caja(mx + 1, my + 1, 4, 1, 0)
            if p.saluda: caja(mx - 1, my + 2, 3, 2, 0) # boca abierta
        elif est == PH_BAJO:
            for i in range(-4, 5): punto(mx + i, my + 1 + red(math.sin(i * 1.5 + tf / 90.0) * 1.2), 0)
        elif est == PH_ALTO:
            disco(mx, my + 2, 2, 0); punto(mx, my + 2, 1)
        elif est == TURBIA:
            caja(mx - 2, my + 1, 5, 1, 0); punto(mx - 3, my + 2, 0); punto(mx + 3, my + 2, 0)
        else:
            caja(mx - 1, my + 1, 3, 1, 0)
    else:
        if est == BIEN:
            caja(mx - 3, my, 7, 1, 0)
            punto(mx - 4, my - 1, 0); punto(mx + 4, my - 1, 0)
            punto(mx + 4 * inc, my - 2, 0) #la comisura del lado que sube
            if p.saluda: caja(mx - 2, my + 1, 5, 1, 0); caja(mx - 1, my + 2, 3, 1, 0) # boca abierta
        elif est == PH_BAJO:
            for i in range(-4, 5): punto(mx + i, my + red(math.sin(i * 1.5 + tf / 90.0) * 1.2), 0)
        elif est == PH_ALTO:
            disco(mx, my + 1, 2, 0); punto(mx, my + 1, 1)
        elif est == TURBIA:
            caja(mx - 2, my, 5, 1, 0); punto(mx - 3, my + 1, 0); punto(mx + 3, my + 1, 0)
        else:
            caja(mx - 1, my, 3, 1, 0)

    if cual != NUTRIA and est in (BIEN, SIN_DATO, PH_ALTO):  #mejillas (la nutria lleva bigotes)
        caja(cx + mi, cy + mey + inc, 4, 2, 3)
        caja(cx + md, cy + mey - inc, 4, 2, 3)

    if est == TURBIA: # lagrima
        f = (t // 110) % 16
        if f < 9: caja(cx + oi - 1, cy + oy + 4 + f, 1, 2, 0)


def burbuja(x, y, grande):
    if grande: sello(x - 2, y - 2, BURBUJA, 5, 1)
    else: sello(x - 1, y - 1, BURBUJA_CHICA, 3, 1)


def brillo(x, y, n):
    if n == 0: punto(x, y, 1)
    elif n == 1: sello(x - 1, y - 1, DESTELLO_CHICO, 3, 1)
    else: sello(x - 3, y - 3, DESTELLO, 7, 1)


#lo que rodea a la mascota: burbujas, destellos, gotas o un signo de pregunta
def entorno(est, p, t):
    cx, cy = p.cx, p.cy
    if est == BIEN:
        for k in range(2): #burbujas que suben por los costados
            f = (t // 70 + k * 13) % 26
            x = (3, 54)[k] + red(math.sin((f + k * 4) / 4.0)); y = 50 - f
            burbuja(x, y, k)
        f = (t // 160) % 12  # destellos que titilan
        if f < 5: brillo(55, 5, (0, 1, 2, 1, 0)[f])
        f = (t // 160 + 6) % 12
        if f < 5: brillo(3, 2, (0, 1, 1, 1, 0)[f])
    if est == PH_BAJO:
        for k in range(3):   # gotas acidas que caen
            f = (t // 45 + k * 11) % 30
            x = (2, 52, 56)[k]; y = (26, 26, 34)[k] + f
            if y < 50: sello(x, y, GOTA, 3, 1)
        if t % 500 < 250:  # gotitas de susto
            linea(cx + 12, cy - 15, cx + 13, cy - 16, 1); linea(cx - 13, cy - 15, cx - 14, cy - 16, 1)
    if est == PH_ALTO:
        for k in range(4): # burbujas como de jabon
            f = (t // 55 + k * 8) % 30
            x = (3, 56, 52, 2)[k] + red(math.sin((f + k) / 4.0)); y = 50 - f
            if k != 2 or f < 18: burbuja(x, y, k % 2 == 0)
    if est == SIN_DATO and t % 1000 < 650:
        texto(53, 1 + (t // 1000) % 2, "?", 1)


# una mascota completa - (cx, cy) es el centro de la cabeza
def mascota(cual, est, cx, cy, t):
    if est == TURBIA:
        for k in range(10):  # particulas que caen, por detras
            f = (t // 70 + k * 7) % 52
            x = (k * 23) % 59
            punto(x, f, 1)
            if k % 3 == 0: punto(x + 1, f, 1)
    p = pose(est, cx, cy, t)
    CUERPOS[cual](est, p)
    cara(cual, est, p, t)
    entorno(est, p, t)


def ajolote(est, cx, cy, t):
    mascota(AJOLOTE, est, cx, cy, t)


# ---- la pantalla completa, como en dibujarPantalla ----

def pantalla(est, t, valores=("7.32", "12", "11.4oC", "356"), cual=AJOLOTE):
    for i in range(len(buf)): buf[i] = 0
    mascota(cual, est, CENTROS[cual][0], CENTROS[cual][1], t)
    for i, nombre in enumerate(("pH", "NTU", "TEMP", "CE")):
        y = 3 + i * 12
        texto(62, y, nombre, 1)
        texto(126 - ancho_texto(valores[i]), y, valores[i], 1)
        if i < 3:
            for x in range(62, 126, 3): punto(x, y + 9, 1)
    barra(est)


def barra(est):
    caja(0, 54, W, 10, 1)
    texto(2, 55, "37%", 0)
    texto((W - ancho_texto(MENSAJES[est])) // 2, 55, MENSAJES[est], 0)
    caja(117, 60, 2, 2, 0); caja(120, 58, 2, 4, 0); caja(123, 56, 2, 6, 0)


# ---- las otras vistas: el boton del equipo pasa de una a otra (vistaValores, vistaEstado y avisoMuestra en pantalla.h) ----

def texto_grande(x, y, s, on):
    for ch in s:
        g = LETRAS[LETRAS_ORDEN.index(ch) if ch in LETRAS_ORDEN else 13]
        for c in range(5):
            for f in range(7):
                if g[c] >> f & 1: caja(x + c * 2, y + f * 2, 2, 2, on)
        x += 12


def vista_valores(est, valores=("7.32", "12", "11.4", "356")):
    for i in range(len(buf)): buf[i] = 0
    for i, nombre in enumerate(("pH", "NTU", "TEMP oC", "CE uS")):
        x = 2 + (i % 2) * 65; y = 1 + (i // 2) * 27
        texto(x, y, nombre, 1)
        texto_grande(x, y + 10, valores[i], 1)
    for y in range(0, 53, 3): punto(64, y, 1)
    for x in range(0, W, 3): punto(x, 26, 1)
    barra(est)


def vista_estado(lineas, ads="ADS SI"):
    for i in range(len(buf)): buf[i] = 0
    caja(0, 0, W, 9, 1)
    texto(2, 1, lineas[0], 0)
    for i, l in enumerate(lineas[1:]):
        texto(1, 10 + i * 9, l, 1)
    texto(126 - ancho_texto(ads), 37, ads, 1)


def aviso_muestra():
    for i in range(len(buf)): buf[i] = 0
    caja(0, 0, W, 64, 1)
    caja(2, 2, W - 4, 60, 0)
    texto_grande(23, 14, "MUESTRA", 1)
    texto_grande(17, 36, "GUARDADA", 1)


VALORES = (("7.32", "12", "11.4oC", "356"), ("5.10", "18", "11.4oC", "410"), ("9.40", "15", "11.6oC", "390"),
           ("7.10", "850", "11.2oC", "620"), ("--", "--", "--", "--"))


def imagen(k, recorte=None):
    im = Image.frombytes("L", (W, H), bytes(255 if b else 0 for b in buf))
    if recorte: im = im.crop(recorte)
    return im.resize((im.width * k, im.height * k), Image.NEAREST)


if __name__ == "__main__":
    capturas = AQUI.resolve().parents[1] / "documentacion" / "capturas"
    K = 3
    if len(sys.argv) > 1 and sys.argv[1] == "vistas":  # nomas las vistas del boton (es rapido)
        hoja = Image.new("L", (W * K * 2 + 15, H * K * 3 + 20), 90)
        dibujos = (
            lambda: pantalla(BIEN, 500, VALORES[0], NUTRIA),
            lambda: vista_valores(BIEN),
            lambda: vista_estado(("EQUIPO 2 NUTRIA", "WIFI SI  INTERNET NO", "IP 10.157.93.51", "RED RIOSANJUAN-E2",
                                  "GPS SI 12 SAT", "MEM 37%  RELOJ SI", "TEMP SI PH SI TURB NO")),
            lambda: vista_valores(TURBIA, ("14.00", "3000", "-12.5", "2200")),
            lambda: vista_estado(("EQUIPO 3 PINGUINO", "WIFI NO  INTERNET NO", "SIN WIFI DEL CELULAR", "RED RIOSANJUAN-E3",
                                  "GPS BUSCANDO", "MEM 62%  RELOJ NO", "TEMP NO PH NO TURB NO"), "ADS NO"),
            aviso_muestra,
        )
        for n, dibujar in enumerate(dibujos):
            dibujar()
            hoja.paste(imagen(K), (5 + (n % 2) * (W * K + 5), 5 + (n // 2) * (H * K + 5)))
        hoja.save(capturas / "pantalla_vistas.png")
        # las tres vistas y el aviso, pa los documentos (fondo gris -> asi se ve donde termina cada pantalla)
        guia = Image.new("L", (W * K * 2 + 15, H * K * 2 + 15), 120)
        for n, i in enumerate((0, 1, 2, 5)):
            dibujos[i]()
            guia.paste(imagen(K), (5 + (n % 2) * (W * K + 5), 5 + (n // 2) * (H * K + 5)))
        guia.save(capturas / "pantalla_vistas_guia.png")
        print("guardado en", capturas / "pantalla_vistas.png", "y pantalla_vistas_guia.png")
        sys.exit()
    # una hoja por mascota: los cinco estados, tres momentos de cada uno
    for cual in range(3):
        hoja = Image.new("L", (W * K * 3 + 20, H * K * 5 + 30), 90)
        for est in range(5):
            for n, t in enumerate((500, 1700, 5300)):
                pantalla(est, t + est * 137, VALORES[est], cual)
                hoja.paste(imagen(K), (5 + n * (W * K + 5), 5 + est * (H * K + 5)))
        hoja.save(capturas / ("pantalla_%s_estados.png" % NOMBRES[cual]))

    # las tres juntas: una columna por equipo, una fila por estado
    hoja = Image.new("L", (W * K * 3 + 20, H * K * 5 + 30), 90)
    for est in range(5):
        for cual in range(3):
            pantalla(est, 500 + est * 137, VALORES[est], cual)
            hoja.paste(imagen(K), (5 + cual * (W * K + 5), 5 + est * (H * K + 5)))
    hoja.save(capturas / "pantalla_mascotas_estados.png")

    # animacion: las tres pantallas lado a lado, 3.5 s de cada estado
    cuadros = []
    for est in range(5):
        for t in range(0, 3500, 50):
            cuadro = Image.new("L", (W * K * 3 + 20, H * K + 10), 90)
            for cual in range(3):
                pantalla(est, t + 400, VALORES[est], cual)
                cuadro.paste(imagen(K), (5 + cual * (W * K + 5), 5))
            cuadros.append(cuadro.convert("P"))
    cuadros[0].save(capturas / "pantalla_mascotas.gif", save_all=True, append_images=cuadros[1:], duration=50, loop=0)
    print("guardado en", capturas)

    if len(sys.argv) > 1: #las mascotas de cerca, pa revisar el dibujo
        cerca = Image.new("L", (60 * 6 * 5 + 30, 54 * 6 * 3 + 20), 90)
        for cual in range(3):
            for est in range(5):
                pantalla(est, 500 + est * 137, VALORES[est], cual)
                cerca.paste(imagen(6, (0, 0, 60, 54)), (5 + est * (60 * 6 + 5), 5 + cual * (54 * 6 + 5)))
        cerca.save(sys.argv[1])


# ---- calibracion con el segundo boton (vistaCalibrar en pantalla.h) ----
def vista_calibrar(paso, valor="1598", estable=True, ph="7.00", resultado=3):
    for i in range(len(buf)): buf[i] = 0
    caja(0, 0, W, 9, 1)
    texto(2, 1, "CALIBRAR", 0)
    if paso == 4:
        texto(1, 14, "PH GUARDADO" if resultado & 1 else "PH NO VALE" if resultado & 4 else "PH SIN CAMBIO", 1)
        if resultado & 4: texto(1, 24, "REPITE PH 7 Y PH 4", 1)
        texto(1, 38, "TURBIDEZ GUARDADA" if resultado & 2 else "TURBIDEZ SIN CAMBIO", 1)
        texto(1, 55, "LISTO", 1)
        return
    l = "%d DE 3" % paso
    texto(126 - ancho_texto(l), 1, l, 0)
    texto(1, 12, "TURBIDEZ AGUA LIMPIA" if paso == 3 else "SONDA EN PH " + ph, 1)
    texto_grande(1, 23, valor, 1)
    texto(len(valor) * 12 + 4, 30, "V" if paso == 3 else "MV", 1)
    q = "QUIETA" if estable else "ESPERA"
    texto(126 - ancho_texto(q), 30, q, 1)
    texto(1, 44, "B1 TOMA  B2 SALTA" if estable else "B2 SALTA EL PASO", 1)
    texto(1, 55, "B2 3 SEG SALE", 1)


def hoja_calibrar(destino):
    casos = [(1, "1603", False, "7.00", 0), (1, "1598", True, "7.00", 0), (2, "2122", True, "4.00", 0),
             (3, "2.014", True, "", 0), (4, "", True, "", 3), (4, "", True, "", 4)]
    hoja = Image.new("L", (W * 3 * 3 + 20, H * 3 * 2 + 15), 90)
    for n, (p, v, e, ph, r) in enumerate(casos):
        vista_calibrar(p, v, e, ph, r)
        hoja.paste(imagen(3), (5 + (n % 3) * (W * 3 + 5), 5 + (n // 3) * (H * 3 + 5)))
    hoja.save(destino)
