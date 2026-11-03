# DC ECU en ESP32 WROOM

Panel OBD-II servido por el propio ESP32: levanta una red WiFi, sirve la misma
interfaz del panel en el navegador del celular y habla con el ELM327 mini por
Bluetooth Classic (SPP).

```
ESP32 WROOM  ──WiFi AP "Buss-PANEL"──►  celular / laptop (panel web)
      └───────Bluetooth SPP──────►  ELM327 mini  ──►  OBD-II del auto
```

## Material

- ESP32 WROOM-32 DevKit (cualquier clon de 30 o 38 pines).
- ELM327 mini Bluetooth (Bluetooth Classic, no BLE).
- Alimentación 5 V / 1 A. El ELM327 se alimenta del conector OBD-II del auto.

## Cableado

No hay cableado entre el ESP32 y el ELM327: el enlace es inalámbrico. Solo
necesitas alimentar el ESP32 por USB o por VIN/5 V y conectar el ELM327 al
puerto OBD-II del vehículo.

## Compilar y flashear

### Linux: script automático (recomendado)

Con el ESP32 ya conectado por USB:

```bash
cd esp32-elm327
chmod +x instalar.sh
./instalar.sh
```

El script revisa Python, instala PlatformIO en un entorno propio
(`~/.local/share/dc-ecu/pio-venv`), detecta el puerto `/dev/ttyUSB*` o
`/dev/ttyACM*`, compila, flashea el firmware y sube el panel web. Opciones:

```bash
./instalar.sh --port /dev/ttyUSB0   # forzar puerto
./instalar.sh --monitor             # abrir el monitor serie al terminar
./instalar.sh --sin-flash           # solo instalar y compilar
```

Si falla por permisos: `sudo usermod -aG dialout $USER` y vuelve a entrar a la
sesión. Si no aparece el puerto, revisa el cable (debe ser de datos) y el driver
CH340 o CP210x.

### Manual con PlatformIO

```bash
cd esp32-elm327
pio run                    # compila
pio run -t upload          # flashea el firmware
pio run -t uploadfs        # sube data/ (el panel web) a LittleFS
pio device monitor         # logs a 115200
```

Con Arduino IDE: instalar el core ESP32, seleccionar placa "ESP32 Dev Module",
y subir `data/index.html` con la herramienta de LittleFS.

## Uso

1. Enciende el auto o pon el contacto (el ELM327 necesita alimentación).
2. Conéctate a la WiFi `Buss-PANEL` (clave `Juanita123`).
3. Abre `http://192.168.4.1`.
4. Pestaña AJUSTES → Escanear adaptadores → elige el ELM327 (PIN 1234, algunos
   clones usan 0000; si falla, emparéjalo primero desde el celular).
5. El panel empieza a pedir los 20 PIDs y a pintar valores.

## API

| Método | Ruta | Qué hace |
|---|---|---|
| GET | `/api/values` | Lecturas actuales: `{connected, protocol, device, v:{ "0C": 850, ... }}` |
| GET | `/api/status` | Estado del enlace: state, intentos, reintento en ms, PIN, bitácora y escaneo |
| GET | `/api/pids` | Catálogo de PIDs: pid, nombre, unidad, decimales |
| POST | `/api/scan` | Busca adaptadores Bluetooth (4 s) y devuelve nombre, MAC y RSSI |
| POST | `/api/connect?mac=AA:BB:...` | Empareja e inicializa (`ATZ`, `ATE0`, `ATL0`, `ATS0`, `ATSP0`, `ATDPN`) |
| POST | `/api/disconnect` | Cierra el enlace y pausa la reconexión automática |
| POST | `/api/auto?on=1` | Reactiva la reconexión automática |
| POST | `/api/pin?pin=1234` | Guarda el PIN del adaptador en NVS |
| GET | `/api/dtc` | Lee códigos con `03` |
| POST | `/api/dtc/clear` | Borra códigos con `04` |

## Arranque y persistencia

- Al encender, el ESP32 lee de NVS la MAC y el PIN del último adaptador y
  reconecta solo, sin tocar el panel.
- Un hilo propio mantiene el enlace: si falla, reintenta con espera creciente
  (5, 10, 20, 40 y 60 s) y nunca bloquea el servidor web.
- Si el ELM327 deja de responder a 3 PIDs seguidos, cierra el enlace y vuelve a
  intentar.
- El escaneo corre en una tarea aparte: la web responde y muestra el avance
  mientras busca.
- `POST /api/disconnect` pausa la reconexión; `POST /api/auto?on=1` la reactiva.
- El PIN se guarda en NVS y se aplica con `setPin` antes de cada emparejamiento.

## Detalles de implementación

- Sondeo round-robin de un PID cada 45 ms con timeout de 160 ms: el panel se
  mantiene fluido aunque el adaptador tarde en responder.
- PIDs sin respuesta se publican como `null` y la interfaz muestra `--`.
- Colores: cada sensor tiene su color; fuera de rango pasa a ámbar o rojo. La
  velocidad usa franjas: 15 verde, 16-30 ámbar, 31-45 naranja, 46+ rojo.
- WiFi y clave se cambian en `src/main.cpp` (`AP_SSID`, `AP_PASS`).

## Límites

- Bluetooth Classic consume RAM: con LittleFS y el servidor web queda poco
  margen. Si necesitas TLS o WebSocket, usa un ESP32 con más PSRAM.
- El ESP32 no puede hablar con ELM327 BLE (solo Classic SPP). Los adaptadores
  "mini" y "v1.5/v2.1" son Classic; los "ELM327 BLE" no funcionan.
- Verificado solo en compilación; no se probó en un vehículo real.
