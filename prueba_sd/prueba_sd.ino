// sonda de microSD -> habla con la tarjeta "a mano" y muestra q hay dentro
//(tipo de tarjeta, capacidad, tabla de particiones y cabecera del sistema de archivos)
//es una herramienta de diagnostico, no forma parte del programa del equipo
//conexion -> VCC a 5V, GND a G, CS al 5, SCK al 18, MISO al 19, MOSI al 23 - monitor serie a 115200

#include <SPI.h>
#include "driver/gpio.h"

const int PIN_CS = 5;
uint8_t sec[512];
bool sdhc = false;

uint8_t cmd(uint8_t c, uint32_t arg) {
  const uint8_t m[5] = { (uint8_t)(0x40 | c), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg };
  uint8_t crc = 0;
  for (uint8_t d : m) {
    for (int i = 0; i < 8; i++) { crc <<= 1; if ((d ^ crc) & 0x80) crc ^= 0x09; d <<= 1; }
  }
  SPI.transfer(0xFF);
  for (uint8_t d : m) SPI.transfer(d);
  SPI.transfer((uint8_t)((crc << 1) | 1));
  uint8_t r = 0xFF;
  for (int i = 0; i < 10 && (r & 0x80); i++) r = SPI.transfer(0xFF);
  return r;
}

void soltar() { digitalWrite(PIN_CS, HIGH); SPI.transfer(0xFF); }

//lee un bloque de datos corto (registros CSD o CID) o un sector de 512 bytes
bool bloque(uint8_t comando, uint32_t arg, uint8_t* destino, int n) {
  bool ok = false;
  digitalWrite(PIN_CS, LOW);
  uint8_t r = cmd(comando, arg), m = 0xFF;
  if (r == 0) {
    unsigned long t = millis();
    while ((m = SPI.transfer(0xFF)) == 0xFF && millis() - t < 500) {}
    if (m == 0xFE) { for (int i = 0; i < n; i++) destino[i] = SPI.transfer(0xFF); SPI.transfer(0xFF); SPI.transfer(0xFF); ok = true; }
  }
  soltar();
  if (!ok) Serial.printf("  comando %u (arg %lu): respuesta %u, marca %02X -> sin datos\n", comando, (unsigned long)arg, r, m);
  return ok;
}

bool leer(uint32_t sector) { return bloque(17, sdhc ? sector : sector << 9, sec, 512); }

uint16_t le16(int o) { return sec[o] | (sec[o + 1] << 8); }
uint32_t le32(int o) { return (uint32_t)sec[o] | ((uint32_t)sec[o + 1] << 8) | ((uint32_t)sec[o + 2] << 16) | ((uint32_t)sec[o + 3] << 24); }

void hex(const char* titulo, int desde, int n) {
  Serial.printf("  %s:", titulo);
  for (int i = 0; i < n; i++) Serial.printf(" %02X", sec[desde + i]);
  Serial.println();
}

void texto(const char* titulo, int desde, int n) {
  Serial.printf("  %s: \"", titulo);
  for (int i = 0; i < n; i++) Serial.print((sec[desde + i] >= 32 && sec[desde + i] < 127) ? (char)sec[desde + i] : '.');
  Serial.println("\"");
}

void cabecera(uint32_t sector) {
  Serial.printf("CABECERA DEL SISTEMA DE ARCHIVOS (sector %lu)\n", (unsigned long)sector);
  hex("primeros bytes", 0, 16);
  texto("nombre (byte 3)", 3, 8);
  Serial.printf("  bytes por sector %u, sectores por grupo %u, reservados %u, copias de la tabla %u\n", le16(11), sec[13], le16(14), sec[16]);
  Serial.printf("  entradas raiz %u, sectores (16 bits) %u, tabla (16 bits) %u, sectores (32 bits) %lu, tabla (32 bits) %lu\n",
                le16(17), le16(19), le16(22), (unsigned long)le32(32), (unsigned long)le32(36));
  texto("tipo en byte 54", 54, 8);
  texto("tipo en byte 82", 82, 8);
  Serial.printf("  firma final: %02X %02X (debe ser 55 AA)\n", sec[510], sec[511]);
}

void sonda() {
  Serial.println("\n======== SONDA DE MICROSD ========");
  SPI.begin(18, 19, 23, PIN_CS);
  gpio_pullup_en((gpio_num_t)19);
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 12; i++) SPI.transfer(0xFF);

  digitalWrite(PIN_CS, LOW); uint8_t r0 = cmd(0, 0); soltar();
  uint8_t eco[4];
  digitalWrite(PIN_CS, LOW); uint8_t r8 = cmd(8, 0x1AA); for (uint8_t &x : eco) x = SPI.transfer(0xFF); soltar();
  bool v2 = (r8 == 1);
  uint8_t r41 = 0xFF;
  unsigned long t0 = millis();
  do {
    digitalWrite(PIN_CS, LOW); cmd(55, 0); soltar();
    digitalWrite(PIN_CS, LOW); r41 = cmd(41, v2 ? 0x40000000UL : 0); soltar();
    if (r41 == 0) break;
    delay(20);
  } while (millis() - t0 < 2000);
  uint8_t ocr[4] = { 0 };
  digitalWrite(PIN_CS, LOW); cmd(58, 0); for (uint8_t &x : ocr) x = SPI.transfer(0xFF); soltar();
  sdhc = v2 && (ocr[0] & 0x40);
  digitalWrite(PIN_CS, LOW); uint8_t r16 = cmd(16, 512); soltar();

  Serial.println("TARJETA");
  Serial.printf("  reinicio %u, identificacion %u, lista %u en %lu ms, fijar bloque %u\n", r0, r8, r41, millis() - t0, r16);
  Serial.printf("  OCR %02X %02X %02X %02X -> direcciones por %s\n", ocr[0], ocr[1], ocr[2], ocr[3], sdhc ? "sector (SDHC)" : "byte (tarjeta de hasta 2 GB)");

  uint8_t csd[16] = { 0 }, cid[16] = { 0 };
  if (bloque(9, 0, csd, 16)) {
    unsigned long megas;
    if ((csd[0] >> 6) == 1) megas = ((((unsigned long)(csd[7] & 0x3F) << 16) | ((unsigned long)csd[8] << 8) | csd[9]) + 1) / 2;
    else {
      unsigned long csize = ((unsigned long)(csd[6] & 0x03) << 10) | ((unsigned long)csd[7] << 2) | (csd[8] >> 6);
      int mult = ((csd[9] & 0x03) << 1) | (csd[10] >> 7), bl = csd[5] & 0x0F;
      megas = ((csize + 1) << (mult + 2 + bl)) >> 20;
    }
    Serial.printf("  CSD version %d, capacidad declarada %lu MB\n", (csd[0] >> 6) + 1, megas);
  }
  if (bloque(10, 0, cid, 16)) {
    Serial.printf("  fabricante %02X, sigla \"%c%c\", modelo \"%c%c%c%c%c\"\n", cid[0],
                  isprint(cid[1]) ? cid[1] : '?', isprint(cid[2]) ? cid[2] : '?',
                  isprint(cid[3]) ? cid[3] : '?', isprint(cid[4]) ? cid[4] : '?', isprint(cid[5]) ? cid[5] : '?',
                  isprint(cid[6]) ? cid[6] : '?', isprint(cid[7]) ? cid[7] : '?');
  }

  if (leer(0)) {
    Serial.println("SECTOR 0");
    hex("primeros bytes", 0, 16);
    Serial.printf("  firma final: %02X %02X\n", sec[510], sec[511]);
    uint32_t inicio = 0;
    for (int p = 0; p < 4; p++) {
      int e = 446 + 16 * p;
      if (sec[e + 4] == 0) continue;
      Serial.printf("  particion %d: tipo %02X, empieza en el sector %lu, mide %lu sectores (%lu MB)\n", p + 1, sec[e + 4],
                    (unsigned long)le32(e + 8), (unsigned long)le32(e + 12), (unsigned long)(le32(e + 12) / 2048));
      if (!inicio) inicio = le32(e + 8);
    }
    bool esCabecera = (sec[0] == 0xEB || sec[0] == 0xE9);
    if (esCabecera) cabecera(0);
    else if (inicio && leer(inicio)) cabecera(inicio);
    else Serial.println("  no hay particion que seguir");
  }
  SPI.endTransaction();
  SPI.end();
  Serial.println("======== fin ========");
}

void setup() {
  Serial.begin(115200);
  delay(500);
}

void loop() {
  sonda();
  delay(8000);
}
