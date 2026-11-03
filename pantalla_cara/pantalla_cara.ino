//cara animada para pantalla OLED de 1.3" con I2C (SH1106, 128x64)
// conexion: VCC -> 3V3, GND -> GND, SCL -> D22, SDA -> D21
// no usa librerias de pantalla: el control esta aca mismo
//la cara parpadea, mira a los lados y cambia de animo sola: feliz, pensando
//(con el destello de Claude), dormida, sorprendida y guino
//si la imagen sale corrida o con una raya a un costado, cambiar DESPLAZAMIENTO a 0
// Monitor serie a 115200: enviar la letra v dibuja en texto lo que hay en pantalla;
// enviar un numero del 0 al 5 cambia el animo (0 normal, 1 feliz, 2 pensando, 3 dormido, 4 sorprendido, 5 guino)

#include <Wire.h>

const int PIN_SDA = 21;
const int PIN_SCL = 22;
const int DESPLAZAMIENTO = 2; // 2 para SH1106 (pantallas de 1.3"), 0 para SSD1306 (0.96")

const int ANCHO = 128, ALTO = 64;
uint8_t direccion = 0; // 0x3C o 0x3D, se detecta sola
uint8_t imagen[ANCHO * ALTO / 8];   // un bit por punto

// un valor q se acerca suavemente a su meta en cada cuadro
struct Suave {
  float v, meta;
  void paso(float k) { v += (meta - v) * k; }
};

enum Animo { NORMAL, FELIZ, PENSANDO, DORMIDO, SORPRENDIDO, GUINO };

// ---- forma de la cara ----
const int OJO_X[2] = { 40, 88 };   //centro de cada ojo
const int OJO_Y = 26;
const int BOCA_Y = 54;

Suave ancho   = { 30, 30 };   // tamano de los ojos
Suave alto    = { 36, 36 };
Suave abierto[2] = { { 0, 0 }, { 0, 0 } };   // 1 = ojo abierto, 0 = cerrado
Suave miraX   = { 0, 0 };   // hacia donde mira
Suave miraY   = { 0, 0 };
Suave alegria = { 0, 0 }; //1 = ojos en arco, como al sonreir
Suave parpado = { 0, 0 };   //1 = parpado de arriba totalmente caido
Suave sonrisa = { 3, 3 };  // curva de la boca
Suave bocaO   = { 0, 0 };   // 1 = boca en forma de "o"

Animo animo = DORMIDO;
unsigned long finAnimo = 0; //cuando termina el animo actual
unsigned long proximoParpadeo = 0, finParpadeo = 0;
unsigned long proximaMirada = 0;
unsigned long inicio = 0;

float azar(float a, float b) { return a + (b - a) * (esp_random() & 0xFFFF) / 65535.0f; }

// ---- pantalla ----

void comando(uint8_t c) {
  Wire.beginTransmission(direccion);
  Wire.write(0x00);
  Wire.write(c);
  Wire.endTransmission();
}

bool buscarPantalla() {
  for (uint8_t d : { 0x3C, 0x3D }) {
    Wire.beginTransmission(d);
    if (Wire.endTransmission() == 0) { direccion = d; return true; }
  }
  return false;
}

void encenderPantalla() {
  const uint8_t pasos[] = { 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0xAD, 0x8B, 0x8D, 0x14,
                            0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0x22, 0xDB, 0x35, 0xA4, 0xA6, 0xAF };
  for (uint8_t c : pasos) comando(c);
}

// envia la imagen, si la pantalla deja de contestar, se vuelve a buscar
void mostrar() {
  for (int pagina = 0; pagina < 8; pagina++) {
    Wire.beginTransmission(direccion);
    Wire.write(0x00);
    Wire.write(0xB0 | pagina);
    Wire.write(0x00 | (DESPLAZAMIENTO & 0x0F));
    Wire.write(0x10 | (DESPLAZAMIENTO >> 4));
    if (Wire.endTransmission() != 0) { direccion = 0; return; }
    for (int x = 0; x < ANCHO; x += 32) {
      Wire.beginTransmission(direccion);
      Wire.write(0x40);
      Wire.write(&imagen[pagina * ANCHO + x], 32);
      Wire.endTransmission();
    }
  }
}

// ---- dibujo ----

void punto(int x, int y, bool on) {
  if (x < 0 || x >= ANCHO || y < 0 || y >= ALTO) return;
  uint8_t& b = imagen[(y / 8) * ANCHO + x];
  if (on) b |= 1 << (y & 7);
  else b &= ~(1 << (y & 7));
}

void linea(int x0, int y0, int x1, int y1) {
  int dx = abs(x1 - x0), dy = -abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
  while (true) {
    punto(x0, y0, true);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * e;
    if (e2 >= dy) { e += dy; x0 += sx; }
    if (e2 <= dx) { e += dx; y0 += sy; }
  }
}

// rectangulo relleno con las esquinas redondeadas - con on = false borra
void caja(int x, int y, int w, int h, int r, bool on) {
  if (w <= 0 || h <= 0) return;
  r = min(r, min(w, h) / 2);
  for (int j = 0; j < h; j++) {
    int d = j < r ? r - 1 - j : (j >= h - r ? j - (h - r) : -1);  // q tan dentro de la esquina
    int recorte = d < 0 ? 0 : r - (int)(sqrtf((float)(r * r - d * d)) + 0.5f);
    for (int i = x + recorte; i <= x + w - 1 - recorte; i++) punto(i, y + j, on);
  }
}

void circulo(int cx, int cy, int r, bool on) { caja(cx - r, cy - r, 2 * r, 2 * r, r, on); }

// el destello de Claude: rayos de largo desigual que salen de un centro
void destello(int cx, int cy, float radio, float giro, int rayos) {
  const float largos[] = { 1.0, 0.70, 0.92, 0.64, 1.0, 0.76, 0.88, 0.68, 0.96, 0.72, 0.90, 0.66 };
  for (int i = 0; i < rayos; i++) {
    float a = giro + i * (2 * PI / rayos);
    int x = cx + (int)lroundf(cosf(a) * radio * largos[i]);
    int y = cy + (int)lroundf(sinf(a) * radio * largos[i]);
    linea(cx, cy, x, y);
    if (radio > 12) linea(cx + 1, cy, x + 1, y); // rayos mas gruesos cuando es grande
  }
}

void letraZ(int x, int y, int t) {
  linea(x, y, x + t, y);
  linea(x + t, y, x, y + t);
  linea(x, y + t, x + t, y + t);
}

void dibujarOjo(int i, float sube) {
  // el ojo del lado hacia donde mira se ve un poco mas grande
  float lado = (i == 0 ? -1 : 1) * miraX.v;
  int w = (int)(ancho.v + lado * 0.25f);
  int h = max(3, (int)((alto.v + lado * 0.3f) * abierto[i].v));
  int cx = OJO_X[i] + (int)miraX.v;
  int cy = OJO_Y + (int)(miraY.v + sube);
  caja(cx - w / 2, cy - h / 2, w, h, 9, true);

  if (h > 8) {
    if (parpado.v > 0.03f) caja(cx - w / 2 - 1, cy - h / 2 - 1, w + 2, (int)(h * parpado.v) + 1, 0, false);
    if (alegria.v > 0.03f) {  // la mejilla sube y deja el ojo en arco
      int radio = w;
      int borde = cy + h / 2 - (int)(h * 0.62f * alegria.v);
      circulo(cx, borde + radio, radio, false);
    }
  }
}

void dibujarBoca(float sube) {
  int cx = 64 + (int)(miraX.v * 0.5f);
  int cy = BOCA_Y + (int)(miraY.v * 0.3f + sube);
  if (bocaO.v > 0.5f) {
    circulo(cx, cy + 2, 5, true);
    circulo(cx, cy + 2, 3, false);
    return;
  }
  int medio = 9 + (int)(alegria.v * 3);
  for (int x = -medio; x <= medio; x++) {
    float t = (float)x / medio;
    int y = cy + (int)lroundf(sonrisa.v * (1 - t * t));
    punto(cx + x, y, true);
    punto(cx + x, y + 1, true);
  }
}

void dibujar(unsigned long ahora) {
  memset(imagen, 0, sizeof(imagen));

  if (ahora - inicio < 1500) {   //al encender: el destello crece en el centro
    float t = (ahora - inicio) / 1500.0f;
    destello(64, 32, 4 + 22 * t, t * 2.5f, 12);
    return;
  }

  float sube = 0;
  if (animo == FELIZ) sube = -2.0f * fabsf(sinf(ahora / 160.0f)); // saltitos
  if (animo == DORMIDO) sube = 1.5f * sinf(ahora / 700.0f);  //respira

  dibujarOjo(0, sube);
  dibujarOjo(1, sube);
  dibujarBoca(sube);

  if (animo == PENSANDO) destello(119, 9, 7, ahora / 350.0f, 8);
  if (animo == DORMIDO) {
    for (int k = 0; k < 3; k++) {
      int f = (ahora / 60 + k * 14) % 42;  //cada Z sube y vuelve a empezar
      letraZ(98 + f / 2, 22 - f / 2, 3 + f / 10);
    }
  }
}

// ---- animacion ----

void ponerAnimo(Animo a, unsigned long ahora) {
  animo = a;
  ancho.meta = 30; alto.meta = 36; alegria.meta = 0; parpado.meta = 0; sonrisa.meta = 3; bocaO.meta = 0;
  abierto[0].meta = abierto[1].meta = 1;
  switch (a) {
    case NORMAL:
      finAnimo = ahora + (unsigned long)azar(6000, 14000);
      break;
    case FELIZ:
      alegria.meta = 1; sonrisa.meta = 6;
      finAnimo = ahora + 4000;
      break;
    case PENSANDO:
      parpado.meta = 0.3f; sonrisa.meta = 0; miraX.meta = 6; miraY.meta = -4;
      finAnimo = ahora + 4500;
      break;
    case DORMIDO:
      abierto[0].meta = abierto[1].meta = 0.05f; sonrisa.meta = 1; miraX.meta = 0; miraY.meta = 3;
      finAnimo = ahora + 8000;
      break;
    case SORPRENDIDO:
      ancho.meta = 34; alto.meta = 42; bocaO.meta = 1; miraX.meta = 0; miraY.meta = 0;
      finAnimo = ahora + 1600;
      break;
    case GUINO:
      abierto[1].meta = 0.05f; sonrisa.meta = 6;
      finAnimo = ahora + 1200;
      break;
  }
}

void animar(unsigned long ahora) {
  //cambio de animo
  if (ahora >= finAnimo) {
    if (animo == DORMIDO) ponerAnimo(SORPRENDIDO, ahora);   // se despierta de golpe
    else if (animo != NORMAL) ponerAnimo(NORMAL, ahora);
    else {
      int n = esp_random() % 12;
      ponerAnimo(n < 4 ? FELIZ : n < 7 ? PENSANDO : n < 9 ? GUINO : n < 11 ? SORPRENDIDO : DORMIDO, ahora);
    }
  }

  //parpadeo y mirada (despierta y sin estar sorprendida)
  bool despierta = animo == NORMAL || animo == FELIZ || animo == PENSANDO;
  if (despierta) {
    if (ahora >= proximoParpadeo) {
      finParpadeo = ahora + 110;
      proximoParpadeo = ahora + (unsigned long)(esp_random() % 6 == 0 ? 350 : azar(2000, 6000)); //a veces doble
    }
    float meta = ahora < finParpadeo ? 0.05f : 1;
    abierto[0].meta = meta;
    abierto[1].meta = meta;
  }
  if ((animo == NORMAL || animo == FELIZ) && ahora >= proximaMirada) {
    bool alFrente = esp_random() % 3 == 0;
    miraX.meta = alFrente ? 0 : azar(-10, 10);
    miraY.meta = alFrente ? 0 : azar(-4, 4);
    proximaMirada = ahora + (unsigned long)azar(900, 3500);
  }

  ancho.paso(0.25f); alto.paso(0.25f);
  abierto[0].paso(0.45f); abierto[1].paso(0.45f);
  miraX.paso(0.22f); miraY.paso(0.22f);
  alegria.paso(0.2f); parpado.paso(0.2f); sonrisa.paso(0.2f); bocaO.paso(0.35f);
}

//dibuja en el monitor serie lo que hay en pantalla (cada letra son 2 puntos de alto)
void imprimirImagen() {
  for (int y = 0; y < ALTO; y += 2) {
    char fila[ANCHO + 1];
    for (int x = 0; x < ANCHO; x++) {
      bool a = imagen[(y / 8) * ANCHO + x] & (1 << (y & 7));
      bool b = imagen[((y + 1) / 8) * ANCHO + x] & (1 << ((y + 1) & 7));
      fila[x] = a && b ? '#' : a ? '"' : b ? '_' : ' ';
    }
    fila[ANCHO] = 0;
    Serial.println(fila);
  }
  Serial.printf("animo=%d pantalla=0x%02X\n", animo, direccion);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
}

void loop() {
  static unsigned long ultimaBusqueda = 0;
  unsigned long ahora = millis();

  if (!direccion && ahora - ultimaBusqueda >= 1000) { //sin pantalla: se busca una vez por segundo
    ultimaBusqueda = ahora;
    if (buscarPantalla()) {
      Serial.printf("Pantalla encontrada en 0x%02X\n", direccion);
      encenderPantalla();
      inicio = ahora;
      ponerAnimo(DORMIDO, ahora);
      abierto[0].v = abierto[1].v = 0;
      finAnimo = ahora + 3500; // destello, un momento dormida y se despierta
    } else {
      Serial.println("No encuentro la pantalla. Revise: VCC a 3V3, GND a GND, SCL a D22, SDA a D21");
    }
  }

  animar(ahora);
  dibujar(ahora);
  if (direccion) mostrar();

  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'v') imprimirImagen();
    if (c >= '0' && c <= '5') ponerAnimo((Animo)(c - '0'), ahora);  // 0 normal, 1 feliz, 2 pensando, 3 dormido, 4 sorprendido, 5 guino
  }

  long espera = 33 - (long)(millis() - ahora);  // unos 30 cuadros por segundo
  if (espera > 0) delay(espera);
}
