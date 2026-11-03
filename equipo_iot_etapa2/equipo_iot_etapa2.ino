// equipo IoT rio San Juan de Rancas - etapa 2 y 3 (pagina en la red local y datos a la nube)
// mide temperatura (DS18B20), pH, turbidez y conductividad y lo muestra x USB y en la pantallita
//guarda todo con fecha y hora en la memoria del ESP32 (y en la microSD si hay)
// lo sube a ThingSpeak y a la nube x wifi, prende 2 LEDs - lo guardado se baja desde la pagina
// lo de la pantalla oled esta en pantalla.h (mascota animada + valores)
// ojo los 3 equipos llevan el MISMO programa, cada placa se da cuenta sola de cual es
// la pagina la da el mismo equipo:
//   - red propia del equipo (AP_NOMBRE): http://192.168.4.1
//   - WiFi de claves.h: http://riosanjuan.local o la IP que sale en el monitor serie
// reloj, microSD, GPS, botones y leds son opcionales, si no estan igual sigue andando
//monitor serie a 115200

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>
#include <RTClib.h>
#include <TinyGPSPlus.h>
#include <driver/gpio.h>
#include "claves.h"  // WIFI_NOMBRE, WIFI_CLAVE, API_KEY, AP_CLAVE (no se sube a ningun lado)
#include "claves_nube.h"  // NUBE_URL, NUBE_CLAVE_PUBLICA, NUBE_CLAVES: lo arma nube/preparar_nube.py (no se sube a ningun lado)
#include "pagina.h"  // la pagina web, sale de actualizar_pagina.py, no se toca a mano

// ---- pines ----
const int PIN_TEMP      = 4;
const int PIN_PH        = 34;
const int PIN_TURB      = 35;
const int PIN_LED_VERDE = 25;
const int PIN_LED_ROJO  = 26;
const int PIN_BOTON     = 27;  // la otra pata a tierra
const int PIN_BOTON_CAL = 15;  // segundo boton (calibrar), tambien a tierra
const int PIN_SD_CS     = 5;  // microSD -> SCK 18, MISO 19, MOSI 23
                                // reloj DS3231: SDA 21, SCL 22 (y el ADS1115 si esta)
const int PIN_GPS_RX    = 16;   // aca entra el TX del GPS
const int PIN_GPS_TX    = 17;  //aca entra el RX del gps
                                // conductividad -> senal (A) en D32 o D33, el que no use la pantalla
const int PIN_CE_ENERGIA = 13; // el + del sensor de conductividad: solo se prende para medir

// ---- calibracion: una fila por equipo ----
//cada equipo tiene sus sondas, osea cada uno sus numeros (sacados del registro de calibracion /
// manual de calibracion) - si se cambian hay que volver a subir el programa - /diagnostico dice cuales usa
// OJO -> lo q se calibra con el segundo boton se guarda en la memoria del equipo y MANDA sobre esta
// tabla - para volver a la tabla: /diagnostico?borrarcal=1
struct Calibracion {
  float phNeutro, mvNeutro; // pH de la solucion neutra (7.00 o 6.86) y los mV q dio el sensor
  float phAcido, mvAcido;   // lo mismo con la acida (4.00 o 4.01)
  float turbClaraV;   // V del sensor de turbidez en agua limpia (modulo en A,no en D)
  float ceFactor;   //conductividad -> patron / lo q marco el equipo (1.0 = sin calibrar)
};
const Calibracion CALIBRACIONES[3] = {
  { 7.00, 1598, 4.00, 2122, 2.014, 1.0 },  // equipo 1, ajolote: pH del 08/10 (el punto de pH 4 duro 1 min: repetir), turbidez del 09/10
  { 7.00, 1548, 4.00, 2071, 2.014, 1.0 }, // equipo 2 nutria: ph calibrado el 15/10 con los sobres de 7 y 4, turbidez y conductividad todavia copia del 1
  { 7.00, 1598, 4.00, 2122, 2.014, 1.0 },   // equipo 3, pinguino -> SIN CALIBRAR (copia del 1)
};
Calibracion cal = CALIBRACIONES[0];   // la de ESTA placa -> se elige al arrancar en reconocerPlaca()

// ---- calibrar con el segundo boton ----
// mantenerlo 3 s entra (o sale sin guardar) - pasos: 1 sonda en pH neutro, 2 sonda en pH acido,
// 3 turbidez en agua limpia - con la lectura quieta, el boton 1 la toma, el boton 2 salta el paso
Preferences ajustes;
bool calGuardada = false; // los numeros en uso vienen de la memoria del equipo, no de la tabla
volatile int calPaso = 0; // 0 = nada, 1 a 3 = el paso, 4 = mostrando el resultado
float calValor = 0;   // lo q se ve en pantalla: mV de pH (pasos 1 y 2) o V de turbidez (3)
bool calEstable = false;
int calResultado = 0;   //bits -> 1 pH guardado, 2 turbidez guardada, 4 pH rechazado (pendiente fuera de 150-190)
float calTomado[3]; bool calHay[3];
float calUltimas[5]; int calN = 0;
unsigned long calDesde = 0, calFin = 0;
const unsigned long CAL_LARGO_MS = 3000, CAL_SE_CIERRA_MS = 300000; //3 s para entrar, a los 5 min sin tocar se cierra
volatile unsigned long boton2Desde = 0;
volatile bool boton2Toque = false, boton2Largo = false;

// ---- limites pa el LED rojo y las alertas ----
// ECA agua, DS 004-2017-MINAM, pH: Categoria 3 (6.5 a 8.5)
//turbidez: 100 UNT es de la 1-A2, las categorias 3 y 4 no dicen nada,asi que es referencial
const float PH_MIN  = 6.5;
const float PH_MAX  = 8.5;
const float NTU_MAX = 100;

// ---- WiFi y ThingSpeak ----
const bool USAR_WIFI = true;
const bool USAR_THINGSPEAK = true;
const unsigned long ENVIAR_CADA_MS = 20000;

// ---- conductividad (Gravity TDS, SEN0244) ----
// su sonda le mueve la lectura al pH si comparten agua, osea solo se prende medio segundo
// cada CE_CADA_MS, mide +- de 120 a 2200 uS/cm
const bool HAY_SENSOR_CE = true;
const unsigned long CE_CADA_MS = 10000;

// ---- que equipo es esta placa ----
// se reconoce por el nro de fabrica (3 ultimos pares de la MAC, sale al subir, por usb al
//arrancar y en /diagnostico) - 1 = ajolote, 2 = nutria, 3 = pinguino - placa q no este: EQUIPO_SI_NO_ESTA
struct Placa { uint32_t serie; uint8_t equipo; };
const Placa PLACAS[] = { { 0x8B5418, 1 }, { 0xE6D7F8, 2 }, { 0x898FC8, 3 } };
const uint8_t EQUIPO_SI_NO_ESTA = 3;
int equipo = 1;
uint32_t serieDeLaPlaca = 0;

// ---- nombres para la pagina (se llenan al arrancar con el numero del equipo) ----
char EQUIPO_ID[10];  // equipo1
char EQUIPO_NOMBRE[12]; // equipo 1
char AP_NOMBRE[16]; // RioSanJuan-E1, la red wifi que crea el equipo
char NOMBRE_RED[14];  // riosanjuan, riosanjuan2, riosanjuan3 -> http://riosanjuan.local

// ---- los otros equipos ----
// cada uno grita x UDP cada 3 s "soy el N" - asi todos conocen la IP de los demas y la pagina
// (q la sirve cualquiera) puede mostrar los tres juntos
const uint16_t PUERTO_AVISOS = 4210;
WiFiUDP avisos;
IPAddress ipDeEquipo[3];
unsigned long vistoEquipo[3] = { 0, 0, 0 };
unsigned long ultimoAviso = 0;

OneWire oneWire(PIN_TEMP);
DallasTemperature sensorTemp(&oneWire);
bool tempSinEspera = false;   // true = se pide la conversion y se lee en la vuelta siguiente (no frena el loop)
RTC_DS3231 reloj;
WebServer servidor(80);

// ---- datos en la memoria interna ----
// al llegar a MAX_ARCHIVO el archivo pasa a ser "el anterior" y se empieza otro: asi siempre
// quedan las lecturas mas nuevas (entre 2 y 5 dias a una cada 20 s)
const char* ARCHIVO          = "/datos.csv";
const char* ARCHIVO_ANTERIOR = "/datos_anterior.csv";
const size_t MAX_ARCHIVO     = 650000;
const char* TITULOS = "fecha,hora,temperatura_C,pH,turbidez_NTU,turbidez_V,pH_mV,conductividad_uS,conductividad_mV,latitud,longitud,origen";

// ---- resumen por hora ----
//ademas de cada lectura se guarda una fila por hora con promedios (unos 45 KB al mes) - con eso la
// pagina arma el registro por dias, semanas y meses sin bajar todo
const char* RESUMEN          = "/resumen.csv";
const char* RESUMEN_ANTERIOR = "/resumen_anterior.csv";
const char* RESUMEN_PARCIAL  = "/hora_en_curso.bin";   // lo que va de la hora,por si se apaga
const size_t MAX_RESUMEN     = 150000;
const char* TITULOS_RESUMEN  = "fecha,hora,lecturas,temperatura_C,pH,pH_min,pH_max,turbidez_NTU,turbidez_max,conductividad_uS";

struct Suma { uint32_t n; float total, minimo, maximo; };
// hora: horas desde 1970 en hora de Peru, -1 = todavia ninguna
struct HoraEnCurso { int32_t hora; uint32_t lecturas; Suma temp, ph, ntu, ce; };
HoraEnCurso enCurso = { -1, 0, {}, {}, {}, {} };
unsigned long ultimoParcial = 0;

bool hayReloj = false;
bool haySD = false;
bool hayMemoria = false;
bool alerta = false;
bool wifiAntes = false;
unsigned long ultimoEnvio = 0;
unsigned long ultimaLectura = 0;
unsigned long ultimoIntentoWifi = 0;
volatile unsigned long botonDesde = 0; // cuando se apreto (0 = suelto o ya atendido)
volatile bool botonToque = false;  // toque corto
volatile bool botonLargo = false; // lo mantuvieron

float temp = 0, ph = 0, phMv = 0, ntu = 0, turbV = 0, turbPinMv = 0;
float ce = NAN, ceMv = 0;   // uS/cm llevados a 25 C, NAN = todavia no se midio
unsigned long ultimaCE = 0;

// ultimas lecturas en RAM, pa que la pagina abra con el grafico ya lleno
const int MAX_HIST = 60;
struct Lectura { unsigned long ms; float temp, ph, ntu; };
Lectura hist[MAX_HIST];
int histCant = 0, histSig = 0;
unsigned long numLectura = 0;

TinyGPSPlus gps;

// pH fuera de 0-14 o turbidez sin tension = sensor desconectado, no una medicion
// la pantalla y la pagina ponen "sin dato" en vez de una alarma falsa
bool phValido() { return ph >= 0 && ph <= 14; }
bool turbValida() { return turbV > 0.3f; }

// cuanta memoria interna esta usada, usedBytes() recorre TODO el sistema de archivos y tarda,
// osea solo se mide despues de escribir (medirMemoria) y los demas (pantalla, /datos) leen lo guardado
volatile int memoriaPct = -1; //-1 = no hay memoria interna
volatile unsigned memoriaKB = 0;
void medirMemoria() {
  if (!hayMemoria) return;
  size_t total = LittleFS.totalBytes(), usado = LittleFS.usedBytes();
  memoriaKB = usado / 1024;
  memoriaPct = total ? (int)((uint64_t)usado * 100 / total) : 0;
}

bool hayInternet();
bool gpsConUbicacion();
uint8_t adsDireccion = 0; // ADS1115 o ADS1015 -> 0 = no hay (ver buscarADS)
#include "pantalla.h"  // mascota, valores y estado del equipo en la oled

const char* const MASCOTAS[] = { "ajolote", "nutria", "pinguino" };

// mira que placa es y de ahi saca equipo, mascota, calibracion y nombres
void reconocerPlaca() {
  uint64_t mac = ESP.getEfuseMac();  //llega al reves -> el primer par de la MAC es el byte mas bajo
  serieDeLaPlaca = (uint32_t)((mac >> 24) & 0xFF) << 16 | (uint32_t)((mac >> 32) & 0xFF) << 8 | (uint32_t)((mac >> 40) & 0xFF);
  equipo = EQUIPO_SI_NO_ESTA;
  for (const Placa& p : PLACAS) if (p.serie == serieDeLaPlaca) equipo = p.equipo;
  mascota = equipo - 1;
  cal = CALIBRACIONES[equipo - 1];
  ajustes.begin("cal", false);   //si ya se calibro con el boton, eso gana a la tabla
  if (ajustes.isKey("claraV")) {
    cal.mvNeutro = ajustes.getFloat("mvNeutro", cal.mvNeutro);
    cal.mvAcido = ajustes.getFloat("mvAcido", cal.mvAcido);
    cal.turbClaraV = ajustes.getFloat("claraV", cal.turbClaraV);
    calGuardada = true;
  }
  ajustes.end();
  snprintf(EQUIPO_ID, sizeof(EQUIPO_ID), "equipo%d", equipo);
  snprintf(EQUIPO_NOMBRE, sizeof(EQUIPO_NOMBRE), "Equipo %d", equipo);
  snprintf(AP_NOMBRE, sizeof(AP_NOMBRE), "RioSanJuan-E%d", equipo);
  if (equipo == 1) strcpy(NOMBRE_RED, "riosanjuan");
  else snprintf(NOMBRE_RED, sizeof(NOMBRE_RED), "riosanjuan%d", equipo);
}

// avisa a los demas "aca estoy" y anota a los que se oyen
void avisarYEscuchar() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - ultimoAviso >= 3000) {
    ultimoAviso = millis();
    avisos.beginPacket(WiFi.broadcastIP(), PUERTO_AVISOS);
    avisos.printf("RSJ%d", equipo);
    avisos.endPacket();
  }
  while (avisos.parsePacket() > 0) {
    char b[8] = "";
    avisos.read(b, sizeof(b) - 1);
    int otro = b[3] - '0';
    if (!strncmp(b, "RSJ", 3) && otro >= 1 && otro <= 3 && otro != equipo) {
      ipDeEquipo[otro - 1] = avisos.remoteIP();
      vistoEquipo[otro - 1] = millis();
    }
  }
}

// IP del equipo n (1 a 3) en el wifi, vacia si hace mas de 20 s q no se oye
String direccionDe(int n) {
  if (WiFi.status() != WL_CONNECTED) return "";
  if (n == equipo) return WiFi.localIP().toString();
  if (vistoEquipo[n - 1] == 0 || millis() - vistoEquipo[n - 1] > 20000) return "";
  return ipDeEquipo[n - 1].toString();
}

// la senal de conductividad usa el pin (32 o 33) q la pantalla deja libre
int pinCE() {
  if (oledSda != 32 && oledScl != 32) return 32;
  if (oledSda != 33 && oledScl != 33) return 33;
  return -1;
}

void leerGPS() {
  while (Serial2.available()) gps.encode(Serial2.read());
}

bool gpsConUbicacion() {
  return gps.location.isValid() && gps.location.age() < 10000;
}

const char* estadoGPS() {
  if (gps.charsProcessed() < 10) return "no";
  return gpsConUbicacion() ? "si" : "buscando";
}

// botones: un toque prende la pantalla / pasa de vista,mantenido marca una muestra
// en la interrupcion solo se anota cuanto duro apretado, lo demas se hace en loop()
// los dos botones hacen lo mismo, osea comparten esta funcion (always_inline para q quede en IRAM
// junto con la interrupcion, si no se queda en flash y revienta)
const unsigned long BOTON_LARGO_MS = 1500;
static inline __attribute__((always_inline)) void moverBoton(int pin, volatile unsigned long& desde, volatile bool& toque,
                                                             volatile bool& largo, unsigned long largoMs) {
  unsigned long ahora = millis();
  if (gpio_get_level((gpio_num_t)pin) == 0) {   // se apreto
    if (!desde) desde = ahora | 1;   // el |1 es para que nunca quede en 0 x casualidad
  } else if (desde) {  //se solto
    unsigned long duro = ahora - desde;
    if (duro >= largoMs) largo = true;
    else if (duro > 30) toque = true;   // menos de eso es rebote del contacto
    desde = 0;
  }
}
void IRAM_ATTR alMoverBoton()  { moverBoton(PIN_BOTON, botonDesde, botonToque, botonLargo, BOTON_LARGO_MS); }
void IRAM_ATTR alMoverBoton2() { moverBoton(PIN_BOTON_CAL, boton2Desde, boton2Toque, boton2Largo, CAL_LARGO_MS); }

// promedio de 20 lecturas del pin,en mV (200 ms bloqueado, no es gratis)
float leerMilivoltios(int pin) {
  long suma = 0;
  for (int i = 0; i < 20; i++) { suma += analogReadMilliVolts(pin); delay(10); }
  return suma / 20.0;
}

// ---- conversor ADS1115 o ADS1015 (opcional) ----
//va en los mismos cables del reloj (SDA 21, SCL 22) y se busca solo al arrancar, si contesta, pH,
// turbidez y conductividad se leen por A0, A1 y A2 (menos ruido que los pines del ESP32) - si no, por los pines
const uint8_t ADS_PH = 0, ADS_TURB = 1, ADS_CE = 2;
uint8_t adsBitsBajos = 0; // el ADS1015 (12 bits) deja los 4 bits bajos siempre en cero, el ADS1115 no

void buscarADS() {
  adsDireccion = 0;
  for (uint8_t d = 0x48; d <= 0x4B && !adsDireccion; d++) {
    Wire.beginTransmission(d);
    if (Wire.endTransmission() == 0) adsDireccion = d;
  }
}

// una medicion de la entrada (0 a 3) contra tierra,en mV, escala de 4.096 V, NAN si no contesta
float adsUnaLectura(uint8_t entrada) {
  Wire.beginTransmission(adsDireccion);
  Wire.write(1);  // registro de configuracion
  Wire.write(0xC3 | (entrada << 4));   //medir ahorita esa entrada, una vez,escala 4.096 V
  Wire.write(0x83);   // velocidad normal, sin comparador
  if (Wire.endTransmission() != 0) return NAN;
  delay(10);   // tarda menos de 9 ms
  Wire.beginTransmission(adsDireccion);
  Wire.write(0);   //registro del resultado
  if (Wire.endTransmission() != 0 || Wire.requestFrom(adsDireccion, (uint8_t)2) != 2) return NAN;
  uint8_t alto = Wire.read(), bajo = Wire.read();
  adsBitsBajos |= bajo & 0x0F;
  return (int16_t)((alto << 8) | bajo) * 0.125f;
}

float leerMilivoltiosADS(uint8_t entrada) {
  float suma = 0;
  for (int i = 0; i < 10; i++) {
    float mv = adsUnaLectura(entrada);
    if (isnan(mv)) return NAN;
    suma += mv;
  }
  return suma / 10;
}

//por el conversor si esta, si no (o si dejo de contestar), por el pin del ESP32
float leerSenal(uint8_t entrada, int pin) {
  if (adsDireccion) {
    float mv = leerMilivoltiosADS(entrada);
    if (!isnan(mv)) return mv;
  }
  return pin >= 0 ? leerMilivoltios(pin) : 0;
}

// con internet el wifi pone la hora solo (hora de Peru)
bool hayHoraInternet() { return time(nullptr) > 1700000000; }

String fechaHora() {
  char b[24];
  if (hayHoraInternet()) {
    time_t ahora = time(nullptr);
    struct tm t;
    localtime_r(&ahora, &t);
    strftime(b, sizeof(b), "%Y-%m-%d,%H:%M:%S", &t);
    return String(b);
  }
  if (!hayReloj) return "sin_reloj," + String(millis() / 1000);
  DateTime t = reloj.now();
  snprintf(b, sizeof(b), "%04d-%02d-%02d,%02d:%02d:%02d",
           t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second());
  return String(b);
}

void parpadear(int pin, int veces) {
  for (int i = 0; i < veces; i++) {
    digitalWrite(pin, HIGH); delay(120);
    digitalWrite(pin, LOW);  delay(120);
  }
}

// los LEDs hablan del EQUIPO, no del agua (el agua mala se ve en la pantalla y la pagina)
// verde solo: miden los tres - rojo solo: no mide ninguno - los dos -> falta alguno
// la conductividad no cuenta: con la sonda al aire marca cero igual que desconectada
void ponerLeds() {
  int miden = (temp > -100) + phValido() + turbValida();
  digitalWrite(PIN_LED_VERDE, miden > 0 ? HIGH : LOW);
  digitalWrite(PIN_LED_ROJO, miden < 3 ? HIGH : LOW);
}

void leerSensores() {
  //el DS18B20 tarda ~750 ms en convertir, si no esta con alimentacion parasita,no se espera:
  //se lee la conversion pedida en la vuelta anterior y se pide la siguiente (lag de 2 s,nada grave)
  if (!tempSinEspera) sensorTemp.requestTemperatures();
  temp = sensorTemp.getTempCByIndex(0);
  if (tempSinEspera) sensorTemp.requestTemperatures();

  phMv = leerSenal(ADS_PH, PIN_PH);
  ph = cal.phNeutro + (cal.mvNeutro - phMv) * (cal.phNeutro - cal.phAcido) / (cal.mvAcido - cal.mvNeutro);

  turbPinMv = leerSenal(ADS_TURB, PIN_TURB);
  turbV = turbPinMv * 1.5 / 1000.0; // deshace el divisor de 10k y 20k
  float v = turbV * 4.2 / cal.turbClaraV;
  ntu = -1120.4 * v * v + 5742.3 * v - 4352.9;
  if (v < 2.5) ntu = 3000;
  if (ntu < 0) ntu = 0;

  // alerta con un poquito de margen para que no cambie a cada rato justo en el limite
  //solo cuenta lo q se midio de verdad: sensor sin conectar no es agua mala
  bool phMal = phValido() && (alerta ? !(ph > PH_MIN + 0.05 && ph < PH_MAX - 0.05) : (ph < PH_MIN - 0.05 || ph > PH_MAX + 0.05));
  bool turbMal = turbValida() && (alerta ? !(ntu < NTU_MAX * 0.9) : (ntu > NTU_MAX * 1.1));
  alerta = phMal || turbMal;
  ponerLeds();
}

// ---- calibracion con el segundo boton ----
void calPasar(int paso) { calPaso = paso; calN = 0; calEstable = false; calDesde = millis(); }

// al final: lo que vale se acepta y se guarda en la memoria del equipo
void calTerminar() {
  calResultado = 0;
  if (calHay[0] && calHay[1]) {
    float pendiente = (calTomado[1] - calTomado[0]) / (cal.phNeutro - cal.phAcido);
    if (pendiente >= 150 && pendiente <= 190) { cal.mvNeutro = calTomado[0]; cal.mvAcido = calTomado[1]; calResultado |= 1; }
    else calResultado |= 4;
  }
  if (calHay[2] && calTomado[2] > 0.3f) { cal.turbClaraV = calTomado[2]; calResultado |= 2; }
  if (calResultado & 3) {
    ajustes.begin("cal", false);
    ajustes.putFloat("mvNeutro", cal.mvNeutro); ajustes.putFloat("mvAcido", cal.mvAcido); ajustes.putFloat("claraV", cal.turbClaraV);
    ajustes.end();
    calGuardada = true;
  }
  Serial.printf("Calibracion: pH %s, turbidez %s\n", calResultado & 1 ? "guardado" : calResultado & 4 ? "rechazado" : "sin cambio",
                calResultado & 2 ? "guardada" : "sin cambio");
  calFin = millis() + 6000;
  calPaso = 4;
}

// en cada lectura nueva: lo que se muestra y si ya esta quieta (5 seguidas dentro de 6 mV)
void calAnotar() {
  if (calPaso < 1 || calPaso > 3) return;
  calValor = calPaso == 3 ? turbV : phMv;
  calUltimas[calN % 5] = calPaso == 3 ? turbPinMv : phMv;
  calN++;
  float menor = calUltimas[0], mayor = calUltimas[0];
  for (int i = 1; i < 5; i++) { menor = min(menor, calUltimas[i]); mayor = max(mayor, calUltimas[i]); }
  calEstable = calN >= 5 && mayor - menor <= 6;
}

// los dos botones mientras se calibra, devuelve true si el boton 1 ya se atendio aca
bool calBotones() {
  unsigned long d2 = boton2Desde;
  if (d2 && millis() - d2 >= CAL_LARGO_MS && digitalRead(PIN_BOTON_CAL) == LOW) { boton2Desde = 0; boton2Largo = true; }
  if (boton2Largo) { //mantenido: entra,o sale sin guardar
    boton2Largo = false; boton2Toque = false;
    if (calPaso) calPaso = 0;
    else { calHay[0] = calHay[1] = calHay[2] = false; calPasar(1); Serial.println("Calibracion: paso 1, sonda en pH neutro"); }
    return calPaso != 0;
  }
  if (!calPaso) { boton2Toque = false; return false; }
  if (calPaso == 4) { if (millis() > calFin) calPaso = 0; botonToque = boton2Toque = false; return true; }
  if (millis() - calDesde > CAL_SE_CIERRA_MS) { calPaso = 0; return false; }   //se olvidaron: cierra sin guardar
  bool sigue = false;
  if (botonToque) {  // boton 1: tomar la lectura (solo si esta quieta)
    botonToque = false;
    if (calEstable) {
      float suma = 0; for (float v : calUltimas) suma += v;
      calTomado[calPaso - 1] = calPaso == 3 ? suma / 5 * 1.5 / 1000.0 : suma / 5;
      calHay[calPaso - 1] = true; sigue = true;
    }
  }
  if (boton2Toque) { boton2Toque = false; sigue = true; }  // boton 2: salta el paso
  if (sigue) { if (calPaso == 3) calTerminar(); else calPasar(calPaso + 1); }
  botonDesde = 0; botonLargo = false;  // calibrando, mantener el boton 1 no marca muestras
  return true;
}

// de mV del sensor a uS/cm, formula del fabricante (uS/cm a 25 C), por debajo de 160 mV el ESP32
// no distingue nada (con el ADS el piso baja a 20 mV)
float conductividadDe(float mv) {
  if (mv < (adsDireccion ? 20 : 160)) return 0;
  float v = mv / 1000.0;
  if (temp > -100) v /= 1.0 + 0.02 * (temp - 25.0);   //sin sensor de temperatura no se corrige
  return (133.42 * v * v * v - 255.86 * v * v + 857.39 * v) * cal.ceFactor;
}

// prende el sensor,espera q la senal se asiente,mide y lo apaga
void leerConductividad() {
  int pin = pinCE();
  if (pin < 0 && !adsDireccion) { ce = NAN; return; }
  digitalWrite(PIN_CE_ENERGIA, HIGH);
  delay(400);
  ceMv = leerSenal(ADS_CE, pin);
  digitalWrite(PIN_CE_ENERGIA, LOW);
  ce = conductividadDe(ceMv);
}

void sumar(Suma& s, float v) {
  if (isnan(v)) return;
  if (!s.n || v < s.minimo) s.minimo = v;
  if (!s.n || v > s.maximo) s.maximo = v;
  s.total += v;
  s.n++;
}

// una celda del resumen: promedio ('p'), minimo ('m') o maximo ('M'), vacia si no hubo mediciones
int celda(char* b, size_t n, const Suma& s, char cual, int decimales) {
  if (!s.n) return snprintf(b, n, ",");
  return snprintf(b, n, ",%.*f", decimales, cual == 'm' ? s.minimo : cual == 'M' ? s.maximo : s.total / s.n);
}

// la fila de una hora (fecha, hora, cuantas lecturas y los promedios) escrita en b - devuelve el largo
int filaResumen(char* b, size_t cap, const HoraEnCurso& h) {
  time_t t = (time_t)h.hora * 3600;
  struct tm f;
  gmtime_r(&t, &f);
  int n = snprintf(b, cap, "%04d-%02d-%02d,%02d:00,%lu", f.tm_year + 1900, f.tm_mon + 1, f.tm_mday, f.tm_hour, (unsigned long)h.lecturas);
  n += celda(b + n, cap - n, h.temp, 'p', 1);
  n += celda(b + n, cap - n, h.ph, 'p', 2);
  n += celda(b + n, cap - n, h.ph, 'm', 2);
  n += celda(b + n, cap - n, h.ph, 'M', 2);
  n += celda(b + n, cap - n, h.ntu, 'p', 0);
  n += celda(b + n, cap - n, h.ntu, 'M', 0);
  n += celda(b + n, cap - n, h.ce, 'p', 0);
  return n;
}

// agrega una fila al archivo, si ya paso del tope, lo manda a "anterior" y se empieza otro
// false si no se pudo abrir, aprovecha y vuelve a medir la memoria (ya se escribio, total cambio)
bool agregarFila(const char* archivo, const char* anterior, size_t tope, const char* titulos, const char* fila) {
  File f = LittleFS.open(archivo, FILE_APPEND);
  if (!f) return false;
  size_t tam = f.size();
  if (tam == 0) tam += f.println(titulos);
  tam += f.println(fila);
  f.close();
  if (tam > tope) {
    LittleFS.remove(anterior);
    LittleFS.rename(archivo, anterior);
  }
  medirMemoria();
  return true;
}

void guardarResumen(const HoraEnCurso& h) {
  if (!hayMemoria || h.hora < 0 || !h.lecturas) return;
  char fila[150];
  filaResumen(fila, sizeof(fila), h);
  agregarFila(RESUMEN, RESUMEN_ANTERIOR, MAX_RESUMEN, TITULOS_RESUMEN, fila);
}

//hora de ahorita en hora de Peru, en horas desde 1970, -1 si el equipo no sabe q hora es
int32_t horaActual() {
  if (hayHoraInternet()) return (time(nullptr) - 5 * 3600) / 3600;
  if (hayReloj) return reloj.now().unixtime() / 3600;
  return -1;
}

// suma la lectura a la hora en curso, al cambiar de hora guarda la fila de la q acabo
void resumirLectura() {
  int32_t hora = horaActual();
  if (hora < 0) return;
  if (hora != enCurso.hora) {
    guardarResumen(enCurso);
    enCurso = { hora, 0, {}, {}, {}, {} };
  }
  enCurso.lecturas++;
  sumar(enCurso.temp, temp > -100 ? temp : NAN);
  sumar(enCurso.ph, phValido() ? ph : NAN);
  sumar(enCurso.ntu, turbValida() ? ntu : NAN);
  sumar(enCurso.ce, ce);
  if (hayMemoria && millis() - ultimoParcial >= 300000) {   //cada 5 min apunta lo q va de la hora
    ultimoParcial = millis();
    File f = LittleFS.open(RESUMEN_PARCIAL, FILE_WRITE);
    if (f) { f.write((const uint8_t*)&enCurso, sizeof(enCurso)); f.close(); }
  }
}

//al arrancar -> recupera lo q llevaba la hora cuando se apago
void recuperarHora() {
  if (!hayMemoria || !LittleFS.exists(RESUMEN_PARCIAL)) return;
  File f = LittleFS.open(RESUMEN_PARCIAL, FILE_READ);
  if (!f) return;
  if (f.size() == sizeof(enCurso)) f.read((uint8_t*)&enCurso, sizeof(enCurso));
  f.close();
}

void anotarLectura() {
  resumirLectura();
  hist[histSig] = { millis(), temp, phValido() ? ph : NAN, turbValida() ? ntu : NAN };
  histSig = (histSig + 1) % MAX_HIST;
  if (histCant < MAX_HIST) histCant++;
  numLectura++;
}

// crea el archivo con sus titulos si la tarjeta es nueva
void prepararSD() {
  if (SD.exists(ARCHIVO)) return;
  File f = SD.open(ARCHIVO, FILE_WRITE);
  if (f) { f.println(TITULOS); f.close(); }
}

// con internet la lectura se sube PRIMERO a la red y no se repite en la memoria interna,sin
// internet se guarda aqui - en false guarda siempre las dos, el resumen por hora se guarda siempre
const bool GUARDAR_SOLO_SIN_INTERNET = true;

// guarda en la memoria interna y, si hay, en la microSD
// "subida" = esta lectura ya llego a la red (ThingSpeak la acepto)
void guardarLectura(const char* origen, bool subida = false) {
  char fila[200];
  int n = snprintf(fila, sizeof(fila), "%s,%.1f,%.2f,%.0f,%.2f,%.0f,", fechaHora().c_str(), temp, ph, ntu, turbV, phMv);
  if (isnan(ce)) n += snprintf(fila + n, sizeof(fila) - n, ",,");
  else n += snprintf(fila + n, sizeof(fila) - n, "%.0f,%.0f,", ce, ceMv);
  if (gpsConUbicacion()) n += snprintf(fila + n, sizeof(fila) - n, "%.6f,%.6f,", gps.location.lat(), gps.location.lng());
  else n += snprintf(fila + n, sizeof(fila) - n, ",,");
  snprintf(fila + n, sizeof(fila) - n, "%s", origen);

  if (hayMemoria && subida && GUARDAR_SOLO_SIN_INTERNET) {
    Serial.println("Subida a la red: no se repite en la memoria interna");
  } else if (hayMemoria) {
    bool ok = agregarFila(ARCHIVO, ARCHIVO_ANTERIOR, MAX_ARCHIVO, TITULOS, fila);
    Serial.println(ok ? "Memoria interna: guardado" : "Memoria interna: no se pudo escribir");
  }

  if (haySD) {
    File f = SD.open(ARCHIVO, FILE_APPEND);
    if (f) { f.println(fila); f.close(); Serial.println("microSD: guardado"); }
    else Serial.println("microSD: no se pudo escribir");
  }
}

// ---- nube (Supabase) ----
// cada NUBE_CADA_MS la ultima lectura sube a una base de datos en internet: de ahi la lee la pagina publica,
//que cualquiera puede abrir sin estar en el WiFi del equipo - mientras NUBE_URL este vacia no se manda nada
// el envio va en una tarea aparte: la conexion segura (https) tarda uno o dos segundos y asi no frena las mediciones
const unsigned long NUBE_CADA_MS = 20000;
const size_t NUBE_LARGO = 360;
volatile int codigoNube = 0; // respuesta del ultimo envio: 200 o 204 = guardado
volatile unsigned long ultimoNube = 0;   // cuando se guardo por ultima vez (millis)
unsigned long ultimoArmado = 0;
QueueHandle_t colaNube = nullptr; //un solo lugar -> si la tarea no alcanzo a mandar la anterior, la nueva la reemplaza

void textoTemp(float t, char* b, size_t n);
void textoNumero(float v, int decimales, char* b, size_t n);

bool nubeActiva() { return NUBE_URL[0] && NUBE_CLAVE_PUBLICA[0]; }
bool nubeAlDia() { return ultimoNube && millis() - ultimoNube < 3 * NUBE_CADA_MS; }

//arma la lectura de ahora pa la nube (lo q no se pudo medir va como null)
void prepararNube() {
  if (!colaNube || millis() - ultimoArmado < NUBE_CADA_MS) return;
  ultimoArmado = millis();
  char t[12], a[12], u[12], c[12], la[16] = "null", lo[16] = "null";
  textoTemp(temp, t, sizeof(t));
  textoNumero(phValido() ? ph : NAN, 2, a, sizeof(a));
  textoNumero(turbValida() ? ntu : NAN, 0, u, sizeof(u));
  textoNumero(ce, 0, c, sizeof(c));
  if (gpsConUbicacion()) {
    snprintf(la, sizeof(la), "%.6f", gps.location.lat());
    snprintf(lo, sizeof(lo), "%.6f", gps.location.lng());
  }
  char cuerpo[NUBE_LARGO];
  snprintf(cuerpo, sizeof(cuerpo),
           "{\"p_equipo\":%d,\"p_clave\":\"%s\",\"p_temp\":%s,\"p_ph\":%s,\"p_ntu\":%s,\"p_ce\":%s,\"p_lat\":%s,\"p_lng\":%s,\"p_gps\":\"%s\",\"p_encendido\":%lu}",
           equipo, NUBE_CLAVES[equipo - 1], t, a, u, c, la, lo, estadoGPS(), millis() / 1000);
  xQueueOverwrite(colaNube, cuerpo);
}

void enviarNube(const char* cuerpo) {
  WiFiClientSecure cliente;
  cliente.setInsecure(); //no se revisa el certificado del servidor: mas simple, suficiente pa el prototipo
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(8000);
  if (!http.begin(cliente, String(NUBE_URL) + "/rest/v1/rpc/registrar_lectura")) { codigoNube = -1; return; }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", NUBE_CLAVE_PUBLICA);
  // la clave vieja ("anon", empieza con eyJ) tmb va como Authorization, la nueva ("publishable") no
  if (strncmp(NUBE_CLAVE_PUBLICA, "eyJ", 3) == 0) http.addHeader("Authorization", String("Bearer ") + NUBE_CLAVE_PUBLICA);
  codigoNube = http.POST((uint8_t*)cuerpo, strlen(cuerpo));
  if (codigoNube == 200 || codigoNube == 204) ultimoNube = millis();
  else Serial.printf("Nube: no se guardo (%d) %s\n", codigoNube, http.getString().c_str());
  http.end();
}

void tareaNube(void*) {
  static char cuerpo[NUBE_LARGO];
  for (;;) {
    if (xQueueReceive(colaNube, cuerpo, portMAX_DELAY) == pdTRUE && WiFi.status() == WL_CONNECTED) enviarNube(cuerpo);
  }
}

// hay internet si el ultimo envio salio bien (o, antes del primero, si ya llego la hora por internet)
int codigoThingSpeak = 0;
bool hayInternet() {
  if (WiFi.status() != WL_CONNECTED) return false;
  return nubeAlDia() || codigoThingSpeak == 200 || (codigoThingSpeak == 0 && hayHoraInternet());
}

//solo manda el equipo 1: el canal de ThingSpeak es uno y los datos de los tres se mezclarian
//true si ThingSpeak lo guardo (responde con el numero de registro, "0" = lo rechazo)
bool enviarThingSpeak() {
  if (!USAR_THINGSPEAK || !USAR_WIFI || equipo != 1 || WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setConnectTimeout(3000); // sin internet no deja la pagina colgada mucho rato
  http.setTimeout(3000);
  char url[300];
  int n = snprintf(url, sizeof(url),
           "http://api.thingspeak.com/update?api_key=%s&field1=%.1f&field2=%.2f&field3=%.0f",
           API_KEY, temp, ph, ntu);
  if (gpsConUbicacion()) {
    snprintf(url + n, sizeof(url) - n, "&lat=%.6f&long=%.6f", gps.location.lat(), gps.location.lng());
  }
  http.begin(url);
  codigoThingSpeak = http.GET();
  bool guardado = codigoThingSpeak == 200 && http.getString().toInt() > 0;
  Serial.printf("Envio a ThingSpeak: %d%s\n", codigoThingSpeak, guardado ? "" : " (no guardado)");
  http.end();
  ultimoEnvio = millis();
  return guardado;
}

// ---- pagina web ----

//temperatura como texto, "null" si el sensor no responde (-127)
void textoTemp(float t, char* b, size_t n) {
  if (t > -100) snprintf(b, n, "%.1f", t);
  else snprintf(b, n, "null");
}

// un numero para la pagina, "null" si no hay medicion
void textoNumero(float v, int decimales, char* b, size_t n) {
  if (isnan(v)) snprintf(b, n, "null");
  else snprintf(b, n, "%.*f", decimales, v);
}

//la pagina viaja en gzip: ocupa la tercera parte y carga mas rapido
void paginaInicio() {
  servidor.sendHeader("Content-Encoding", "gzip");
  servidor.send_P(200, "text/html; charset=utf-8", (const char*)PAGINA_GZ, PAGINA_GZ_LARGO);
}

void paginaDatos() {
  char t[12], lat[16] = "null", lng[16] = "null";
  textoTemp(temp, t, sizeof(t));
  if (gpsConUbicacion()) {
    snprintf(lat, sizeof(lat), "%.6f", gps.location.lat());
    snprintf(lng, sizeof(lng), "%.6f", gps.location.lng());
  }
  bool wifi = WiFi.status() == WL_CONNECTED;
  char conductividad[12], acidez[12], turbidez[12];
  textoNumero(ce, 0, conductividad, sizeof(conductividad));
  textoNumero(phValido() ? ph : NAN, 2, acidez, sizeof(acidez));
  textoNumero(turbValida() ? ntu : NAN, 0, turbidez, sizeof(turbidez));
  char b[640];
  // "nodo" = nro de este equipo, "ips" = IP de los tres en el WiFi (vacia la del que no se oye)
  snprintf(b, sizeof(b),
           "{\"equipo\":\"%s\",\"nombre\":\"%s\",\"n\":%lu,\"temp\":%s,\"ph\":%s,\"ntu\":%s,\"ce\":%s,"
           "\"wifi\":%s,\"internet\":%s,\"encendido\":%lu,\"respaldo\":%s,\"kb\":%u,\"sd\":%s,\"reloj\":%s,\"gps\":\"%s\",\"lat\":%s,\"lng\":%s,\"ip\":\"%s\","
           "\"nodo\":%d,\"ips\":[\"%s\",\"%s\",\"%s\"],\"nube\":%s}",
           EQUIPO_ID, EQUIPO_NOMBRE, numLectura, t, acidez, turbidez, conductividad,
           wifi ? "true" : "false", hayInternet() ? "true" : "false", millis() / 1000, hayMemoria ? "true" : "false", hayMemoria ? memoriaKB : 0u,
           haySD ? "true" : "false", (hayReloj || hayHoraInternet()) ? "true" : "false",
           estadoGPS(), lat, lng, wifi ? WiFi.localIP().toString().c_str() : "",
           equipo, direccionDe(1).c_str(), direccionDe(2).c_str(), direccionDe(3).c_str(),
           nubeActiva() ? (nubeAlDia() ? "true" : "false") : "null");
  servidor.sendHeader("Access-Control-Allow-Origin", "*");
  servidor.send(200, "application/json", b);
}

// cada lectura: [hace cuantos segundos,temperatura, pH, turbidez], de la mas vieja a la mas nueva
// lo que no se pudo medir va null
void paginaHistorial() {
  String s;
  s.reserve(40 + histCant * 32);
  s = "{\"n\":" + String(numLectura) + ",\"lecturas\":[";
  unsigned long ahora = millis();
  for (int i = 0; i < histCant; i++) {
    const Lectura& l = hist[(histSig - histCant + i + MAX_HIST) % MAX_HIST];
    char t[12], acidez[12], turbidez[12], b[56];
    textoTemp(l.temp, t, sizeof(t));
    textoNumero(l.ph, 2, acidez, sizeof(acidez));
    textoNumero(l.ntu, 0, turbidez, sizeof(turbidez));
    snprintf(b, sizeof(b), "%s[%lu,%s,%s,%s]", i ? "," : "", (ahora - l.ms) / 1000, t, acidez, turbidez);
    s += b;
  }
  s += "]}";
  servidor.sendHeader("Access-Control-Allow-Origin", "*");
  servidor.send(200, "application/json", s);
}

// ---- diagnostico: http://192.168.4.1/diagnostico ----
// prueba cada modulo, dice que encuentra y vuelve a buscar el reloj y la microSD

// deja el bus SPI listo pa hablarle a la tarjeta "a mano" (a 400 kHz, con los 80 pulsos de arranque)
void iniciarBusTarjeta() {
  SD.end();
  SPI.begin();
  gpio_pullup_en((gpio_num_t)19); // sin nada en MISO la respuesta saldria 0 en vez de 255
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 10; i++) SPI.transfer(0xFF);
}

// le pregunta a la tarjeta si esta ahi (comando 0)
// 1 = contesta, 255 = nadie contesta, 0 = MISO pegado a 0 V
uint8_t probarTarjeta() {
  iniciarBusTarjeta();
  digitalWrite(PIN_SD_CS, LOW);
  const uint8_t comando0[] = { 0x40, 0, 0, 0, 0, 0x95 };
  for (uint8_t c : comando0) SPI.transfer(c);
  uint8_t r = 0xFF;
  for (int i = 0; i < 10 && r == 0xFF; i++) r = SPI.transfer(0xFF);
  digitalWrite(PIN_SD_CS, HIGH);
  SPI.transfer(0xFF);
  SPI.endTransaction();
  return r;
}

// manda un comando y devuelve el primer byte de la respuesta
//le calcula el CRC7: la libreria SD deja la tarjeta exigiendolo y un comando con CRC falso
// se rechaza con un 8
uint8_t comandoTarjeta(uint8_t c, uint32_t arg) {
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

void soltarTarjeta() {
  digitalWrite(PIN_SD_CS, HIGH);
  SPI.transfer(0xFF);
}

// lee un sector - devuelve 0 = no llegaron datos, 1 = llegaron con errores, 2 = bien
// "orden" = respuesta a la orden de leer, "marca" = lo que mando la tarjeta despues (FE = datos)
uint8_t leerSectorTarjeta(uint32_t numero, bool porSector, uint8_t* bufer, uint8_t* orden, uint8_t* marca) {
  uint8_t resultado = 0;
  *marca = 0xFF;
  digitalWrite(PIN_SD_CS, LOW);
  *orden = comandoTarjeta(17, porSector ? numero : numero << 9);
  if (*orden == 0) {
    unsigned long t = millis();
    while ((*marca = SPI.transfer(0xFF)) == 0xFF && millis() - t < 500) {}
    if (*marca == 0xFE) {
      for (int i = 0; i < 512; i++) bufer[i] = SPI.transfer(0xFF);
      uint16_t recibido = SPI.transfer(0xFF);
      recibido = (recibido << 8) | SPI.transfer(0xFF);
      uint16_t calculado = 0;  // CRC16 de los 512 bytes -> pilla bits cambiados en el camino
      for (int i = 0; i < 512; i++) {
        calculado ^= (uint16_t)bufer[i] << 8;
        for (int k = 0; k < 8; k++) calculado = (calculado & 0x8000) ? (calculado << 1) ^ 0x1021 : calculado << 1;
      }
      resultado = (calculado == recibido) ? 2 : 1;
    }
  }
  soltarTarjeta();
  return resultado;
}

// mira la cabecera del sistema de archivos y dice si el equipo la puede abrir
//la libreria decide FAT16/FAT32 x la cantidad de grupos (clusters), como manda la norma:
// un "FAT32" con menos de 65 525 grupos (asi formatea un celular las de 2 GB) lo rechaza
String formatoTarjeta(const uint8_t* c) {
  char b[230];
  if (c[510] != 0x55 || c[511] != 0xAA) return "SIN FORMATO (cabecera vacia o danada): formatear en la laptop en FAT32";
  if (!memcmp(c + 3, "EXFAT", 5)) return "exFAT: el equipo NO lo lee, formatear en la laptop en FAT32";
  if (!memcmp(c + 3, "NTFS", 4)) return "NTFS: el equipo NO lo lee, formatear en la laptop en FAT32";
  unsigned bps = c[11] | (c[12] << 8), spc = c[13], reservados = c[14] | (c[15] << 8), copias = c[16];
  unsigned raiz = c[17] | (c[18] << 8);
  unsigned long total = c[19] | (c[20] << 8);
  if (!total) total = c[32] | ((unsigned long)c[33] << 8) | ((unsigned long)c[34] << 16) | ((unsigned long)c[35] << 24);
  unsigned long tabla = c[22] | (c[23] << 8);
  bool dice32 = (tabla == 0);
  if (dice32) tabla = c[36] | ((unsigned long)c[37] << 8) | ((unsigned long)c[38] << 16) | ((unsigned long)c[39] << 24);
  if (bps != 512 || spc == 0 || copias == 0 || tabla == 0) return "la cabecera no es FAT: formatear en la laptop en FAT32";
  unsigned long grupos = (total - (reservados + copias * tabla + (raiz * 32UL + 511) / 512)) / spc;
  if (dice32 && grupos < 65525UL) {
    snprintf(b, sizeof(b), "FAT32 con solo %lu grupos (el minimo es 65525): asi formatea un celular las tarjetas chicas y el equipo NO lo acepta. "
                           "Formatear en la laptop: FAT32 con unidad de asignacion de 4096 bytes", grupos);
  } else if (!dice32 && grupos >= 65525UL) {
    snprintf(b, sizeof(b), "FAT16 con demasiados grupos (%lu): formatear en la laptop en FAT32", grupos);
  } else {
    snprintf(b, sizeof(b), "%s correcto (%lu grupos de %u KB)", dice32 ? "FAT32" : "FAT16", grupos, spc / 2);
  }
  return b;
}

//la tarjeta contesta pero no se deja abrir: dice en q paso se queda
// (identificacion, arranque, lectura o formato), hablandole comando por comando
String detalleTarjeta() {
  static uint8_t sector[512];
  char b[230];
  String s = "  Paso a paso:\n";
  iniciarBusTarjeta();
  digitalWrite(PIN_SD_CS, LOW); comandoTarjeta(0, 0); soltarTarjeta();

  // 1, identificacion (comando 8)
  uint8_t eco[4];
  digitalWrite(PIN_SD_CS, LOW);
  uint8_t r8 = comandoTarjeta(8, 0x1AA);
  for (uint8_t &x : eco) x = SPI.transfer(0xFF);
  soltarTarjeta();
  bool v2 = (r8 == 1 && eco[3] == 0xAA);
  snprintf(b, sizeof(b), "    1. Identificacion: %u, eco %02X -> %s\n", r8, eco[3],
           v2 ? "bien" : r8 == 5 ? "bien, tarjeta de primera generacion (hasta 2 GB)" : "MAL: la senal llega danada (cables largos o flojos)");
  s += b;

  // 2 - arranque (comando 41 repetido hasta q diga que esta lista)
  uint8_t r41 = 0xFF;
  unsigned long t0 = millis();
  do {
    digitalWrite(PIN_SD_CS, LOW); comandoTarjeta(55, 0); soltarTarjeta();
    digitalWrite(PIN_SD_CS, LOW); r41 = comandoTarjeta(41, v2 ? 0x40000000UL : 0); soltarTarjeta();
    if (r41 == 0) break;
    delay(20);
  } while (millis() - t0 < 2000);
  snprintf(b, sizeof(b), "    2. Arranque: %u en %lu ms -> %s\n", r41, millis() - t0,
           r41 == 0 ? "bien, la tarjeta quedo lista"
                    : "MAL: la tarjeta NO termina de arrancar. Casi siempre es falta de energia: el VCC del lector necesita 5 V");
  s += b;
  if (r41 != 0) { SPI.endTransaction(); return s; }

  // las de mas de 2 GB numeran por sector y las chicas por byte, lo dice el registro OCR
  uint8_t ocr[4] = { 0, 0, 0, 0 };
  digitalWrite(PIN_SD_CS, LOW); comandoTarjeta(58, 0); for (uint8_t &x : ocr) x = SPI.transfer(0xFF); soltarTarjeta();
  bool porSector = v2 && (ocr[0] & 0x40);
  digitalWrite(PIN_SD_CS, LOW); comandoTarjeta(16, 512); soltarTarjeta();

  // 3, lectura del primer sector
  uint8_t orden, marca;
  uint8_t leido = leerSectorTarjeta(0, porSector, sector, &orden, &marca);
  if (leido == 0) {
    // estado despues del fallo (0 = sigue lista, 1 = se reinicio sola por un bajon de energia, 255 = dejo de contestar),
    // banderas de error (comando 13) y carnet de fabrica (comando 10): si el carnet llega, el camino de datos sirve
    digitalWrite(PIN_SD_CS, LOW);
    uint8_t despues = comandoTarjeta(58, 0);
    for (int i = 0; i < 4; i++) SPI.transfer(0xFF);
    soltarTarjeta();
    digitalWrite(PIN_SD_CS, LOW);
    uint8_t e1 = comandoTarjeta(13, 0), e2 = SPI.transfer(0xFF);
    soltarTarjeta();
    uint8_t cid[16] = { 0 }, marcaCid = 0xFF;
    digitalWrite(PIN_SD_CS, LOW);
    if (comandoTarjeta(10, 0) == 0) {
      unsigned long t2 = millis();
      while ((marcaCid = SPI.transfer(0xFF)) == 0xFF && millis() - t2 < 300) {}
      if (marcaCid == 0xFE) { for (uint8_t &x : cid) x = SPI.transfer(0xFF); SPI.transfer(0xFF); SPI.transfer(0xFF); }
    }
    soltarTarjeta();
    for (int i = 3; i <= 7; i++) if (cid[i] < 32 || cid[i] > 126) cid[i] = '?';
    snprintf(b, sizeof(b), "    3. Lectura: MAL, acepta la orden (%u) pero no manda los datos (marca %02X). Estado despues %u, banderas %02X %02X\n"
                           "    4. Carnet de fabrica: fabricante %02X, modelo %c%c%c%c%c -> si dice 00 y APPSD es una tarjeta generica que no sirve con el equipo: usar otra\n",
             orden, marca, despues, e1, e2, cid[0], cid[3], cid[4], cid[5], cid[6], cid[7]);
    s += b;
  } else if (leido == 1) {
    s += "    3. Lectura: MAL, los datos llegan con errores: cable MISO (19) o SCK (18) flojo, largo o junto a otros\n";
  } else {
    s += "    3. Lectura: bien, los datos llegan sin errores\n";
    //4, formato, si el sector 0 es una tabla de particiones, la cabecera esta donde empieza la primera
    bool esCabecera = (sector[0] == 0xEB || sector[0] == 0xE9);
    uint8_t tipo = sector[450];
    uint32_t inicio = sector[454] | ((uint32_t)sector[455] << 8) | ((uint32_t)sector[456] << 16) | ((uint32_t)sector[457] << 24);
    String formato;
    if (esCabecera || sector[510] != 0x55 || sector[511] != 0xAA) formato = formatoTarjeta(sector);
    else if (tipo == 0xEE) formato = "tabla GPT: el equipo NO la lee, formatear en la laptop en FAT32";
    else if (tipo == 0 || inicio == 0) formato = "sin particiones: formatear en la laptop en FAT32";
    else if (leerSectorTarjeta(inicio, porSector, sector, &orden, &marca) != 2) formato = "no se pudo leer donde empieza la particion";
    else formato = formatoTarjeta(sector);
    s += "    4. Formato: " + formato + "\n";
  }
  SPI.endTransaction();
  return s;
}

const char* motivoReinicio() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "encendido o boton EN";
    case ESP_RST_BROWNOUT: return "BAJON DE VOLTAJE (la fuente no alcanza)";
    case ESP_RST_PANIC:    return "error del programa";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "el programa se colgo";
    case ESP_RST_SW:       return "reinicio pedido por el programa";
    default:               return "otro";
  }
}

String diagnostico() {
  char b[260];
  String s = "DIAGNOSTICO DEL EQUIPO\n";
  snprintf(b, sizeof(b), "Equipo %d (%s). Placa %06lX. Otros equipos en el WiFi: 1 [%s]  2 [%s]  3 [%s]\n"
                         "Encendido hace %lu s. Ultimo reinicio: %s\n\n",
           equipo, MASCOTAS[mascota], (unsigned long)serieDeLaPlaca, direccionDe(1).c_str(), direccionDe(2).c_str(), direccionDe(3).c_str(),
           millis() / 1000, motivoReinicio());
  s += b;
  if (!nubeActiva()) s += "Nube: sin configurar (falta NUBE_URL en claves_nube.h)\n\n";
  else {
    snprintf(b, sizeof(b), "Nube: ultimo envio %d (%s); ultimo guardado hace %lu s\n\n", codigoNube,
             codigoNube == 200 || codigoNube == 204 ? "bien" : codigoNube == 401 || codigoNube == 403 ? "clave rechazada" : "sin guardar",
             ultimoNube ? (millis() - ultimoNube) / 1000 : 0UL);
    s += b;
  }

  // reloj: estado de las lineas del bus y quien contesta
  int sda = gpio_get_level((gpio_num_t)21), scl = gpio_get_level((gpio_num_t)22);
  String vistos;
  for (uint8_t d = 8; d < 120; d++) {
    Wire.beginTransmission(d);
    if (Wire.endTransmission() == 0) { snprintf(b, sizeof(b), " 0x%02X", d); vistos += b; }
  }
  hayReloj = reloj.begin();
  snprintf(b, sizeof(b), "RELOJ (SDA 21, SCL 22)\n  Lineas en reposo: SDA %s, SCL %s\n  Direcciones que contestan:%s\n  Reloj: %s\n",
           sda ? "alta" : "BAJA", scl ? "alta" : "BAJA", vistos.length() ? vistos.c_str() : " ninguna",
           hayReloj ? "detectado" : "NO detectado");
  s += b;
  if (hayReloj) {
    s += "  Hora: " + fechaHora() + (reloj.lostPower() ? "  (perdio la hora: revisar la pila)" : "") + "\n";
  }

  // conversor: se busca de nuevo, cada entrada debe marcar parecido al pin del ESP32 con la misma senal
  buscarADS();
  if (adsDireccion) {
    adsBitsBajos = 0;
    float a0 = leerMilivoltiosADS(ADS_PH), a1 = leerMilivoltiosADS(ADS_TURB);
    float p0 = leerMilivoltios(PIN_PH), p1 = leerMilivoltios(PIN_TURB);
    snprintf(b, sizeof(b), "\nCONVERSOR (en los cables del reloj)\n  Detectado en 0x%02X, tipo %s. Los sensores se leen por el.\n",
             adsDireccion, adsBitsBajos ? "ADS1115 (16 bits)" : "ADS1015 (12 bits)");
    s += b;
    snprintf(b, sizeof(b), "  A0 (pH): %.0f mV. El pin 34 marca %.0f mV\n  A1 (turbidez): %.0f mV. El pin 35 marca %.0f mV\n%s",
             a0, p0, a1, p1, (fabsf(a0 - p0) > 200 || fabsf(a1 - p1) > 200)
               ? "    Diferencia grande entre una entrada y su pin: revisar el puente de esa senal\n" : "");
  } else {
    snprintf(b, sizeof(b), "\nCONVERSOR\n  No detectado: los sensores se leen por los pines del ESP32\n");
  }
  s += b;

  //microSD: primero si la tarjeta contesta, despues si se deja abrir
  uint8_t r = probarTarjeta();
  long velocidad = 0;
  haySD = false;
  for (long v : { 4000000L, 1000000L, 400000L }) {
    if (SD.begin(PIN_SD_CS, SPI, v)) { haySD = true; velocidad = v; break; }
    SD.end();
  }
  snprintf(b, sizeof(b), "\nMICROSD (CS 5, SCK 18, MISO 19, MOSI 23)\n  Respuesta de la tarjeta: %u (%s)\n",
           r, r == 1 ? "contesta" : r == 0xFF ? "nadie contesta: modulo sin conectar, cable MISO suelto o sin tarjeta"
              : r == 0 ? "MISO pegado a 0 V: modulo sin sus 5 V o cable en corto" : "respuesta rara: cables flojos o cruzados");
  s += b;
  if (haySD) {
    prepararSD();
    snprintf(b, sizeof(b), "  Tarjeta abierta a %ld Hz, %lu MB\n", velocidad, (unsigned long)(SD.cardSize() / (1024 * 1024)));
  } else {
    snprintf(b, sizeof(b), "  NO se pudo abrir la tarjeta\n");
  }
  s += b;
  if (!haySD && r == 1) s += detalleTarjeta();

  // memoria interna y hora
  if (hayMemoria) {
    medirMemoria();
    snprintf(b, sizeof(b), "\nMEMORIA INTERNA\n  Usados %u de %u KB\n", (unsigned)memoriaKB, (unsigned)(LittleFS.totalBytes() / 1024));
  } else {
    snprintf(b, sizeof(b), "\nMEMORIA INTERNA\n  NO disponible\n");
  }
  s += b;
  s += "  Hora que se guarda: " + fechaHora() + (hayHoraInternet() ? "  (de internet)" : hayReloj ? "  (del reloj)" : "  (sin hora)") + "\n";

  // GPS
  snprintf(b, sizeof(b), "\nGPS (TX del GPS al pin 16)\n  Letras recibidas: %lu\n  Mensajes correctos: %lu, con error: %lu\n  Satelites: %lu\n  Ubicacion: %s\n",
           (unsigned long)gps.charsProcessed(), (unsigned long)gps.passedChecksum(), (unsigned long)gps.failedChecksum(),
           gps.satellites.isValid() ? (unsigned long)gps.satellites.value() : 0UL, gpsConUbicacion() ? "si" : "todavia no");
  s += b;

  // pantalla
  if (oledDireccion) {
    snprintf(b, sizeof(b), "\nPANTALLA\n  Detectada en 0x%02X, con SDA en D%d y SCL en D%d\n", oledDireccion, oledSda, oledScl);
  } else {
    snprintf(b, sizeof(b), "\nPANTALLA\n  NO detectada en ninguna pareja de los pines 14, 33 y 32. Linea D%d: %s. Linea D%d: %s.\n",
             oledSda, oledSdaConTension ? "con tension" : "SIN tension", oledScl, oledSclConTension ? "con tension" : "SIN tension");
  }
  s += b;

  // sensores
  snprintf(b, sizeof(b), "\nSENSORES\n  Temperatura: %d sensor(es), %.1f C\n  pH: %.0f mV en %s (pH %.2f)\n"
           "  Turbidez: %.0f mV en %s (%.2f V en el sensor, %.0f NTU)\n",
           sensorTemp.getDeviceCount(), temp, phMv, adsDireccion ? "la entrada A0" : "el pin 34", ph,
           turbPinMv, adsDireccion ? "la entrada A1" : "el pin 35", turbV, ntu);
  s += b;
  if (!HAY_SENSOR_CE) snprintf(b, sizeof(b), "  Conductividad: sensor desactivado\n");
  else if (pinCE() < 0 && !adsDireccion) snprintf(b, sizeof(b), "  Conductividad: sin pin libre (la pantalla ocupa el 32 y el 33)\n");
  else {
    // se mide con el sensor apagado y prendido -> al apagarlo la senal tiene que caer a cero
    char donde[16];
    if (adsDireccion) strcpy(donde, "la entrada A2");
    else snprintf(donde, sizeof(donde), "D%d", pinCE());
    float apagado = leerSenal(ADS_CE, pinCE());
    digitalWrite(PIN_CE_ENERGIA, HIGH);
    delay(400);
    float encendido = leerSenal(ADS_CE, pinCE());
    digitalWrite(PIN_CE_ENERGIA, LOW);
    snprintf(b, sizeof(b), "  Conductividad (senal en %s, energia en D13): apagado %.0f mV, encendido %.0f mV (%.0f uS/cm)\n%s",
             donde, apagado, encendido, conductividadDe(encendido),
             apagado > 400 ? "    La senal NO baja al apagar el sensor: el cable azul no esta en ese pin, o el rojo no esta en D13\n"
             : encendido < (adsDireccion ? 20 : 160) ? "    Por debajo de lo que se puede medir: sonda al aire, agua muy pura o sensor sin conectar\n" : "");
  }
  s += b;

  //los numeros de calibracion q usa este equipo
  s += calGuardada ? "\nCalibrado con el boton (guardado en el equipo). Para volver a la tabla del programa: /diagnostico?borrarcal=1" : "\nCalibracion de la tabla del programa (no se ha calibrado con el boton)";
  snprintf(b, sizeof(b), "\nCALIBRACION DE ESTE EQUIPO\n  pH %.2f = %.0f mV y pH %.2f = %.0f mV (%.1f mV por unidad de pH)\n"
           "  Turbidez en agua limpia: %.3f V. Factor de conductividad: %.3f\n",
           cal.phNeutro, cal.mvNeutro, cal.phAcido, cal.mvAcido, (cal.mvAcido - cal.mvNeutro) / (cal.phNeutro - cal.phAcido),
           cal.turbClaraV, cal.ceFactor);
  s += b;
  return s;
}

// lo que se ve en la OLED, dibujado con letras
// ?estado=0..4 muestra 15 s esa animacion: 0 agua bien, 1 pH bajo, 2 pH alto, 3 turbia, 4 sin dato
// &mascota=0..2 la muestra con la mascota de otro equipo: 0 ajolote, 1 nutria, 2 pinguino
//?vista=0..2 cambia a esa vista y prende la pantalla, como el boton: 0 mascota, 1 valores grandes, 2 estado del equipo
void paginaPantalla() {
  if (servidor.hasArg("vista")) {
    oledVista = constrain(servidor.arg("vista").toInt(), 0, 2);
    oledPedida = millis();
    delay(150);
  }
  if (servidor.hasArg("estado")) {
    oledDemoEstado = (EstadoAgua)constrain(servidor.arg("estado").toInt(), 0, 4);
    oledDemoMascota = servidor.hasArg("mascota") ? constrain(servidor.arg("mascota").toInt(), 0, 2) : mascota;
    oledDemoHasta = millis() + 15000;
    delay(150);  // para q alcance a dibujarse el cuadro siguiente
  }
  servidor.send(200, "text/plain; charset=utf-8", imagenEnTexto());
}

void paginaDiagnostico() {
  if (servidor.hasArg("borrarcal")) { ajustes.begin("cal", false); ajustes.clear(); ajustes.end(); cal = CALIBRACIONES[equipo - 1]; calGuardada = false; }
  servidor.send(200, "text/plain; charset=utf-8", diagnostico());
}

// baja todo lo guardado en la memoria interna (primero el anterior y luego el actual)
// mientras baja, el equipo no mide
void paginaCsv() {
  File a, b;
  if (hayMemoria && LittleFS.exists(ARCHIVO_ANTERIOR)) a = LittleFS.open(ARCHIVO_ANTERIOR, FILE_READ);
  if (hayMemoria && LittleFS.exists(ARCHIVO)) b = LittleFS.open(ARCHIVO, FILE_READ);
  if (!a && !b) { servidor.send(404, "text/plain", "Todavia no hay datos guardados"); return; }
  size_t total = (a ? a.size() : 0) + (b ? b.size() : 0);
  if (a && b) {   // los titulos van una sola vez (salvo que hayan cambiado)
    String titulosA = a.readStringUntil('\n'), titulosB = b.readStringUntil('\n');
    a.seek(0);
    if (titulosA == titulosB) total -= titulosB.length() + 1;
    else b.seek(0);
  }
  servidor.sendHeader("Access-Control-Allow-Origin", "*");  //la pagina de otro equipo tmb lo puede bajar
  servidor.sendHeader("Content-Disposition", String("attachment; filename=datos_rio_san_juan_") + EQUIPO_ID + ".csv");
  servidor.setContentLength(total);
  servidor.send(200, "text/csv", "");
  if (a) { servidor.client().write(a); a.close(); }
  if (b) { servidor.client().write(b); b.close(); }
}

// el resumen por hora de todo lo guardado, con la hora en curso al final
//de aqui sale el registro por dias, semanas y meses de la pagina
void paginaResumen() {
  servidor.sendHeader("Access-Control-Allow-Origin", "*");
  servidor.setContentLength(CONTENT_LENGTH_UNKNOWN);
  servidor.send(200, "text/csv", "");
  servidor.sendContent(String(TITULOS_RESUMEN) + "\n");
  for (const char* nombre : { RESUMEN_ANTERIOR, RESUMEN }) {
    if (!hayMemoria || !LittleFS.exists(nombre)) continue;
    File f = LittleFS.open(nombre, FILE_READ);
    if (!f) continue;
    f.readStringUntil('\n');   //los titulos ya se mandaron arriba
    char b[512];
    size_t n;
    while ((n = f.readBytes(b, sizeof(b))) > 0) servidor.sendContent(b, n);
    f.close();
  }
  if (enCurso.hora >= 0 && enCurso.lecturas) {
    char fila[160];
    filaResumen(fila, 150, enCurso);
    strcat(fila, "\n");
    servidor.sendContent(fila);
  }
  servidor.sendContent("");  //fin del envio
}

void setup() {
  Serial.begin(115200);
  reconocerPlaca();
  Serial.printf("\nEquipo %d (%s). Placa %06lX\n", equipo, MASCOTAS[mascota], (unsigned long)serieDeLaPlaca);
  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_ROJO, OUTPUT);
  pinMode(PIN_BOTON, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_BOTON), alMoverBoton, CHANGE);
  pinMode(PIN_BOTON_CAL, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_BOTON_CAL), alMoverBoton2, CHANGE);
  iniciarPantalla();
  parpadear(PIN_LED_VERDE, 2);
  parpadear(PIN_LED_ROJO, 2);

  sensorTemp.begin();
  tempSinEspera = !sensorTemp.isParasitePowerMode();  //con parasita no queda otra que esperar la conversion
  sensorTemp.setWaitForConversion(!tempSinEspera);
  if (tempSinEspera) sensorTemp.requestTemperatures();   //deja una conversion lista para la primera lectura

  Serial2.setRxBufferSize(1024);
  Serial2.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

  Wire.begin(21, 22);
  hayReloj = reloj.begin();
  if (hayReloj && (reloj.lostPower() || reloj.now().year() < 2026)) {
    reloj.adjust(DateTime(F(__DATE__), F(__TIME__)));  // le pone la hora de la pc al subir el codigo
  }
  buscarADS();
  if (adsDireccion) Serial.printf("Conversor ADS: detectado en 0x%02X\n", adsDireccion);

  haySD = SD.begin(PIN_SD_CS);
  if (haySD) prepararSD();

  hayMemoria = LittleFS.begin(true);  // la primera vez la prepara y tarda unos segundos
  Serial.println(hayMemoria ? "Memoria interna: lista" : "Memoria interna: NO disponible");
  recuperarHora();
  if (hayMemoria && LittleFS.exists(ARCHIVO)) {
    // si el archivo viene de una version con otras columnas, pasa a "anterior" y se empieza uno nuevo
    File f = LittleFS.open(ARCHIVO, FILE_READ);
    String titulos = f.readStringUntil('\n');
    f.close();
    titulos.trim();
    if (titulos != TITULOS) {
      LittleFS.remove(ARCHIVO_ANTERIOR);
      LittleFS.rename(ARCHIVO, ARCHIVO_ANTERIOR);
    }
  }
  medirMemoria();

  pinMode(PIN_CE_ENERGIA, OUTPUT);
  digitalWrite(PIN_CE_ENERGIA, LOW);

  //red propia del equipo (siempre) y, si se puede, tambien el wifi de claves.h
  WiFi.mode(USAR_WIFI ? WIFI_AP_STA : WIFI_AP);
  if (WiFi.softAP(AP_NOMBRE, AP_CLAVE, 1, 0, 8)) {
    Serial.printf("Pagina web: conectese a la red \"%s\" y abra http://%s\n", AP_NOMBRE, WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println("No se pudo crear la red propia (la clave AP_CLAVE necesita 8 letras o numeros como minimo)");
  }
  if (USAR_WIFI) {
    WiFi.setAutoReconnect(false); // se reintenta abajo una vez por minuto, pa no tumbar la red propia
    WiFi.begin(WIFI_NOMBRE, WIFI_CLAVE);
    for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) delay(500);
    ultimoIntentoWifi = millis();
  }
  configTime(-5 * 3600, 0, "pool.ntp.org", "time.google.com");   //hora de Peru cuando haya internet
  MDNS.begin(NOMBRE_RED);
  MDNS.addService("http", "tcp", 80);
  avisos.begin(PUERTO_AVISOS);

  servidor.on("/", paginaInicio);
  servidor.on("/datos", paginaDatos);
  servidor.on("/historial", paginaHistorial);
  servidor.on("/datos.csv", paginaCsv);
  servidor.on("/resumen.csv", paginaResumen);
  servidor.on("/diagnostico", paginaDiagnostico);
  servidor.on("/pantalla", paginaPantalla);
  servidor.begin();
  if (nubeActiva()) {
    colaNube = xQueueCreate(1, NUBE_LARGO);
    xTaskCreatePinnedToCore(tareaNube, "nube", 10240, nullptr, 1, nullptr, 0);
  }
}

void loop() {
  leerGPS();
  servidor.handleClient();
  avisarYEscuchar();

  if (millis() - ultimaLectura >= 2000) {
    ultimaLectura = millis();
    leerSensores();
    if (HAY_SENSOR_CE && millis() > 16000 && (ultimaCE == 0 || millis() - ultimaCE >= CE_CADA_MS)) {  // justo despues del pH, pa no moverselo
      ultimaCE = millis();
      leerConductividad();
    }
    calAnotar();
    anotarLectura();
    prepararNube();
    Serial.printf("Temp: %.1f C | pH: %.2f (%.0f mV) | Turbidez: %.0f NTU aprox (%.2f V, pin %.0f mV) | "
                  "CE: %.0f uS/cm (%.0f mV) | Reloj:%s SD:%s GPS:%s WiFi:%s Alerta:%s\n",
                  temp, ph, phMv, ntu, turbV, turbPinMv, ce, ceMv,
                  hayReloj ? "si" : "no", haySD ? "si" : "no", estadoGPS(),
                  WiFi.status() == WL_CONNECTED ? "si" : "no", alerta ? "SI" : "no");
  }

  //WiFi: avisa la direccion al conectar y reintenta una vez x minuto si se cayo
  bool wifiAhora = WiFi.status() == WL_CONNECTED;
  if (wifiAhora && !wifiAntes) {
    Serial.printf("Pagina web en el WiFi \"%s\": http://%s o http://%s.local\n",
                  WIFI_NOMBRE, WiFi.localIP().toString().c_str(), NOMBRE_RED);
  }
  wifiAntes = wifiAhora;
  if (USAR_WIFI && !wifiAhora && millis() - ultimoIntentoWifi > 60000) {
    ultimoIntentoWifi = millis();
    WiFi.begin(WIFI_NOMBRE, WIFI_CLAVE);
  }

  bool calibrando = calBotones(); // segundo boton = calibrar,mientras dura, el boton 1 toma la lectura

  //boton 1, toque corto -> prende la pantalla, si ya estaba prendida,pasa a la vista siguiente
  if (!calibrando && botonToque) {
    botonToque = false;
    if (oledSeVe(millis())) oledVista = (oledVista + 1) % 3;
    oledPedida = millis();
  }

  //boton 1 mantenido: marca una muestra (guarda y manda la medicion de ese momento, con origen "boton")
  unsigned long desde = botonDesde;
  if (!calibrando && desde && millis() - desde >= BOTON_LARGO_MS && digitalRead(PIN_BOTON) == LOW) {
    botonDesde = 0;  // ya esta atendido -> al soltar no cuenta como toque
    botonLargo = true;
  }
  if (botonLargo) {
    botonLargo = false;
    oledAvisoHasta = millis() + 20000;   // "MUESTRA GUARDADA" mientras guarda y manda
    oledPedida = millis();
    Serial.println("Boton: guardando medicion");
    leerSensores();
    bool subida = millis() - ultimoEnvio > 16000 && enviarThingSpeak(); // ThingSpeak nomas acepta un dato cada 15 s
    guardarLectura("boton", subida);
    parpadear(PIN_LED_VERDE, 3);
    ponerLeds();
    oledAvisoHasta = millis() + 1500;   // y lo sigue diciendo un ratito mas
  }

  // guardado y envio automatico
  if (calibrando) ultimoEnvio = millis(); //con las sondas en los patrones no se guarda ni se manda
  if (millis() - ultimoEnvio > ENVIAR_CADA_MS) {
    ultimoEnvio = millis();
    guardarLectura("auto", enviarThingSpeak());  // primero a la red, a la memoria interna solo si no subio
  }
  delay(20);
}
