// pantallita OLED de 1.3" (SH1106, 128x64) con la mascota de cada equipo (ajolote, nutria o pinguino)
// y los valores del agua
// conexion: VCC -> 3V3, GND -> GND y SDA/SCL en 2 de los pines D14, D33 y D32, el orden da igual
// xq al arrancar prueba todas las combinaciones - el reloj sigue en D21 y D22
// el pin q sobre entre el 32 y el 33 es pal sensor de conductividad
// corre en otra tarea pa q la animacion no se trabe mientras el equipo mide
// usa variables del .ino: temp, ph, ntu, turbV, numLectura y los limites
// si la imagen sale corrida o con una raya al costado poner OLED_DESPLAZAMIENTO en 0

const int PINES_PANTALLA[] = { 14, 33, 32 };
int oledSda = 14, oledScl = 33; // donde esta la pantalla (o donde se espera si no aparecio)
bool oledUbicada = false;   // ya se encontro una vez: despues no se prueban otros pines
const int OLED_DESPLAZAMIENTO = 2; //2 = SH1106 (1.3"), 0 = SSD1306 (0.96")

const int OLED_ANCHO = 128, OLED_ALTO = 64;
uint8_t oledDireccion = 0;   //0x3C o 0x3D, 0 = no esta
uint8_t oledImagen[OLED_ANCHO * OLED_ALTO / 8];  // un bit por punto
uint8_t oledVisto[sizeof(oledImagen)];   //el ultimo cuadro terminado (lo lee /pantalla)

enum EstadoAgua { AGUA_BIEN, PH_BAJO, PH_ALTO, AGUA_TURBIA, SIN_DATO };

// letras 5x7, cada byte es una columna y el bit 0 es el punto de arriba
// la "o" hace de signo de grados - OJO simulador_pantalla.py lee esta tabla tal cual (busca el
// texto entre LETRAS y el titulo de "Control de la pantalla") asi q no meter llaves
// ni cambiarle el titulo
const char LETRAS_ORDEN[] = " -.0123456789?ABCDEFGHIJKLMNOPQRSTUVWXYZpou+%";
const uint8_t LETRAS[][5] = {
  {0x00,0x00,0x00,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
  {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
  {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x02,0x01,0x51,0x09,0x06},
  {0x7C,0x12,0x11,0x12,0x7C}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
  {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
  {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
  {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
  {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
  {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
  {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
  {0x7C,0x14,0x14,0x14,0x08}, {0x06,0x09,0x09,0x06,0x00}, {0x3C,0x40,0x40,0x20,0x7C},
  {0x08,0x08,0x3E,0x08,0x08}, {0x23,0x13,0x08,0x64,0x62}
};

// ---- Control de la pantalla ----

void oledComando(uint8_t c) {
  Wire1.beginTransmission(oledDireccion);
  Wire1.write(0x00);
  Wire1.write(c);
  Wire1.endTransmission();
}

bool oledBuscar() {
  for (uint8_t d : { 0x3C, 0x3D }) {
    Wire1.beginTransmission(d);
    if (Wire1.endTransmission() == 0) { oledDireccion = d; return true; }
  }
  return false;
}

//para el diagnostico -> si algo mantiene con tension cada linea (la pantalla lo hace cuando
// esta conectada y con energia)
bool oledSdaConTension = false, oledSclConTension = false;

bool oledProbar(int sda, int scl) {
  Wire1.end();
  Wire1.begin(sda, scl, 400000);
  if (oledBuscar()) { oledSda = sda; oledScl = scl; return true; }
  Wire1.end();
  return false;
}

// busca la pantalla en sus pines, en los dos ordenes, con enTodos = true prueba ademas todas las
// parejas de PINES_PANTALLA, por si los cables quedaron en otros
bool oledConectar(bool enTodos) {
  Wire1.end();
  pinMode(oledSda, INPUT_PULLDOWN);
  pinMode(oledScl, INPUT_PULLDOWN);
  delay(3);
  oledSdaConTension = digitalRead(oledSda);
  oledSclConTension = digitalRead(oledScl);
  if (oledProbar(oledSda, oledScl) || oledProbar(oledScl, oledSda)) return true;
  if (!enTodos) return false;
  for (int a : PINES_PANTALLA)
    for (int b : PINES_PANTALLA)
      if (a != b && oledProbar(a, b)) return true;
  return false;
}

void oledEncender() {
  const uint8_t pasos[] = { 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0xAD, 0x8B, 0x8D, 0x14,
                            0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0x22, 0xDB, 0x35, 0xA4, 0xA6, 0xAF };
  for (uint8_t c : pasos) oledComando(c);
}

//manda la imagen - si la pantalla deja de contestar, se pone en 0 y la tarea la vuelve a buscar
void oledMostrar() {
  for (int pagina = 0; pagina < 8; pagina++) {
    Wire1.beginTransmission(oledDireccion);
    Wire1.write(0x00);
    Wire1.write(0xB0 | pagina);
    Wire1.write(0x00 | (OLED_DESPLAZAMIENTO & 0x0F));
    Wire1.write(0x10 | (OLED_DESPLAZAMIENTO >> 4));
    if (Wire1.endTransmission() != 0) { oledDireccion = 0; return; }
    for (int x = 0; x < OLED_ANCHO; x += 32) {
      Wire1.beginTransmission(oledDireccion);
      Wire1.write(0x40);
      Wire1.write(&oledImagen[pagina * OLED_ANCHO + x], 32);
      Wire1.endTransmission();
    }
  }
}

// ---- dibujo ----
// "on": 0 apaga, 1 prende, 2 tablero (un punto si, otro no: se ve gris), 3 sombrea (apaga uno de
// cada dos), 4 invierte (oscuro sobre lo claro y claro sobre lo oscuro)
// para probar cambios sin el equipo: simulador_pantalla.py repite estas mismas operaciones

void punto(int x, int y, int on) {
  if (x < 0 || x >= OLED_ANCHO || y < 0 || y >= OLED_ALTO) return;
  if (on == 2) on = (x + y) & 1;
  else if (on == 3) { if ((x + y) & 1) return; on = 0; }
  uint8_t& b = oledImagen[(y / 8) * OLED_ANCHO + x];
  if (on == 4) b ^= 1 << (y & 7);
  else if (on) b |= 1 << (y & 7);
  else b &= ~(1 << (y & 7));
}

void caja(int x, int y, int w, int h, int on) {
  for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) punto(x + i, y + j, on);
}

void disco(int cx, int cy, int r, int on) {
  const float limite = r * r + r * 0.6f;   // se calcula una vez, no por punto
  for (int j = -r; j <= r; j++)
    for (int i = -r; i <= r; i++)
      if (i * i + j * j <= limite) punto(cx + i, cy + j, on);
}

void linea(int x0, int y0, int x1, int y1, int on) {
  int dx = abs(x1 - x0), dy = -abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
  while (true) {
    punto(x0, y0, on);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * e;
    if (e2 >= dy) { e += dy; x0 += sx; }
    if (e2 <= dx) { e += dx; y0 += sy; }
  }
}

int anchoTexto(const char* s) { return strlen(s) * 6 - 1; }

//texto con la letra 5x7, k = 1 normal, k = 2 al doble (10x14), lo que no esta en la tabla sale como "?"
void letras(int x, int y, const char* s, int k, int on) {
  for (; *s; s++, x += 6 * k) {
    const char* p = strchr(LETRAS_ORDEN, *s);
    const uint8_t* g = LETRAS[p ? p - LETRAS_ORDEN : 13];
    for (int c = 0; c < 5; c++)
      for (int f = 0; f < 7; f++)
        if (g[c] >> f & 1) {
          if (k == 1) punto(x + c, y + f, on);
          else caja(x + c * k, y + f * k, k, k, on);
        }
  }
}
void texto(int x, int y, const char* s, int on) { letras(x, y, s, 1, on); }
void textoGrande(int x, int y, const char* s, int on) { letras(x, y, s, 2, on); }

void elipse(int cx, int cy, float rx, float ry, int on) {
  const float rx2 = rx * rx, ry2 = ry * ry;   // fuera de los bucles -> eran dos multiplicaciones x punto
  for (int j = -(int)ry; j <= (int)ry; j++) {
    const float fila = j * j / ry2;
    if (fila > 1) continue; // esa fila no toca la elipse, ni se mira
    for (int i = -(int)rx; i <= (int)rx; i++)
      if (i * i / rx2 + fila <= 1) punto(cx + i, cy + j, on);
  }
}

int red(float v) { return (int)lroundf(v); }

// mancha redonda: centro y radio con decimales, asi las formas se mueven suave
void mancha(float cx, float cy, float r, int on) {
  if (r < 0.4f) return;
  if (r < 1) { punto(red(cx), red(cy), on); return; }
  for (int j = floorf(cy - r); j <= ceilf(cy + r); j++)
    for (int i = floorf(cx - r); i <= ceilf(cx + r); i++)
      if ((i - cx) * (i - cx) + (j - cy) * (j - cy) <= r * r) punto(i, j, on);
}

//tira curva de (x0,y0) a (x2,y2) pasando cerca de (x1,y1), el grosor va de r0 a r2 y, con
// "panza",engorda en el medio como una hoja
void tira(float x0, float y0, float x1, float y1, float x2, float y2, float r0, float r2, int on, float panza = 0) {
  int n = (int)(fabsf(x2 - x0) + fabsf(y2 - y0) + fabsf(x1 - x0) + fabsf(y1 - y0)) + 2;
  for (int k = 0; k <= n; k++) {
    float s = (float)k / n, a = (1 - s) * (1 - s), b = 2 * s * (1 - s), c = s * s;
    mancha(a * x0 + b * x1 + c * x2, a * y0 + b * y1 + c * y2, r0 + (r2 - r0) * s + panza * sinf(PI * s), on);
  }
}

//dibujito de pocos puntos: cada nro es una fila y, de sus "ancho" bits,el mas alto es el punto
// de la izquierda, se puede poner en espejo o girar de a cuartos de vuelta (solo cuadrados)
void sello(int x, int y, const uint8_t* filas, int alto, int ancho, int on, bool espejo = false, int giro = 0) {
  for (int j = 0; j < alto; j++)
    for (int i = 0; i < ancho; i++)
      if (filas[j] >> (ancho - 1 - i) & 1) {
        int a = espejo ? ancho - 1 - i : i, b = j;
        for (int g = 0; g < giro; g++) { int c = a; a = ancho - 1 - b; b = c; }
        punto(x + a, y + b, on);
      }
}

const uint8_t OJO_FELIZ[] = { 0b0111110, 0b1100011, 0b1000001 };  // arco hacia arriba: cerrado, sonriendo
const uint8_t OJO_TRISTE[] = { 0b1000001, 0b1100011, 0b0111110 };  //arco hacia abajo
const uint8_t OJO_APRETADO[] = { 0b11000, 0b00110, 0b00001, 0b00110, 0b11000 };  // ">"
const uint8_t OJO_MAREADO[] = { 0b0011100, 0b0100010, 0b1001101, 0b1010001, 0b1010010, 0b0001100, 0 };  // espiral
const uint8_t BRAZO[] = { 0b10000, 0b10000, 0b10001, 0b10001, 0b01110 }; // bracito recogido (el izquierdo)
const uint8_t DESTELLO[] = { 0b0001000, 0b0001000, 0b0011100, 0b1110111, 0b0011100, 0b0001000, 0b0001000 };
const uint8_t DESTELLO_CHICO[] = { 0b010, 0b111, 0b010 };
const uint8_t GOTA[] = { 0b010, 0b010, 0b111, 0b111, 0b010 };
const uint8_t BURBUJA[] = { 0b01110, 0b10001, 0b10001, 0b10001, 0b01110 };
const uint8_t BURBUJA_CHICA[] = { 0b010, 0b101, 0b010 };

const uint8_t OJO_FELIZ_CHICO[] = { 0b01110, 0b10001, 0b10001 };   //el arco de los ojos del pinguino
const uint8_t NARIZ[] = { 0b0111110, 0b1111111, 0b0111110, 0b0011100 };  //nariz ancha de la nutria
const uint8_t PATITAS[] = { 0b0011100, 0b0100010, 0b0100010, 0b0100010 };   // patas de la nutria, recogidas sobre la barriga
const uint8_t PICO[] = { 0b01110, 0b11111 };  // la parte de arriba del pico

// branquias del ajolote: base x,y, punta x,y y grosor, desde el centro de la cabeza
//las de la izquierda son mas largas porque en la foto esta un poco de lado
const float BRANQUIAS[6][5] = { { -12, -7, -22, -13, 1.5f }, { -14, 0, -25, -1, 1.6f }, { -12, 7, -20, 10, 1.5f },
                                { 11, -9, 15, -13, 1.4f },   { 14, -3, 22, -6, 1.3f },  { 14, 4, 20, 6, 1.3f } };

// ---- las tres mascotas: una por equipo ----
enum Mascota { AJOLOTE, NUTRIA, PINGUINO };
int mascota = AJOLOTE;  // la de este equipo: la pone el programa principal al arrancar
const uint8_t CENTROS[3][2] = { { 32, 20 }, { 24, 16 }, { 29, 16 } };   // centro de la cabeza de cada una
// donde va la cara, desde el centro de la cabeza:
// ojo izq x, ojo der x,ojos y, boca x, boca y, mejilla izq x, mejilla der x, mejillas y
const int8_t CARAS[3][8] = { { -8, 9, 0, 1, 6, -12, 10, 5 }, { -8, 8, -1, 1, 4, 0, 0, 0 }, { -7, 7, 0, 0, 1, -11, 8, 3 } };

struct Pose {
  int cx, cy;   // centro de la cabeza, ya con el movimiento
  int inc, paso; // cabeza ladeada (+1 sube el lado derecho), que pata estira (0 = ninguna)
  float tf;  // el tiempo con decimales
  float caida, onda, colea; // branquias/orejas/aletas caidas, cuanto se mecen, punta de la cola
  float cg, sg; // coseno y seno de la cabeza ladeada
  float brazoI, brazoD; // NAN = recogido, con numero = estirado, mano a esa altura
  bool saluda;
};

// el movimiento de cada estado del agua es igual pa las tres mascotas
Pose poseDe(EstadoAgua est, int cx, int cy, unsigned long t) {
  Pose p;
  float tf = p.tf = t % 1048576UL;   // se reinicia cada 17 min: asi no pierde finura con los dias
  float sube = 1.2f * sinf(tf / 450.0f), mueve = 0;   // flota
  p.inc = 0; p.caida = 0; p.onda = 0.22f; p.paso = 1; p.saluda = false;
  p.colea = sinf(tf / 260.0f) * 1.5f;
  if (est == AGUA_BIEN) {
    float f = tf / 330.0f;
    sube = -fabsf(sinf(f)) * 2.2f;  // baila a brinquitos
    p.inc = (int)(f / PI) % 2 == 0 ? 1 : -1;  //y ladea la cabeza en cada brinco
    p.paso = p.inc;
    p.colea = sinf(tf / 150.0f) * 2.5f;
    p.saluda = t % 5200 < 1700;
  } else if (est == PH_BAJO) {
    mueve = red(sinf(tf / 35.0f)); sube = 0;   //tiembla
    p.caida = 0.55f; p.onda = 0.05f; p.colea = 3 + sinf(tf / 40.0f) * 0.6f; p.paso = 0;
  } else if (est == PH_ALTO) {
    mueve = sinf(tf / 420.0f) * 2.4f;   // se tambalea, mareado
    p.inc = mueve > 1 ? -1 : mueve < -1 ? 1 : 0;
    p.onda = 0.32f; p.paso = p.inc;
  } else if (est == AGUA_TURBIA) {
    sube = 1 + 0.5f * sinf(tf / 700.0f);   // decaido
    p.caida = 0.6f; p.onda = 0.04f; p.colea = 4; p.paso = 0;
  } else {
    p.inc = (t / 2400) % 2 == 0 ? 1 : 0;  // ladea la cabeza, curioso
  }
  p.cx = cx + red(mueve);
  p.cy = cy + red(sube);
  p.cg = cosf(p.inc * 0.10f); p.sg = sinf(p.inc * 0.10f);
  p.brazoI = p.brazoD = NAN;
  if (p.saluda) p.brazoD = -5 + sinf(tf / 110.0f) * 2;
  if (est == PH_BAJO) p.brazoI = p.brazoD = -2 + sinf(tf / 40.0f);
  if (est == PH_ALTO) { p.brazoI = sinf(tf / 200.0f) * 4 - 1; p.brazoD = -p.brazoI - 2; }
  return p;
}

void cuerpoAjolote(EstadoAgua est, const Pose& p) {
  const int cx = p.cx, cy = p.cy;

  // cola: una hoja hacia la izquierda con su borde interior
  for (int k = 0; k < 3; k++) {
    float dr = k ? k + 1 : 0;  // 0, 2 y 3: la hoja, el borde oscuro y el centro
    tira(cx - 7, cy + 25, cx - 15, cy + 25.5f + p.colea * 0.4f, cx - 22, cy + 22.5f + p.colea, 4.6f - dr, 1.8f - dr, k != 1, 0.9f);
  }

  // branquias: tres hojas por lado, en gris
  for (int k = 0; k < 6; k++) {
    float bx = BRANQUIAS[k][0], by = BRANQUIAS[k][1], r = BRANQUIAS[k][4];
    float dx = BRANQUIAS[k][2] - bx, dy = BRANQUIAS[k][3] - by;
    float a = (bx < 0 ? -p.caida : p.caida) + p.onda * sinf(p.tf / 280.0f + k * 1.3f);
    float tx = bx + dx * cosf(a) - dy * sinf(a), ty = by + dx * sinf(a) + dy * cosf(a);  // la punta gira sobre la base
    float gx = bx * p.cg + by * p.sg, gy = by * p.cg - bx * p.sg;   // y la cabeza ladeada se las lleva
    float hx = tx * p.cg + ty * p.sg, hy = ty * p.cg - tx * p.sg;
    tira(cx + gx, cy + gy, cx + (gx + hx) / 2, cy + (gy + hy) / 2, cx + hx, cy + hy, r, r, 2, r);
  }

  //patas: una estirada y la otra recogida, al bailar se turnan
  if (p.paso > 0) {
    tira(cx - 1, cy + 26, cx - 1, cy + 28, cx - 1, cy + 30, 1.9f, 1.7f, 1);
    tira(cx + 9, cy + 25, cx + 10, cy + 26, cx + 12, cy + 27.5f, 2.0f, 1.9f, 1);
  } else if (p.paso < 0) {
    tira(cx + 7, cy + 26, cx + 7, cy + 28, cx + 7, cy + 30, 1.9f, 1.7f, 1);
    tira(cx - 3, cy + 25, cx - 4, cy + 26, cx - 6, cy + 27.5f, 2.0f, 1.9f, 1);
  } else {
    mancha(cx - 1, cy + 28.5f, 2.0f, 1);
    mancha(cx + 7, cy + 28.5f, 2.0f, 1);
  }

  // cuerpo y brazos
  elipse(cx + 3, cy + 18, 10.5f, 9.5f, 1);
  caja(cx - 3, cy + 24, 13, 4, 1);
  if (isnan(p.brazoI)) sello(cx - 4, cy + 16, BRAZO, 5, 5, 0);
  else tira(cx - 4, cy + 18, cx - 7, cy + 18, cx - 10, cy + 18 + p.brazoI, 2.0f, 1.7f, 1);
  if (isnan(p.brazoD)) sello(cx + 5, cy + 15, BRAZO, 5, 5, 0, true);
  else tira(cx + 10, cy + 18, cx + 13, cy + 18, cx + 16, cy + 18 + p.brazoD, 2.0f, 1.7f, 1);

  //cabeza, con un borde oscuro que la separa de branquias y cuerpo
  elipse(cx, cy, 16.5f, 14.5f, 0);
  elipse(cx, cy, 15.5f, 13.5f, 1);

  if (est == AGUA_TURBIA) {   // manchas de suciedad
    mancha(cx + 9, cy + 23, 2.8f, 3);
    mancha(cx + 2, cy - 9, 2.6f, 3);
    mancha(cx + 10, cy + 9, 2.3f, 3);
  }
}

void cuerpoNutria(EstadoAgua est, const Pose& p) {
  const int cx = p.cx, cy = p.cy;

  //mano izquierda arriba (saludando o del susto): va por detras de la cabeza
  float mano = isnan(p.brazoD) ? p.brazoI : p.brazoD;
  if (!isnan(mano))
    for (int k = 0; k < 2; k++) tira(cx - 7, cy + 17, cx - 12, cy + 17, cx - 15, cy + 16 + mano, 2.8f - k, 2.5f - k, !k);

  // cuerpo acurrucado y cola gruesa que sube por la derecha hasta una punta: contorno claro, oscuros por dentro
  for (int k = 0; k < 2; k++) {
    elipse(cx + 3, cy + 20, 14.3f - k, 13.3f - k, !k);
    tira(cx + 14, cy + 27, cx + 27, cy + 28, cx + 23.5f, cy + 12.5f + p.colea * 0.8f, 5.5f - k, 1.8f - k, !k, 1.2f);
  }

  // barriga clara
  elipse(cx + 8, cy + 21, 7.6f, 7.2f, 1);

  //orejitas en contorno: con el agua mal se agachan
  for (int lado = -1; lado <= 1; lado += 2) {
    float ex = lado * 14.5f, ey = -6 + p.caida * 4;
    float ox = cx + ex * p.cg + ey * p.sg, oy = cy + ey * p.cg - ex * p.sg;
    mancha(ox, oy, 2.9f, 1);
    mancha(ox, oy, 1.9f, 0);
  }

  // cabeza grande y clara, con borde oscuro q la separa de las orejas y el cuerpo
  elipse(cx, cy + 1, 16.5f, 13.5f, 0);
  elipse(cx, cy, 15.5f, 12.5f, 1);

  // manos sobre el pecho (la izquierda, si no esta levantada) y patas recogidas sobre la barriga
  if (isnan(mano)) {
    elipse(cx - 2, cy + 18, 4.2f, 3.7f, 1);
    elipse(cx - 2, cy + 18, 3.2f, 2.7f, 0);
  }
  int tiembla = isnan(p.brazoI) ? 0 : red(p.brazoI + 2);
  mancha(cx + 10.5f, cy + 16 + tiembla, 2.8f, 1);
  mancha(cx + 10.5f, cy + 16 + tiembla, 1.8f, 0);
  mancha(cx + 4, cy + 27, 2.0f, 0);
  caja(cx + 2, cy + 27, 5, 3, 0);
  sello(cx + 1, cy + 25, PATITAS, 4, 7, 1);

  // bigotes -> oscuros sobre la cabeza y claros al salir
  int cae = red(p.caida * 3);
  linea(cx - 11, cy + 4, cx - 19, cy + 2 + cae, 4);
  linea(cx - 11, cy + 6, cx - 19, cy + 8 + cae, 4);
  linea(cx + 11, cy + 3, cx + 19, cy + 1 + cae, 4);
  linea(cx + 11, cy + 5, cx + 19, cy + 6 + cae, 4);

  if (est == AGUA_TURBIA) {   // manchas de suciedad
    mancha(cx + 11, cy + 23, 2.0f, 3);
    mancha(cx + 5, cy - 8, 2.4f, 3);
    mancha(cx - 7, cy + 9, 2.0f, 3);
  }
}

void cuerpoPinguino(EstadoAgua est, const Pose& p) {
  const int cx = p.cx, cy = p.cy;
  const int ladea = -p.inc;   //la cabeza se va hacia el lado que baja

  //aletas - con el agua bien la derecha va siempre arriba, saludando, si no, cuelgan y aletean
  float bi = p.brazoI, bd = p.brazoD;
  if (est == AGUA_BIEN && isnan(bd)) bd = -4.5f + sinf(p.tf / 400.0f);
  float ix = -21, iy = 13 + bi, dx = 19, dy = 5 + bd;
  if (isnan(bi)) {
    float a = 1.05f + p.onda * sinf(p.tf / 280.0f) - p.caida * 0.6f;
    ix = -11 - 10.5f * sinf(a);
    iy = 13 + 8 * cosf(a);
  }
  if (isnan(bd)) {
    float a = 0.9f + p.onda * sinf(p.tf / 280.0f + 1) - p.caida * 0.5f;
    dx = 13 + 9 * sinf(a);
    dy = 11 + 9 * cosf(a);
  }

  //aletas anchas, cabeza y cuerpo: todo en contorno, oscuro x dentro
  for (int k = 0; k < 2; k++) {
    tira(cx - 9, cy + 13, cx - 15, cy + 13, cx + ix, cy + iy, 5.2f - k, 2.0f - k, !k);
    tira(cx + 12, cy + 9, cx + 16, cy + 7, cx + dx, cy + dy, 4.6f - k, 2.5f - k, !k);
    elipse(cx - 1 + ladea, cy - 1, 14.5f - k, 12.5f - k, !k);
    elipse(cx + 2, cy + 12, 15.0f - k, 19.5f - k, !k);
  }

  // frente clara: los dos redondeles de la cara (con un pico de plumas oscuras en medio), barbilla y barriga
  elipse(cx - 7 + ladea, cy, 4.8f, 5.8f, 1);
  elipse(cx + 7 + ladea, cy, 4.8f, 5.8f, 1);
  elipse(cx + 1, cy + 7, 9.5f, 4.0f, 1);
  elipse(cx + 3, cy + 17, 11.4f, 12.1f, 1);

  //patas: una apoyada y la otra levantada, al bailar se turnan
  for (int lado = -1; lado <= 1; lado += 2) {
    bool alzada = p.paso == lado;
    int px = cx + (lado > 0 ? (alzada ? 13 : 9) : (alzada ? -9 : -4)), py = cy + (alzada ? 30 : 32);
    elipse(px, py, 3.6f, 2.5f, 1);
    punto(px - 1, py + 2, 0); // los dedos
    punto(px + 1, py + 2, 0);
  }

  if (est == AGUA_TURBIA) {
    mancha(cx + 7, cy + 23, 2.8f, 3);
    mancha(cx - 3, cy + 15, 2.2f, 3);
    mancha(cx + 9, cy + 12, 1.8f, 3);
  }
}

//ojos, boca, mejillas y lagrima - los ojos son iguales en las tres, la boca cambia
void caraMascota(int cual, EstadoAgua est, const Pose& p, unsigned long t) {
  const int inc = p.inc, cy = p.cy;
  const int cx = p.cx - (cual == PINGUINO ? inc : 0); // la cara del pinguino sigue a la cabeza ladeada
  const int8_t* c = CARAS[cual];

  // con el agua bien, el ajolote y el pinguino sonrien con los ojos cerrados y de vez en cuando
  // los abren, la nutria los tiene abiertos y los cierra al parpadear o al saludar
  bool parpadea = t % 4200 < 300;
  bool abiertos = cual == NUTRIA ? (!parpadea && !p.saluda) : parpadea;
  for (int lado = -1; lado <= 1; lado += 2) {
    int ox = cx + (lado > 0 ? c[1] : c[0]), y = cy + c[2] - lado * inc;
    if (est == AGUA_BIEN) {
      if (abiertos) { disco(ox, y, 2, 0); punto(ox - 1, y - 1, 1); }
      else if (cual == PINGUINO) sello(ox - 2, y - 1, OJO_FELIZ_CHICO, 3, 5, 0);
      else sello(ox - 3, y - 1, OJO_FELIZ, 3, 7, 0);
    } else if (est == SIN_DATO) {
      int mira = red(sinf(p.tf / 600.0f) * (cual == AJOLOTE ? 2 : 1));
      if (t % 3200 < 120) caja(ox - 2, y, 5, 1, 0);
      else { disco(ox + mira, y, 2, 0); punto(ox + mira - 1, y - 1, 1); }
    } else if (est == PH_BAJO) {
      sello(ox - 2, y - 2, OJO_APRETADO, 5, 5, 0, lado > 0);   // ojos apretados  > <
    } else if (est == PH_ALTO) {
      sello(ox - 3, y - 3, OJO_MAREADO, 7, 7, 0, false, (t / 130) % 4);  // espirales q giran
    } else {
      sello(ox - 3, y, OJO_TRISTE, 3, 7, 0);
      linea(ox + lado * 3, y - 2, ox - lado * 2, y - 4, 0);  // cejas caidas
    }
  }

  int mx = cx + c[3], my = cy + c[4];
  if (cual == PINGUINO) {
    // pico claro en un hueco oscuro: sonrie (una raya oscura lo parte), se abre del mareo o castanetea de frio
    int abre = est == PH_ALTO ? 2 : (est == AGUA_BIEN || (est == PH_BAJO && (t / 90) % 2 == 0)) ? 1 : 0;
    caja(mx - 3, my - 1, 7, 4 + abre, 0);
    caja(mx - 2, my + 3 + abre, 5, 1, 0);
    sello(mx - 2, my, PICO, 2, 5, 1);
    caja(mx - 1, my + 2 + abre, 3, 1, 1);
    if (est == AGUA_TURBIA) { caja(mx - 2, my + 6, 5, 1, 0); punto(mx - 3, my + 7, 0); punto(mx + 3, my + 7, 0); }
  } else if (cual == NUTRIA) {
    sello(mx - 3, my - 4, NARIZ, 4, 7, 0); //nariz ancha y, debajo, la rayita y la boca
    punto(mx, my, 0);
    my++;
    if (est == AGUA_BIEN) {   // boca en "w"
      punto(mx - 5, my, 0); punto(mx, my, 0); punto(mx + 5, my, 0);
      caja(mx - 4, my + 1, 4, 1, 0);
      caja(mx + 1, my + 1, 4, 1, 0);
      if (p.saluda) caja(mx - 1, my + 2, 3, 2, 0); // boca abierta
    } else if (est == PH_BAJO) {
      for (int i = -4; i <= 4; i++) punto(mx + i, my + 1 + red(sinf(i * 1.5f + p.tf / 90.0f) * 1.2f), 0);
    } else if (est == PH_ALTO) {
      disco(mx, my + 2, 2, 0);
      punto(mx, my + 2, 1);
    } else if (est == AGUA_TURBIA) {
      caja(mx - 2, my + 1, 5, 1, 0);
      punto(mx - 3, my + 2, 0);
      punto(mx + 3, my + 2, 0);
    } else {
      caja(mx - 1, my + 1, 3, 1, 0);
    }
  } else {
    if (est == AGUA_BIEN) {
      caja(mx - 3, my, 7, 1, 0);
      punto(mx - 4, my - 1, 0);
      punto(mx + 4, my - 1, 0);
      punto(mx + 4 * inc, my - 2, 0);  // la comisura del lado que sube
      if (p.saluda) { caja(mx - 2, my + 1, 5, 1, 0); caja(mx - 1, my + 2, 3, 1, 0); } // boca abierta
    } else if (est == PH_BAJO) {
      for (int i = -4; i <= 4; i++) punto(mx + i, my + red(sinf(i * 1.5f + p.tf / 90.0f) * 1.2f), 0);
    } else if (est == PH_ALTO) {
      disco(mx, my + 1, 2, 0);
      punto(mx, my + 1, 1);
    } else if (est == AGUA_TURBIA) {
      caja(mx - 2, my, 5, 1, 0);
      punto(mx - 3, my + 1, 0);
      punto(mx + 3, my + 1, 0);
    } else {
      caja(mx - 1, my, 3, 1, 0);
    }
  }

  if (cual != NUTRIA && (est == AGUA_BIEN || est == SIN_DATO || est == PH_ALTO)) {  // mejillas (la nutria ya lleva bigotes)
    caja(cx + c[5], cy + c[7] + inc, 4, 2, 3);
    caja(cx + c[6], cy + c[7] - inc, 4, 2, 3);
  }

  if (est == AGUA_TURBIA) { // lagrima
    int f = (t / 110) % 16;
    if (f < 9) caja(cx + c[0] - 1, cy + c[2] + 4 + f, 1, 2, 0);
  }
}

void burbuja(int x, int y, bool grande) {
  if (grande) sello(x - 2, y - 2, BURBUJA, 5, 5, 1);
  else sello(x - 1, y - 1, BURBUJA_CHICA, 3, 3, 1);
}

void brillo(int x, int y, int n) {
  if (n == 0) punto(x, y, 1);
  else if (n == 1) sello(x - 1, y - 1, DESTELLO_CHICO, 3, 3, 1);
  else sello(x - 3, y - 3, DESTELLO, 7, 7, 1);
}

// lo q rodea a la mascota: burbujas, destellos, gotas o un signo de pregunta
void entorno(EstadoAgua est, const Pose& p, unsigned long t) {
  const int cx = p.cx, cy = p.cy;
  if (est == AGUA_BIEN) {
    const int bx[] = { 3, 54 };
    for (int k = 0; k < 2; k++) { // burbujas q suben por los costados
      int f = (t / 70 + k * 13) % 26;
      burbuja(bx[k] + red(sinf((f + k * 4) / 4.0f)), 50 - f, k);
    }
    const int8_t tam[] = { 0, 1, 2, 1, 0 };   //destellos que titilan
    int f = (t / 160) % 12;
    if (f < 5) brillo(55, 5, tam[f]);
    f = (t / 160 + 6) % 12;
    if (f < 5) brillo(3, 2, tam[f] > 1 ? 1 : tam[f]);
  }
  if (est == PH_BAJO) {
    const int gx[] = { 2, 52, 56 }, gy[] = { 26, 26, 34 };
    for (int k = 0; k < 3; k++) {  // gotas acidas q caen
      int y = gy[k] + (t / 45 + k * 11) % 30;
      if (y < 50) sello(gx[k], y, GOTA, 5, 3, 1);
    }
    if (t % 500 < 250) { // gotitas de susto
      linea(cx + 12, cy - 15, cx + 13, cy - 16, 1);
      linea(cx - 13, cy - 15, cx - 14, cy - 16, 1);
    }
  }
  if (est == PH_ALTO) {
    const int bx[] = { 3, 56, 52, 2 };
    for (int k = 0; k < 4; k++) {  //burbujas de jabon
      int f = (t / 55 + k * 8) % 30;
      if (k != 2 || f < 18) burbuja(bx[k] + red(sinf((f + k) / 4.0f)), 50 - f, k % 2 == 0);
    }
  }
  if (est == SIN_DATO && t % 1000 < 650) texto(53, 1 + (t / 1000) % 2, "?", 1);
}

//una mascota completa -> cada estado del agua tiene su cara y su movimiento
void dibujarMascota(int cual, EstadoAgua est, unsigned long t) {
  if (est == AGUA_TURBIA) {   // particulas que caen, por detras
    for (int k = 0; k < 10; k++) {
      int f = (t / 70 + k * 7) % 52, x = (k * 23) % 59;
      punto(x, f, 1);
      if (k % 3 == 0) punto(x + 1, f, 1);
    }
  }
  Pose p = poseDe(est, CENTROS[cual][0], CENTROS[cual][1], t);
  if (cual == NUTRIA) cuerpoNutria(est, p);
  else if (cual == PINGUINO) cuerpoPinguino(est, p);
  else cuerpoAjolote(est, p);
  caraMascota(cual, est, p, t);
  entorno(est, p, t);
}

// que le pasa al agua ahorita, con dos alertas a la vez se van turnando
EstadoAgua estadoDelAgua(unsigned long t) {
  bool phOk = phValido(), turbOk = turbValida();
  if (!phOk && !turbOk) return SIN_DATO;
  EstadoAgua alertas[3];
  int n = 0;
  if (phOk && ph < PH_MIN) alertas[n++] = PH_BAJO;
  if (phOk && ph > PH_MAX) alertas[n++] = PH_ALTO;
  if (turbOk && ntu > NTU_MAX) alertas[n++] = AGUA_TURBIA;
  return n ? alertas[(t / 3500) % n] : AGUA_BIEN;
}

// para ver una animacion a pedido unos segundos: /pantalla?estado=0 a 4
// y, pa ver la mascota de otro equipo, &mascota=0 (ajolote), 1 (nutria) o 2 (pinguino)
EstadoAgua oledDemoEstado = AGUA_BIEN;
int oledDemoMascota = AJOLOTE;
unsigned long oledDemoHasta = 0;

// ---- encendido de la pantalla, vistas y avisos ----
// un toque del boton la prende y, si ya estaba prendida, pasa a la vista siguiente
// sin tocar nada se apaga sola al minuto, tambien se prende sola cuando el agua sale de los limites
// sin boton: /pantalla?vista=0 (mascota), 1 (valores grandes) o 2 (estado del equipo)
const long OLED_SE_APAGA_MS = 60000;
volatile unsigned long oledPedida = 0; //la ultima vez que pidieron verla (el arranque cuenta)
volatile int oledVista = 0;  //0 mascota, 1 valores en grande, 2 estado del equipo
volatile unsigned long oledAvisoHasta = 0;  // hasta ahi se muestra "MUESTRA GUARDADA"
bool oledPrendida = true;  // lo ultimo que se le ordeno a la pantalla

bool oledSeVe(unsigned long t) {
  return calPaso || (long)(t - oledPedida) < OLED_SE_APAGA_MS || t < oledDemoHasta || t < oledAvisoHasta;
}

// barra de abajo: cuanta memoria interna va usada, el estado del agua y si hay internet
void barraDeAbajo(EstadoAgua est) {
  const char* mensajes[] = { "AGUA BIEN", "PH BAJO", "PH ALTO", "AGUA TURBIA", "SIN DATO" };
  const char* m = numLectura ? mensajes[est] : "INICIANDO";
  caja(0, 54, OLED_ANCHO, 10, true);
  char mem[8];
  int usada = memoriaPct; // una sola lectura: la cambia el otro nucleo
  if (usada >= 0) snprintf(mem, sizeof(mem), "%d%%", usada);
  else strcpy(mem, "---");
  texto(2, 55, mem, false);
  texto((OLED_ANCHO - anchoTexto(m)) / 2, 55, m, false);
  if (hayInternet()) {   // tres barras de senal
    caja(117, 60, 2, 2, false);
    caja(120, 58, 2, 4, false);
    caja(123, 56, 2, 6, false);
  } else {   // una equis: sin internet
    linea(118, 56, 123, 61, false);
    linea(123, 56, 118, 61, false);
  }
}

//vista 2 -> como esta el equipo (lo mismo q /diagnostico, resumido) - la letra solo tiene mayusculas
void vistaEstado() {
  const char* const nombres[] = { "AJOLOTE", "NUTRIA", "PINGUINO" };
  char l[24];
  bool wifi = WiFi.status() == WL_CONNECTED;
  caja(0, 0, OLED_ANCHO, 9, true);
  snprintf(l, sizeof(l), "EQUIPO %d %s", equipo, nombres[mascota]);
  texto(2, 1, l, false);
  snprintf(l, sizeof(l), "WIFI %s  INTERNET %s", wifi ? "SI" : "NO", hayInternet() ? "SI" : "NO");
  texto(1, 10, l, true);
  if (wifi) snprintf(l, sizeof(l), "IP %s", WiFi.localIP().toString().c_str());
  else strcpy(l, "SIN WIFI DEL CELULAR");
  texto(1, 19, l, true);
  snprintf(l, sizeof(l), "RED RIOSANJUAN-E%d", equipo); //la red propia del equipo
  texto(1, 28, l, true);
  if (gps.charsProcessed() < 10) strcpy(l, "GPS NO");
  else if (gpsConUbicacion()) snprintf(l, sizeof(l), "GPS SI %d SAT", (int)gps.satellites.value());
  else strcpy(l, "GPS BUSCANDO");
  texto(1, 37, l, true);
  texto(126 - anchoTexto("ADS SI"), 37, adsDireccion ? "ADS SI" : "ADS NO", true);
  int usada = memoriaPct;
  if (usada >= 0) snprintf(l, sizeof(l), "MEM %d%%  RELOJ %s", usada, hayReloj ? "SI" : "NO");
  else snprintf(l, sizeof(l), "MEM ---  RELOJ %s", hayReloj ? "SI" : "NO");
  texto(1, 46, l, true);
  snprintf(l, sizeof(l), "TEMP %s PH %s TURB %s", temp > -100 ? "SI" : "NO", phValido() ? "SI" : "NO", turbValida() ? "SI" : "NO");
  texto(1, 55, l, true);
}

//calibracion con el segundo boton: que poner en la sonda, la lectura en vivo y q boton apretar
void vistaCalibrar() {
  char l[24];
  caja(0, 0, OLED_ANCHO, 9, true);
  texto(2, 1, "CALIBRAR", false);
  if (calPaso == 4) {
    texto(1, 14, calResultado & 1 ? "PH GUARDADO" : calResultado & 4 ? "PH NO VALE" : "PH SIN CAMBIO", true);
    if (calResultado & 4) texto(1, 24, "REPITE PH 7 Y PH 4", true);
    texto(1, 38, calResultado & 2 ? "TURBIDEZ GUARDADA" : "TURBIDEZ SIN CAMBIO", true);
    texto(1, 55, "LISTO", true);
    return;
  }
  snprintf(l, sizeof(l), "%d DE 3", calPaso);
  texto(126 - anchoTexto(l), 1, l, false);
  if (calPaso == 3) strcpy(l, "TURBIDEZ AGUA LIMPIA");
  else snprintf(l, sizeof(l), "SONDA EN PH %.2f", calPaso == 1 ? cal.phNeutro : cal.phAcido);
  texto(1, 12, l, true);
  if (calPaso == 3) snprintf(l, sizeof(l), "%.3f", calValor); else snprintf(l, sizeof(l), "%.0f", calValor);
  textoGrande(1, 23, l, true);
  texto(strlen(l) * 12 + 4, 30, calPaso == 3 ? "V" : "MV", true);
  texto(126 - anchoTexto(calEstable ? "QUIETA" : "ESPERA"), 30, calEstable ? "QUIETA" : "ESPERA", true);
  texto(1, 44, calEstable ? "B1 TOMA  B2 SALTA" : "B2 SALTA EL PASO", true);
  texto(1, 55, "B2 3 SEG SALE", true);
}

void dibujarPantalla(unsigned long t) {
  memset(oledImagen, 0, sizeof(oledImagen));
  if (calPaso) { vistaCalibrar(); return; }
  if (t < oledAvisoHasta) {  //acaban de marcar una muestra con el boton
    caja(0, 0, OLED_ANCHO, OLED_ALTO, true);
    caja(2, 2, OLED_ANCHO - 4, OLED_ALTO - 4, false);
    textoGrande(23, 14, "MUESTRA", true);
    textoGrande(17, 36, "GUARDADA", true);
    return;
  }
  bool demo = t < oledDemoHasta;
  int vista = demo ? 0 : oledVista;
  if (vista == 2) { vistaEstado(); return; }

  EstadoAgua est = numLectura ? estadoDelAgua(t) : SIN_DATO;
  int cual = mascota;
  if (demo) { est = oledDemoEstado; cual = oledDemoMascota; }

  //los cuatro valores como texto ("--" si no hay medicion)
  char v[4][10];
  bool hayTemp = numLectura && temp > -100;
  if (numLectura && phValido()) snprintf(v[0], sizeof(v[0]), "%.2f", ph); else strcpy(v[0], "--");
  if (numLectura && turbValida()) snprintf(v[1], sizeof(v[1]), "%.0f", ntu); else strcpy(v[1], "--");
  if (hayTemp) snprintf(v[2], sizeof(v[2]), "%.1f", temp); else strcpy(v[2], "--");
  if (!isnan(ce)) snprintf(v[3], sizeof(v[3]), "%.0f", ce); else strcpy(v[3], "--");

  if (vista == 1) {   // vista 1: valores en grande, dos arriba y dos abajo
    const char* nombres[] = { "pH", "NTU", "TEMP oC", "CE uS" };
    for (int i = 0; i < 4; i++) {
      int x = 2 + (i % 2) * 65, y = 1 + (i / 2) * 27;
      texto(x, y, nombres[i], true);
      textoGrande(x, y + 10, v[i], true);
    }
    for (int y = 0; y < 53; y += 3) punto(64, y, true);
    for (int x = 0; x < OLED_ANCHO; x += 3) punto(x, 26, true);
  } else { // vista 0 -> la mascota y, a su derecha, los valores
    dibujarMascota(cual, est, t);
    if (hayTemp) strcat(v[2], "oC");
    const char* nombres[] = { "pH", "NTU", "TEMP", "CE" };
    for (int i = 0; i < 4; i++) {
      int y = 3 + i * 12;
      texto(62, y, nombres[i], true);
      texto(126 - anchoTexto(v[i]), y, v[i], true);
      if (i < 3) for (int x = 62; x < 126; x += 3) punto(x, y + 9, true);
    }
  }
  barraDeAbajo(est);
}

void tareaPantalla(void*) {
  unsigned long ultimaBusqueda = 0;
  bool alertaAntes = false;
  for (;;) {
    unsigned long t = millis();
    if (!oledDireccion && (ultimaBusqueda == 0 || t - ultimaBusqueda >= 1000)) {  // sin pantalla: la busca una vez por segundo
      ultimaBusqueda = t;
      if (oledConectar(!oledUbicada && t < 15000)) { oledEncender(); oledUbicada = true; oledPrendida = true; }
    }
    if (alerta && !alertaAntes) oledPedida = t; // el agua salio de los limites: se prende sola
    alertaAntes = alerta;
    bool ver = oledSeVe(t);
    dibujarPantalla(t);
    memcpy(oledVisto, oledImagen, sizeof(oledVisto));
    if (oledDireccion) {
      if (ver != oledPrendida) { oledComando(ver ? 0xAF : 0xAE); oledPrendida = ver; } // la prende o la apaga
      if (ver) oledMostrar();
    }
    vTaskDelay(pdMS_TO_TICKS(ver ? 40 : 200));
  }
}

void iniciarPantalla() {
  xTaskCreatePinnedToCore(tareaPantalla, "pantalla", 4096, nullptr, 1, nullptr, 0);
}

//lo q hay en pantalla, en texto (cada letra son 2 puntos de alto), es lo q se ve en /pantalla
String imagenEnTexto() {
  String s;
  s.reserve((OLED_ANCHO + 1) * OLED_ALTO / 2 + 40);
  for (int y = 0; y < OLED_ALTO; y += 2) {
    for (int x = 0; x < OLED_ANCHO; x++) {
      bool a = oledVisto[(y / 8) * OLED_ANCHO + x] & (1 << (y & 7));
      bool b = oledVisto[((y + 1) / 8) * OLED_ANCHO + x] & (1 << ((y + 1) & 7));
      s += a && b ? '#' : a ? '"' : b ? '_' : ' ';
    }
    s += '\n';
  }
  s += oledDireccion ? "Pantalla detectada\n" : "Pantalla NO detectada\n";
  if (!oledSeVe(millis())) s += "Ahora esta apagada: se enciende con el boton (esto es lo que mostraria)\n";
  return s;
}
