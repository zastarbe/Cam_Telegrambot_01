# ESP32-CAM Telegram Bot (foto + microSD + programación)

Proyecto basado en ESP32-CAM para:
- Enviar fotos por Telegram.
- Guardar fotos en microSD.
- Programar capturas por hora fija.
- Programar series automáticas en amanecer/atardecer.

## 1) Requisitos

- Placa ESP32-CAM (AI Thinker o compatible)
- Arduino IDE con soporte ESP32 instalado
- Librerías:
  - `UniversalTelegramBot`
  - `ArduinoJson`
- Conexión WiFi
- (Opcional) microSD

## 2) Configuración de secretos

Este repo usa un archivo privado con claves reales.

1. Copia `secrets.h.example` a `secrets.h`
2. Rellena tus datos:
   - `WIFI_SSID`
   - `WIFI_PASSWORD`
   - `TELEGRAM_BOT_TOKEN`
   - `TELEGRAM_CHAT_ID`
    - `LOCATION_LAT`
    - `LOCATION_LON`
    - `TIME_ZONE_INFO`

> `secrets.h` está ignorado por Git en `.gitignore`.

## 3) Cargar firmware

1. Abre `Cam_telegrambot_01.ino` en Arduino IDE.
2. Selecciona la placa ESP32-CAM correcta.
3. Compila y sube.
4. Abre monitor serie (115200) para diagnóstico inicial.

Al arrancar, el dispositivo envía por Telegram un mensaje de estado con:
- `ON`
- IP local
- estado WiFi, microSD, NTP y programación activa

Además, levanta un servidor web en la IP local de la ESP32-CAM:
- `http://<IP_ESP32>/` → página HTML con estado completo

La página incluye información de arranque y configuración:
- conectividad (SSID, IP, gateway, DNS, RSSI, MAC)
- hardware (microSD, PSRAM, heap libre, estado flash, frame de cámara)
- hora/NTP, zona horaria, ubicación y programación activa
- log de arranque registrado durante `setup()`

## 4) Comandos de Telegram

### Captura y flash
- `/inicio` → ayuda
- `/foto` → toma foto y la envía por Telegram
- `/guardar` → toma foto y la guarda en microSD
- `/flash` → enciende/apaga flash

### Gestión microSD
- `/listar` → lista fotos `.jpg`
- `/borrar <nombre.jpg>` → borra una foto concreta
- `/borrar_todo` → solicita borrado masivo
- `/confirmar_borrado` → confirma borrado masivo
- `/cancelar_borrado` → cancela borrado masivo

### Programación automática
- `/prog_hora HH:MM` → foto diaria a hora fija
- `/prog_amanecer` → serie cada 2 min desde -30 hasta +30 min alrededor de amanecer
- `/prog_atardecer` → serie cada 2 min desde -30 hasta +30 min alrededor de atardecer
- `/stop_hora` → detiene hora fija
- `/stop_amanecer` → detiene serie de amanecer
- `/stop_atardecer` → detiene serie de atardecer
- `/stop_todo` → detiene toda la programación
- `/estado_programacion` → muestra estado y ventanas de disparo

## 5) Persistencia

La configuración de programación se guarda en NVS:
- Sobrevive a reinicios y cortes de alimentación.
- Amanecer/atardecer se recalculan automáticamente cada día con NTP + zona horaria Madrid.

## 6) Notas

- Si la microSD no está disponible, la serie de amanecer/atardecer sigue enviando fotos por Telegram.
- Si ves errores de `includePath` en VS Code, normalmente son del entorno de IntelliSense, no del firmware en sí.

---
Si quieres, el siguiente paso recomendado es añadir alias en español para comandos (`/start`, `/foto`) manteniendo compatibilidad con los actuales.
