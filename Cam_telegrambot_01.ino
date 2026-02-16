/*
  Rui Santos
  Complete project details at https://RandomNerdTutorials.com/telegram-esp32-cam-photo-arduino/
  
  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files.
  
  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <math.h>
#include <Preferences.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include <WebServer.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include "secrets.h"

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* TIME_ZONE_INFO = "CET-1CEST,M3.5.0/2,M10.5.0/3";  // Europe/Madrid
const float LOCATION_LAT = 42.4680f;  // Cirueña
const float LOCATION_LON = -2.8950f;  // Cirueña

// Initialize Telegram BOT
String BOTtoken = TELEGRAM_BOT_TOKEN;  // your Bot Token (Get from Botfather)

// Use @myidbot to find out the chat ID of an individual or a group
// Also note that you need to click "start" on a bot before it can
// message you
String CHAT_ID = TELEGRAM_CHAT_ID;

bool sendPhoto = false;
bool sdCardReady = false;
bool deleteAllPending = false;
unsigned long deleteAllPendingSince = 0;
const unsigned long deleteAllConfirmWindowMs = 30000;

bool scheduleFixedEnabled = false;
int scheduleFixedMinuteOfDay = -1;
bool scheduleSunriseEnabled = false;
bool scheduleSunsetEnabled = false;
const int sunWindowMinutes = 30;
const int sunIntervalMinutes = 2;
int lastFixedTriggerYday = -1;
int lastSunriseTriggerYday = -1;
int lastSunsetTriggerYday = -1;
int lastSunriseTriggerMinute = -1;
int lastSunsetTriggerMinute = -1;
unsigned long lastSchedulerCheckMs = 0;
const unsigned long schedulerCheckDelayMs = 10000;
Preferences preferences;
const char* PREF_NS = "cam_cfg";
WebServer webServer(80);
String bootLog;
unsigned long bootStartMs = 0;

WiFiClientSecure clientTCP;
UniversalTelegramBot bot(BOTtoken, clientTCP);

#define FLASH_LED_PIN 4
bool flashState = LOW;
bool flashActive = false;
unsigned long flashStartedAt = 0;
const unsigned long flashOnDurationMs = 20000;

//Checks for new messages every 1 second.
int botRequestDelay = 1000;
unsigned long lastTimeBotRan;

//CAMERA_MODEL_AI_THINKER
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22


void configInitCamera(){
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  //init with high specs to pre-allocate larger buffers
  if(psramFound()){
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;  //0-63 lower number means higher quality
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;  //0-63 lower number means higher quality
    config.fb_count = 1;
  }
  
  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    delay(1000);
    ESP.restart();
  }

  // Drop down frame size for higher initial frame rate
  sensor_t * s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_XGA);  // UXGA|SXGA|XGA|SVGA|VGA|CIF|QVGA|HQVGA|QQVGA
}

bool initSDCard() {
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("SD_MMC mount failed");
    return false;
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD card attached");
    return false;
  }

  uint64_t cardSizeMB = SD_MMC.cardSize() / (1024 * 1024);
  Serial.printf("SD Card Size: %lluMB\n", cardSizeMB);
  return true;
}

String savePhotoToSDCard() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    return "";
  }

  String path = "/photo_" + String(millis()) + ".jpg";
  File file = SD_MMC.open(path, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open file in SD");
    esp_camera_fb_return(fb);
    return "";
  }

  size_t written = file.write(fb->buf, fb->len);
  file.close();
  esp_camera_fb_return(fb);

  if (written == fb->len) {
    Serial.println("Photo saved to SD: " + path);
    return path;
  }

  Serial.println("Failed to write complete photo to SD");
  return "";
}

String listPhotosSDCard() {
  File root = SD_MMC.open("/");
  if (!root || !root.isDirectory()) {
    return "No se pudo leer el directorio de la microSD";
  }

  String response = "Fotos en microSD:\n";
  int photoCount = 0;

  File file = root.openNextFile();
  while (file) {
    String name = String(file.name());
    if (!file.isDirectory() && name.endsWith(".jpg")) {
      photoCount++;
      response += String(photoCount) + ") " + name + "\n";
      if (response.length() > 3500) {
        response += "... (lista truncada)";
        file.close();
        break;
      }
    }
    file.close();
    file = root.openNextFile();
  }
  root.close();

  if (photoCount == 0) {
    return "No hay fotos .jpg en la microSD";
  }
  return response;
}

String deletePhotoFromSDCard(String fileName) {
  if (fileName.length() == 0) {
    return "Usa: /borrar photo_123.jpg";
  }

  fileName.trim();
  if (!fileName.startsWith("/")) {
    fileName = "/" + fileName;
  }

  if (!SD_MMC.exists(fileName)) {
    return "El archivo no existe: " + fileName;
  }

  if (SD_MMC.remove(fileName)) {
    return "Eliminado: " + fileName;
  }

  return "No se pudo eliminar: " + fileName;
}

String deleteAllPhotosFromSDCard() {
  File root = SD_MMC.open("/");
  if (!root || !root.isDirectory()) {
    return "No se pudo leer el directorio de la microSD";
  }

  int removed = 0;
  int failed = 0;

  File file = root.openNextFile();
  while (file) {
    String name = String(file.name());
    bool isDir = file.isDirectory();
    file.close();

    if (!isDir && name.endsWith(".jpg")) {
      if (!name.startsWith("/")) {
        name = "/" + name;
      }
      if (SD_MMC.remove(name)) {
        removed++;
      } else {
        failed++;
      }
    }
    file = root.openNextFile();
  }
  root.close();

  if (removed == 0 && failed == 0) {
    return "No hay fotos .jpg para eliminar";
  }

  return "Borrado masivo finalizado. Eliminadas: " + String(removed) + " | Fallidas: " + String(failed);
}

bool isTimeSynced() {
  time_t now = time(nullptr);
  return now > 1700000000;
}

int normalizeMinuteOfDay(int minuteOfDay) {
  while (minuteOfDay < 0) {
    minuteOfDay += 1440;
  }
  while (minuteOfDay >= 1440) {
    minuteOfDay -= 1440;
  }
  return minuteOfDay;
}

String minuteToHHMM(int minuteOfDay) {
  minuteOfDay = normalizeMinuteOfDay(minuteOfDay);
  int hh = minuteOfDay / 60;
  int mm = minuteOfDay % 60;
  char buffer[6];
  snprintf(buffer, sizeof(buffer), "%02d:%02d", hh, mm);
  return String(buffer);
}

bool parseHHMM(String value, int &minuteOfDay) {
  value.trim();
  if (value.length() != 5 || value.charAt(2) != ':') {
    return false;
  }

  int hh = value.substring(0, 2).toInt();
  int mm = value.substring(3, 5).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
    return false;
  }

  minuteOfDay = hh * 60 + mm;
  return true;
}

int getUtcOffsetMinutes(const struct tm &localTime) {
  char offsetStr[6];
  if (strftime(offsetStr, sizeof(offsetStr), "%z", &localTime) == 0) {
    return 60;
  }

  int sign = (offsetStr[0] == '-') ? -1 : 1;
  int hours = (offsetStr[1] - '0') * 10 + (offsetStr[2] - '0');
  int mins = (offsetStr[3] - '0') * 10 + (offsetStr[4] - '0');
  return sign * (hours * 60 + mins);
}

bool computeSunTimesMinutes(const struct tm &localTime, int &sunriseMinute, int &sunsetMinute) {
  int dayOfYear = localTime.tm_yday + 1;
  float gamma = 2.0f * PI / 365.0f * (dayOfYear - 1);

  float eqTime = 229.18f * (0.000075f + 0.001868f * cosf(gamma) - 0.032077f * sinf(gamma)
      - 0.014615f * cosf(2.0f * gamma) - 0.040849f * sinf(2.0f * gamma));

  float decl = 0.006918f - 0.399912f * cosf(gamma) + 0.070257f * sinf(gamma)
      - 0.006758f * cosf(2.0f * gamma) + 0.000907f * sinf(2.0f * gamma)
      - 0.002697f * cosf(3.0f * gamma) + 0.00148f * sinf(3.0f * gamma);

  float latRad = LOCATION_LAT * PI / 180.0f;
  float zenithRad = 90.833f * PI / 180.0f;
  float cosH = (cosf(zenithRad) / (cosf(latRad) * cosf(decl))) - tanf(latRad) * tanf(decl);

  if (cosH < -1.0f || cosH > 1.0f) {
    return false;
  }

  float hourAngleDeg = acosf(cosH) * 180.0f / PI;
  int tzOffsetMinutes = getUtcOffsetMinutes(localTime);

  float sunrise = 720.0f - 4.0f * (LOCATION_LON + hourAngleDeg) - eqTime + tzOffsetMinutes;
  float sunset = 720.0f - 4.0f * (LOCATION_LON - hourAngleDeg) - eqTime + tzOffsetMinutes;

  sunriseMinute = normalizeMinuteOfDay((int)roundf(sunrise));
  sunsetMinute = normalizeMinuteOfDay((int)roundf(sunset));
  return true;
}

bool shouldTriggerSunSeries(int minuteNow, int sunMinute) {
  int delta = minuteNow - sunMinute;
  if (delta > 720) {
    delta -= 1440;
  } else if (delta < -720) {
    delta += 1440;
  }

  return abs(delta) <= sunWindowMinutes && (abs(delta) % sunIntervalMinutes == 0);
}

void saveScheduleConfig() {
  if (!preferences.begin(PREF_NS, false)) {
    return;
  }

  preferences.putBool("fix_en", scheduleFixedEnabled);
  preferences.putInt("fix_min", scheduleFixedMinuteOfDay);
  preferences.putBool("sunr_en", scheduleSunriseEnabled);
  preferences.putBool("suns_en", scheduleSunsetEnabled);
  preferences.end();
}

void loadScheduleConfig() {
  if (!preferences.begin(PREF_NS, true)) {
    return;
  }

  scheduleFixedEnabled = preferences.getBool("fix_en", false);
  scheduleFixedMinuteOfDay = preferences.getInt("fix_min", -1);
  scheduleSunriseEnabled = preferences.getBool("sunr_en", false);
  scheduleSunsetEnabled = preferences.getBool("suns_en", false);
  preferences.end();

  if (scheduleFixedMinuteOfDay < 0 || scheduleFixedMinuteOfDay >= 1440) {
    scheduleFixedMinuteOfDay = -1;
    scheduleFixedEnabled = false;
  }
}

String getSchedulingStatus() {
  String status = "Programación:\n";

  bool anyProgramActive = scheduleFixedEnabled || scheduleSunriseEnabled || scheduleSunsetEnabled;
  if (!anyProgramActive) {
    status += "- Estado: sin programación activa\n";
  }

  status += "- Hora fija: ";
  status += scheduleFixedEnabled ? ("activa (" + minuteToHHMM(scheduleFixedMinuteOfDay) + ")\n") : "inactiva\n";

  status += "- Amanecer serie (2 min, -30/+30): ";
  status += scheduleSunriseEnabled ? "activa\n" : "inactiva\n";

  status += "- Atardecer serie (2 min, -30/+30): ";
  status += scheduleSunsetEnabled ? "activa\n" : "inactiva\n";

  if (isTimeSynced()) {
    time_t now = time(nullptr);
    struct tm localNow;
    localtime_r(&now, &localNow);

    int sunriseMinute = 0;
    int sunsetMinute = 0;
    if (computeSunTimesMinutes(localNow, sunriseMinute, sunsetMinute)) {
      status += "- Amanecer hoy: " + minuteToHHMM(sunriseMinute) + "\n";
      status += "- Ventana amanecer: " + minuteToHHMM(sunriseMinute - sunWindowMinutes) + " -> " + minuteToHHMM(sunriseMinute + sunWindowMinutes) + "\n";
      status += "- Atardecer hoy: " + minuteToHHMM(sunsetMinute) + "\n";
      status += "- Ventana atardecer: " + minuteToHHMM(sunsetMinute - sunWindowMinutes) + " -> " + minuteToHHMM(sunsetMinute + sunWindowMinutes);
    }
  }

  return status;
}

String getBootStatusMessage() {
  String msg = "ON ✅\n";
  msg += "- Dispositivo: ESP32-CAM\n";
  msg += "- IP: " + WiFi.localIP().toString() + "\n";
  msg += "- WiFi: " + String(ssid) + "\n";
  msg += "- RSSI: " + String(WiFi.RSSI()) + " dBm\n";
  msg += "- microSD: ";
  msg += sdCardReady ? "lista\n" : "no disponible\n";
  msg += getSchedulingStatus();
  return msg;
}

String htmlEscape(const String &value) {
  String escaped = value;
  escaped.replace("&", "&amp;");
  escaped.replace("<", "&lt;");
  escaped.replace(">", "&gt;");
  return escaped;
}

void appendBootLog(const String &line) {
  Serial.println(line);
  bootLog += line + "\n";
}

String getCameraFrameSizeText() {
  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) {
    return "desconocido";
  }

  switch (sensor->status.framesize) {
    case FRAMESIZE_QQVGA: return "QQVGA";
    case FRAMESIZE_QCIF: return "QCIF";
    case FRAMESIZE_HQVGA: return "HQVGA";
    case FRAMESIZE_240X240: return "240x240";
    case FRAMESIZE_QVGA: return "QVGA";
    case FRAMESIZE_CIF: return "CIF";
    case FRAMESIZE_HVGA: return "HVGA";
    case FRAMESIZE_VGA: return "VGA";
    case FRAMESIZE_SVGA: return "SVGA";
    case FRAMESIZE_XGA: return "XGA";
    case FRAMESIZE_HD: return "HD";
    case FRAMESIZE_SXGA: return "SXGA";
    case FRAMESIZE_UXGA: return "UXGA";
    default: return "otro";
  }
}

String getCurrentTimeString() {
  if (!isTimeSynced()) {
    return "No sincronizada";
  }

  time_t now = time(nullptr);
  struct tm localNow;
  localtime_r(&now, &localNow);
  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localNow);
  return String(buffer);
}

String getCommandsTelegramMarkdown() {
  String commands;
  commands.reserve(1200);
  commands += "Comandos disponibles por tipo:\n\n";
  commands += "📸 *Captura y flash*\n";
  commands += "- */inicio* : muestra esta ayuda\n";
  commands += "- */foto* : toma una foto y la envía por Telegram\n";
  commands += "- */flash* : enciende el flash durante 20s y se apaga\n";
  commands += "- */reiniciar* : reinicia el ESP32\n\n";
  commands += "💾 *Gestión microSD*\n";
  commands += "- */listar* : lista las fotos de la microSD\n";
  commands += "- */borrar <nombre.jpg>* : borra una foto de la microSD\n";
  commands += "- */borrar_todo* : solicita borrar todas las fotos\n";
  commands += "- */confirmar_borrado* : confirma el borrado masivo\n";
  commands += "- */cancelar_borrado* : cancela el borrado masivo\n\n";
  commands += "⏱️ *Programación automática*\n";
  commands += "- */prog_hora HH:MM* : foto diaria a una hora fija\n";
  commands += "- */prog_amanecer* : serie cada 2 min entre -30/+30 de amanecer\n";
  commands += "- */prog_atardecer* : serie cada 2 min entre -30/+30 de atardecer\n";
  commands += "- */stop_hora* : detiene la hora fija\n";
  commands += "- */stop_amanecer* : detiene la serie de amanecer\n";
  commands += "- */stop_atardecer* : detiene la serie de atardecer\n";
  commands += "- */stop_todo* : detiene toda la programación automática\n";
  commands += "- */estado_programacion* : muestra el estado actual\n";
  return commands;
}

String getCommandsHtml() {
  String html;
  html.reserve(1700);
  html += "<h2>Comandos</h2>";
  html += "<h3>📸 Captura y flash</h3><ul>";
  html += "<li><strong>/inicio</strong>: muestra esta ayuda</li>";
  html += "<li><strong>/foto</strong>: toma una foto y la envía por Telegram</li>";
  html += "<li><strong>/flash</strong>: enciende el flash durante 20s y se apaga</li>";
  html += "<li><strong>/reiniciar</strong>: reinicia el ESP32</li>";
  html += "</ul>";
  html += "<h3>💾 Gestión microSD</h3><ul>";
  html += "<li><strong>/listar</strong>: lista las fotos de la microSD</li>";
  html += "<li><strong>/borrar &lt;nombre.jpg&gt;</strong>: borra una foto de la microSD</li>";
  html += "<li><strong>/borrar_todo</strong>: solicita borrar todas las fotos</li>";
  html += "<li><strong>/confirmar_borrado</strong>: confirma el borrado masivo</li>";
  html += "<li><strong>/cancelar_borrado</strong>: cancela el borrado masivo</li>";
  html += "</ul>";
  html += "<h3>⏱️ Programación automática</h3><ul>";
  html += "<li><strong>/prog_hora HH:MM</strong>: foto diaria a una hora fija</li>";
  html += "<li><strong>/prog_amanecer</strong>: serie cada 2 min entre -30/+30 de amanecer</li>";
  html += "<li><strong>/prog_atardecer</strong>: serie cada 2 min entre -30/+30 de atardecer</li>";
  html += "<li><strong>/stop_hora</strong>: detiene la hora fija</li>";
  html += "<li><strong>/stop_amanecer</strong>: detiene la serie de amanecer</li>";
  html += "<li><strong>/stop_atardecer</strong>: detiene la serie de atardecer</li>";
  html += "<li><strong>/stop_todo</strong>: detiene toda la programación automática</li>";
  html += "<li><strong>/estado_programacion</strong>: muestra el estado actual</li>";
  html += "</ul>";
  return html;
}

String getWebStatusText() {
  String status;
  status.reserve(2048);

  status += "Estado actual\n";
  status += "============\n";
  status += "Dispositivo: ESP32-CAM\n";
  status += "Firmware compilado: " + String(__DATE__) + " " + String(__TIME__) + "\n";
  status += "Uptime (s): " + String((millis() - bootStartMs) / 1000) + "\n";
  status += "Tiempo local: " + getCurrentTimeString() + "\n";
  status += "Zona horaria: " + String(TIME_ZONE_INFO) + "\n";
  status += "Lat/Lon: " + String(LOCATION_LAT, 4) + ", " + String(LOCATION_LON, 4) + "\n";
  status += "\nConectividad\n------------\n";
  status += "SSID: " + String(ssid) + "\n";
  status += "IP: " + WiFi.localIP().toString() + "\n";
  status += "Gateway: " + WiFi.gatewayIP().toString() + "\n";
  status += "DNS: " + WiFi.dnsIP().toString() + "\n";
  status += "MAC: " + WiFi.macAddress() + "\n";
  status += "RSSI: " + String(WiFi.RSSI()) + " dBm\n";
  status += "\nHardware\n--------\n";
  status += "microSD: " + String(sdCardReady ? "lista" : "no disponible") + "\n";
  status += "PSRAM: " + String(psramFound() ? "sí" : "no") + "\n";
  status += "Heap libre: " + String(ESP.getFreeHeap()) + " bytes\n";
  status += "Flash LED: " + String(flashState ? "encendido" : "apagado") + "\n";
  status += "Frame cámara: " + getCameraFrameSizeText() + "\n";
  status += "\nConfiguración\n-------------\n";
  status += "Bot poll delay (ms): " + String(botRequestDelay) + "\n\n";
  status += getSchedulingStatus() + "\n";
  status += "\nLog de arranque\n---------------\n";
  status += bootLog;

  return status;
}

String buildStatusPage() {
  String body = getWebStatusText();
  String html;
  String commandsHtml = getCommandsHtml();
  html.reserve(body.length() + commandsHtml.length() + 700);
  html += "<!doctype html><html lang='es'><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='5'>";
  html += "<title>ESP32-CAM Estado</title>";
  html += "<style>body{font-family:Arial,sans-serif;margin:16px;background:#f5f5f5;}";
  html += "h1{font-size:20px;}h2{margin-top:20px;}h3{margin:12px 0 6px;}ul{margin-top:6px;}";
  html += "pre{white-space:pre-wrap;background:#fff;border:1px solid #ddd;padding:12px;border-radius:8px;}</style>";
  html += "</head><body><h1>ESP32-CAM - Estado de arranque y configuración</h1>";
  html += "<pre>" + htmlEscape(body) + "</pre>";
  html += commandsHtml;
  html += "</body></html>";
  return html;
}

void handleWebRoot() {
  webServer.send(200, "text/html; charset=utf-8", buildStatusPage());
}

void handleWebStatus() {
  webServer.send(200, "text/plain; charset=utf-8", getWebStatusText());
}

void handleWebNotFound() {
  webServer.send(404, "text/plain; charset=utf-8", "Ruta no encontrada. Usa / o /estado");
}

void processScheduledCaptures() {
  if (!isTimeSynced()) {
    return;
  }

  if (millis() - lastSchedulerCheckMs < schedulerCheckDelayMs) {
    return;
  }
  lastSchedulerCheckMs = millis();

  time_t now = time(nullptr);
  struct tm localNow;
  localtime_r(&now, &localNow);

  int minuteNow = localNow.tm_hour * 60 + localNow.tm_min;
  int ydayNow = localNow.tm_yday;

  if (scheduleFixedEnabled && minuteNow == scheduleFixedMinuteOfDay && lastFixedTriggerYday != ydayNow) {
    sendPhoto = true;
    bot.sendMessage(CHAT_ID, "Programación automática: foto de hora fija (Telegram + microSD si está disponible)", "");
    lastFixedTriggerYday = ydayNow;
  }

  int sunriseMinute = 0;
  int sunsetMinute = 0;
  if (computeSunTimesMinutes(localNow, sunriseMinute, sunsetMinute)) {
    if (scheduleSunriseEnabled && shouldTriggerSunSeries(minuteNow, sunriseMinute)
        && (lastSunriseTriggerYday != ydayNow || lastSunriseTriggerMinute != minuteNow)) {
      sendPhoto = true;
      lastSunriseTriggerYday = ydayNow;
      lastSunriseTriggerMinute = minuteNow;
      bot.sendMessage(CHAT_ID, "Programación automática: foto de la serie de amanecer", "");
    }

    if (scheduleSunsetEnabled && shouldTriggerSunSeries(minuteNow, sunsetMinute)
        && (lastSunsetTriggerYday != ydayNow || lastSunsetTriggerMinute != minuteNow)) {
      sendPhoto = true;
      lastSunsetTriggerYday = ydayNow;
      lastSunsetTriggerMinute = minuteNow;
      bot.sendMessage(CHAT_ID, "Programación automática: foto de la serie de atardecer", "");
    }
  }
}

void handleNewMessages(int numNewMessages) {
  Serial.print("Handle New Messages: ");
  Serial.println(numNewMessages);

  for (int i = 0; i < numNewMessages; i++) {
    if (deleteAllPending && (millis() - deleteAllPendingSince > deleteAllConfirmWindowMs)) {
      deleteAllPending = false;
    }

    String chat_id = String(bot.messages[i].chat_id);
    if (chat_id != CHAT_ID){
      bot.sendMessage(chat_id, "Usuario no autorizado", "");
      continue;
    }
    
    // Print the received message
    String text = bot.messages[i].text;
    Serial.println(text);
    
    String from_name = bot.messages[i].from_name;
    if (text == "/inicio") {
      String welcome = "Bienvenido, " + from_name + "\n";
      welcome += getCommandsTelegramMarkdown();
      bot.sendMessage(CHAT_ID, welcome, "Markdown");
    }
    if (text == "/flash") {
      flashState = HIGH;
      flashActive = true;
      flashStartedAt = millis();
      digitalWrite(FLASH_LED_PIN, flashState);
      bot.sendMessage(CHAT_ID, "Flash encendido 20 segundos. Se apagará automáticamente.", "");
      Serial.println("Flash encendido durante 20s");
    }
    if (text == "/reiniciar") {
      bot.sendMessage(CHAT_ID, "Reiniciando ESP32...", "");
      Serial.println("Reinicio solicitado por Telegram");
      delay(100);
      ESP.restart();
    }
    if (text == "/foto") {
      sendPhoto = true;
      Serial.println("Enviando foto...");
    }
    if (text == "/listar") {
      if (!sdCardReady) {
        bot.sendMessage(CHAT_ID, "La microSD no está lista", "");
      } else {
        bot.sendMessage(CHAT_ID, listPhotosSDCard(), "");
      }
    }
    if (text == "/borrar_todo") {
      if (!sdCardReady) {
        bot.sendMessage(CHAT_ID, "La microSD no está lista", "");
      } else {
        deleteAllPending = true;
        deleteAllPendingSince = millis();
        bot.sendMessage(CHAT_ID, "¡Atención! Esto borrará todas las fotos .jpg. Para confirmar, envía: /confirmar_borrado (en 30 segundos)", "");
      }
    }
    if (text.startsWith("/borrar ") || text == "/borrar") {
      if (!sdCardReady) {
        bot.sendMessage(CHAT_ID, "La microSD no está lista", "");
      } else {
        String target = "";
        if (text.length() > 8) {
          target = text.substring(8);
          target.trim();
        }
        bot.sendMessage(CHAT_ID, deletePhotoFromSDCard(target), "");
      }
    }
    if (text == "/confirmar_borrado") {
      if (!deleteAllPending) {
        bot.sendMessage(CHAT_ID, "No hay ningún borrado masivo pendiente", "");
      } else if (millis() - deleteAllPendingSince > deleteAllConfirmWindowMs) {
        deleteAllPending = false;
        bot.sendMessage(CHAT_ID, "La confirmación ha caducado. Usa de nuevo /borrar_todo", "");
      } else if (!sdCardReady) {
        bot.sendMessage(CHAT_ID, "La microSD no está lista", "");
      } else {
        bot.sendMessage(CHAT_ID, deleteAllPhotosFromSDCard(), "");
        deleteAllPending = false;
      }
    }
    if (text == "/cancelar_borrado") {
      if (deleteAllPending) {
        deleteAllPending = false;
        bot.sendMessage(CHAT_ID, "Borrado masivo cancelado", "");
      } else {
        bot.sendMessage(CHAT_ID, "No hay ningún borrado masivo pendiente", "");
      }
    }
    if (text.startsWith("/prog_hora")) {
      String value = "";
      if (text.length() > 10) {
        value = text.substring(10);
      }
      value.trim();
      int minuteOfDay = -1;
      if (!parseHHMM(value, minuteOfDay)) {
        bot.sendMessage(CHAT_ID, "Usa: /prog_hora HH:MM (ejemplo: /prog_hora 08:30)", "");
      } else {
        scheduleFixedMinuteOfDay = minuteOfDay;
        scheduleFixedEnabled = true;
        saveScheduleConfig();
        bot.sendMessage(CHAT_ID, "Programación de hora fija activada: " + minuteToHHMM(scheduleFixedMinuteOfDay), "");
      }
    }
    if (text.startsWith("/prog_amanecer")) {
      scheduleSunriseEnabled = true;
      saveScheduleConfig();
      bot.sendMessage(CHAT_ID, "Serie de amanecer activada: cada 2 min, de -30 a +30. Todas las fotos se envían a Telegram", "");
    }
    if (text.startsWith("/prog_atardecer")) {
      scheduleSunsetEnabled = true;
      saveScheduleConfig();
      bot.sendMessage(CHAT_ID, "Serie de atardecer activada: cada 2 min, de -30 a +30. Todas las fotos se envían a Telegram", "");
    }
    if (text == "/stop_hora") {
      scheduleFixedEnabled = false;
      saveScheduleConfig();
      bot.sendMessage(CHAT_ID, "Programación de hora fija detenida", "");
    }
    if (text == "/stop_amanecer") {
      scheduleSunriseEnabled = false;
      lastSunriseTriggerYday = -1;
      lastSunriseTriggerMinute = -1;
      saveScheduleConfig();
      bot.sendMessage(CHAT_ID, "Programación de amanecer detenida", "");
    }
    if (text == "/stop_atardecer") {
      scheduleSunsetEnabled = false;
      lastSunsetTriggerYday = -1;
      lastSunsetTriggerMinute = -1;
      saveScheduleConfig();
      bot.sendMessage(CHAT_ID, "Programación de atardecer detenida", "");
    }
    if (text == "/stop_todo") {
      scheduleFixedEnabled = false;
      scheduleSunriseEnabled = false;
      scheduleSunsetEnabled = false;
      lastSunriseTriggerYday = -1;
      lastSunriseTriggerMinute = -1;
      lastSunsetTriggerYday = -1;
      lastSunsetTriggerMinute = -1;
      saveScheduleConfig();
      bot.sendMessage(CHAT_ID, "Toda la programación automática detenida", "");
    }
    if (text == "/estado_programacion") {
      bot.sendMessage(CHAT_ID, getSchedulingStatus(), "");
    }
  }
}

String sendPhotoTelegram() {
  const char* myDomain = "api.telegram.org";
  String getAll = "";
  String getBody = "";

  camera_fb_t * fb = NULL;
  fb = esp_camera_fb_get();  
  if(!fb) {
    Serial.println("Camera capture failed");
    delay(1000);
    ESP.restart();
    return "Camera capture failed";
  }  
  
  Serial.println("Connect to " + String(myDomain));


  if (clientTCP.connect(myDomain, 443)) {
    Serial.println("Connection successful");
    
    String head = "--RandomNerdTutorials\r\nContent-Disposition: form-data; name=\"chat_id\"; \r\n\r\n" + CHAT_ID + "\r\n--RandomNerdTutorials\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"esp32-cam.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
    String tail = "\r\n--RandomNerdTutorials--\r\n";

    uint16_t imageLen = fb->len;
    uint16_t extraLen = head.length() + tail.length();
    uint16_t totalLen = imageLen + extraLen;
  
    clientTCP.println("POST /bot"+BOTtoken+"/sendPhoto HTTP/1.1");
    clientTCP.println("Host: " + String(myDomain));
    clientTCP.println("Content-Length: " + String(totalLen));
    clientTCP.println("Content-Type: multipart/form-data; boundary=RandomNerdTutorials");
    clientTCP.println();
    clientTCP.print(head);
  
    uint8_t *fbBuf = fb->buf;
    size_t fbLen = fb->len;
    for (size_t n=0;n<fbLen;n=n+1024) {
      if (n+1024<fbLen) {
        clientTCP.write(fbBuf, 1024);
        fbBuf += 1024;
      }
      else if (fbLen%1024>0) {
        size_t remainder = fbLen%1024;
        clientTCP.write(fbBuf, remainder);
      }
    }  
    
    clientTCP.print(tail);
    
    esp_camera_fb_return(fb);
    
    int waitTime = 10000;   // timeout 10 seconds
    long startTimer = millis();
    boolean state = false;
    
    while ((startTimer + waitTime) > millis()){
      Serial.print(".");
      delay(100);      
      while (clientTCP.available()) {
        char c = clientTCP.read();
        if (state==true) getBody += String(c);        
        if (c == '\n') {
          if (getAll.length()==0) state=true; 
          getAll = "";
        } 
        else if (c != '\r')
          getAll += String(c);
        startTimer = millis();
      }
      if (getBody.length()>0) break;
    }
    clientTCP.stop();
    Serial.println(getBody);
  }
  else {
    getBody="Connected to api.telegram.org failed.";
    Serial.println("Connected to api.telegram.org failed.");
  }
  return getBody;
}

void setup(){
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); 
  // Init Serial Monitor
  Serial.begin(115200);
  bootStartMs = millis();
  appendBootLog("Iniciando ESP32-CAM...");

  loadScheduleConfig();
  appendBootLog("Configuración de programación cargada (NVS)");

  // Set LED Flash as output
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, flashState);

  // Config and init the camera
  configInitCamera();
  appendBootLog("Cámara inicializada");

  // Init SD card
  sdCardReady = initSDCard();
  appendBootLog(String("microSD: ") + (sdCardReady ? "lista" : "no disponible"));

  // Connect to Wi-Fi
  WiFi.mode(WIFI_STA);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  clientTCP.setCACert(TELEGRAM_CERTIFICATE_ROOT); // Add root certificate for api.telegram.org
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.print("ESP32-CAM IP Helbidea: ");
  Serial.println(WiFi.localIP()); 
  appendBootLog("WiFi conectado. IP: " + WiFi.localIP().toString());

  configTzTime(TIME_ZONE_INFO, "pool.ntp.org");
  Serial.println("Sincronizando NTP...");
  int ntpRetries = 0;
  while (!isTimeSynced() && ntpRetries < 40) {
    delay(500);
    Serial.print(".");
    ntpRetries++;
  }
  Serial.println();
  if (isTimeSynced()) {
    time_t now = time(nullptr);
    struct tm localNow;
    localtime_r(&now, &localNow);
    char nowBuffer[32];
    strftime(nowBuffer, sizeof(nowBuffer), "%Y-%m-%d %H:%M:%S", &localNow);
    appendBootLog("Hora sincronizada: " + String(nowBuffer));
  } else {
    appendBootLog("No se pudo completar la sincronización NTP");
  }

  webServer.on("/", handleWebRoot);
  webServer.on("/estado", handleWebStatus);
  webServer.onNotFound(handleWebNotFound);
  webServer.begin();
  appendBootLog("Servidor web iniciado en http://" + WiFi.localIP().toString() + "/");

  String startupMessage = getBootStatusMessage();
  startupMessage += "\n\nUsa /inicio para ver los comandos.";
  bot.sendMessage(CHAT_ID, startupMessage, "");
}

void loop() {
  webServer.handleClient();
  processScheduledCaptures();

  if (flashActive && (millis() - flashStartedAt >= flashOnDurationMs)) {
    flashState = LOW;
    flashActive = false;
    digitalWrite(FLASH_LED_PIN, flashState);
    Serial.println("Flash apagado automáticamente tras 20s");
  }

  if (sendPhoto) {
    Serial.println("Preparando foto");
    sendPhotoTelegram();
    if (sdCardReady) {
      String savedPath = savePhotoToSDCard();
      if (savedPath.length() == 0) {
        bot.sendMessage(CHAT_ID, "No se pudo guardar la foto en la microSD", "");
      }
    }
    sendPhoto = false; 
  }

  if (millis() > lastTimeBotRan + botRequestDelay)  {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    while (numNewMessages) {
      Serial.println("got response");
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastTimeBotRan = millis();
  }
}