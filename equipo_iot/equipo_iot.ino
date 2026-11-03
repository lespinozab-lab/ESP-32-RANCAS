// Equipo IoT - Rio San Juan de Rancas
// Mide temperatura (DS18B20), pH y turbidez. Muestra por USB, guarda en microSD
// con fecha y hora, envia a ThingSpeak por WiFi y avisa con dos LEDs.
// El reloj, la microSD, el GPS, el boton y los LEDs son opcionales: si no estan
// conectados, el equipo sigue funcionando sin ellos.
// Monitor serie a 115200.

#include <WiFi.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <RTClib.h>
#include <TinyGPSPlus.h>

// ---- Pines ----
const int PIN_TEMP      = 4;
const int PIN_PH        = 34;
const int PIN_TURB      = 35;
const int PIN_LED_VERDE = 25;
const int PIN_LED_ROJO  = 26;
const int PIN_BOTON     = 27;   // el otro lado del boton va a tierra
const int PIN_SD_CS     = 5;    // microSD: SCK 18, MISO 19, MOSI 23
                                // reloj DS3231: SDA 21, SCL 22
const int PIN_GPS_RX    = 16;   // aqui llega el cable TX del GPS
const int PIN_GPS_TX    = 17;   // aqui llega el cable RX del GPS

// ---- Calibracion: cambiar por sus propias lecturas ----
float PH7_MV = 1500;        // mV que marca en el sobre de pH 7
float PH4_MV = 2032;        // mV que marca en el sobre de pH 4
float TURB_CLARA_V = 3.37;  // PROVISIONAL: medido al aire el 07/10. Repetir con agua limpia.

// ---- Limites para el LED rojo (provisionales) ----
const float PH_MIN  = 6.5;
const float PH_MAX  = 8.5;
const float NTU_MAX = 100;

// ---- WiFi y ThingSpeak ----
const bool USAR_WIFI = true;
const char* WIFI_NOMBRE = "H15P1";
const char* WIFI_CLAVE  = "123pablo";
const char* API_KEY     = "7QQWEPJOKDMQF5IY";
const unsigned long ENVIAR_CADA_MS = 20000;

OneWire oneWire(PIN_TEMP);
DallasTemperature sensorTemp(&oneWire);
RTC_DS3231 reloj;

bool hayReloj = false;
bool haySD = false;
bool alerta = false;
unsigned long ultimoEnvio = 0;
unsigned long ultimaLectura = 0;
unsigned long ultimoBoton = 0;
volatile bool botonPulsado = false;

float temp = 0, ph = 0, phMv = 0, ntu = 0, turbV = 0, turbPinMv = 0;

TinyGPSPlus gps;

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

void IRAM_ATTR alPulsarBoton() { botonPulsado = true; }

float leerMilivoltios(int pin) {
  long suma = 0;
  for (int i = 0; i < 20; i++) { suma += analogReadMilliVolts(pin); delay(10); }
  return suma / 20.0;
}

String fechaHora() {
  if (!hayReloj) return "sin_reloj," + String(millis() / 1000);
  DateTime t = reloj.now();
  char b[24];
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

void leerSensores() {
  sensorTemp.requestTemperatures();
  temp = sensorTemp.getTempCByIndex(0);

  phMv = leerMilivoltios(PIN_PH);
  ph = 7.0 + (PH7_MV - phMv) * 3.0 / (PH4_MV - PH7_MV);

  turbPinMv = leerMilivoltios(PIN_TURB);
  turbV = turbPinMv * 1.5 / 1000.0;            // deshace el divisor de 10k y 20k
  float v = turbV * 4.2 / TURB_CLARA_V;
  ntu = -1120.4 * v * v + 5742.3 * v - 4352.9;
  if (v < 2.5) ntu = 3000;
  if (ntu < 0) ntu = 0;

  // Alerta con un pequeno margen para que el LED no parpadee en el limite
  if (!alerta) {
    alerta = (ph < PH_MIN - 0.05) || (ph > PH_MAX + 0.05) || (ntu > NTU_MAX * 1.1);
  } else {
    alerta = !((ph > PH_MIN + 0.05) && (ph < PH_MAX - 0.05) && (ntu < NTU_MAX * 0.9));
  }
  digitalWrite(PIN_LED_ROJO, alerta ? HIGH : LOW);
  digitalWrite(PIN_LED_VERDE, alerta ? LOW : HIGH);
}

void guardarEnSD(const char* origen) {
  if (!haySD) return;
  File f = SD.open("/datos.csv", FILE_APPEND);
  if (!f) { Serial.println("microSD: no se pudo escribir"); return; }
  f.printf("%s,%.1f,%.2f,%.0f,%.2f,%.0f,", fechaHora().c_str(), temp, ph, ntu, turbV, phMv);
  if (gpsConUbicacion()) f.printf("%.6f,%.6f,", gps.location.lat(), gps.location.lng());
  else f.print(",,");
  f.println(origen);
  f.close();
  Serial.println("microSD: guardado");
}

void enviarThingSpeak() {
  if (!USAR_WIFI || WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  char url[300];
  int n = snprintf(url, sizeof(url),
           "http://api.thingspeak.com/update?api_key=%s&field1=%.1f&field2=%.2f&field3=%.0f",
           API_KEY, temp, ph, ntu);
  if (gpsConUbicacion()) {
    snprintf(url + n, sizeof(url) - n, "&lat=%.6f&long=%.6f", gps.location.lat(), gps.location.lng());
  }
  http.begin(url);
  Serial.printf("Envio a ThingSpeak: %d\n", http.GET());
  http.end();
  ultimoEnvio = millis();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_ROJO, OUTPUT);
  pinMode(PIN_BOTON, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_BOTON), alPulsarBoton, FALLING);
  parpadear(PIN_LED_VERDE, 2);
  parpadear(PIN_LED_ROJO, 2);

  sensorTemp.begin();

  Serial2.setRxBufferSize(1024);
  Serial2.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

  Wire.begin(21, 22);
  hayReloj = reloj.begin();
  if (hayReloj && (reloj.lostPower() || reloj.now().year() < 2026)) {
    reloj.adjust(DateTime(F(__DATE__), F(__TIME__)));   // pone la hora de la PC al subir el codigo
  }

  haySD = SD.begin(PIN_SD_CS);
  if (haySD && !SD.exists("/datos.csv")) {
    File f = SD.open("/datos.csv", FILE_WRITE);
    if (f) { f.println("fecha,hora,temperatura_C,pH,turbidez_NTU,turbidez_V,pH_mV,latitud,longitud,origen"); f.close(); }
  }

  if (USAR_WIFI) {
    WiFi.begin(WIFI_NOMBRE, WIFI_CLAVE);
    for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  }
}

void loop() {
  leerGPS();

  if (millis() - ultimaLectura >= 2000) {
    ultimaLectura = millis();
    leerSensores();
    Serial.printf("Temp: %.1f C | pH: %.2f (%.0f mV) | Turbidez: %.0f NTU aprox (%.2f V, pin %.0f mV) | "
                  "Reloj:%s SD:%s GPS:%s WiFi:%s Alerta:%s\n",
                  temp, ph, phMv, ntu, turbV, turbPinMv,
                  hayReloj ? "si" : "no", haySD ? "si" : "no", estadoGPS(),
                  WiFi.status() == WL_CONNECTED ? "si" : "no", alerta ? "SI" : "no");
  }

  // Boton: guarda y envia la medicion en ese momento
  if (botonPulsado) {
    botonPulsado = false;
    if (millis() - ultimoBoton > 1000) {
      ultimoBoton = millis();
      Serial.println("Boton: guardando medicion");
      leerSensores();
      guardarEnSD("boton");
      if (millis() - ultimoEnvio > 16000) enviarThingSpeak();
      parpadear(PIN_LED_VERDE, 3);
      digitalWrite(PIN_LED_VERDE, alerta ? LOW : HIGH);
    }
  }

  // Envio y guardado automatico
  if (millis() - ultimoEnvio > ENVIAR_CADA_MS) {
    guardarEnSD("auto");
    enviarThingSpeak();
    if (!USAR_WIFI || WiFi.status() != WL_CONNECTED) ultimoEnvio = millis();
  }
  delay(20);
}
