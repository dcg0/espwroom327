#!/usr/bin/env bash
# DC ECU · instalador para Linux
#
# Instala PlatformIO, compila el firmware, lo flashea en el ESP32 WROOM
# conectado por USB y sube el panel web (LittleFS). Al terminar te deja la
# placa sirviendo el panel en http://192.168.4.1.
#
# Uso:
#   ./instalar.sh                 detecta el puerto automáticamente
#   ./instalar.sh --port /dev/ttyUSB0
#   ./instalar.sh --monitor       abre el monitor serie al terminar
#   ./instalar.sh --sin-flash     solo instala PlatformIO y compila
#   ./instalar.sh --help

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="${HOME}/.local/share/dc-ecu/pio-venv"
PORT=""
DO_FLASH=1
DO_MONITOR=0
BAUD=115200

if [ -t 1 ]; then
  C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_ERR=$'\033[31m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
else
  C_OK=""; C_WARN=""; C_ERR=""; C_DIM=""; C_OFF=""
fi

ok()   { printf '%s✓%s %s\n' "$C_OK" "$C_OFF" "$*"; }
warn() { printf '%s!%s %s\n' "$C_WARN" "$C_OFF" "$*"; }
fail() { printf '%s✗%s %s\n' "$C_ERR" "$C_OFF" "$*" >&2; exit 1; }
step() { printf '\n%s==>%s %s\n' "$C_DIM" "$C_OFF" "$*"; }

usage() {
  awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "${BASH_SOURCE[0]}"
  exit 0
}

while [ $# -gt 0 ]; do
  case "$1" in
    --port|-p) PORT="${2:-}"; [ -n "$PORT" ] || fail "Falta el valor de --port"; shift 2 ;;
    --monitor|-m) DO_MONITOR=1; shift ;;
    --sin-flash|--no-flash) DO_FLASH=0; shift ;;
    --help|-h) usage ;;
    *) fail "Opción desconocida: $1 (usa --help)" ;;
  esac
done

step "Revisando el sistema"
command -v python3 >/dev/null 2>&1 || fail "Falta python3. Instálalo: sudo apt install python3 python3-venv"
PY_OK="$(python3 -c 'import sys; print("1" if sys.version_info >= (3, 8) else "0")')"
[ "$PY_OK" = "1" ] || fail "Se necesita Python 3.8 o superior."
ok "python3 $(python3 -c 'import platform; print(platform.python_version())')"

[ -f "${SCRIPT_DIR}/platformio.ini" ] || fail "No encuentro platformio.ini. Ejecuta el script dentro de la carpeta esp32-elm327."
[ -f "${SCRIPT_DIR}/src/main.cpp" ] || fail "Falta src/main.cpp."
[ -f "${SCRIPT_DIR}/data/index.html" ] || fail "Falta data/index.html (el panel web)."
ok "Proyecto completo en ${SCRIPT_DIR}"

step "Instalando PlatformIO"
PIO=""
if command -v pio >/dev/null 2>&1; then
  PIO="$(command -v pio)"
  ok "PlatformIO ya instalado: $($PIO --version 2>/dev/null || echo pio)"
elif [ -x "${VENV_DIR}/bin/pio" ]; then
  PIO="${VENV_DIR}/bin/pio"
  ok "Usando PlatformIO del entorno anterior"
else
  python3 -m venv "$VENV_DIR" 2>/dev/null || fail "No se pudo crear el entorno virtual. En Debian/Ubuntu: sudo apt install python3-venv"
  "${VENV_DIR}/bin/pip" install --quiet --upgrade pip
  "${VENV_DIR}/bin/pip" install --quiet platformio
  PIO="${VENV_DIR}/bin/pio"
  ok "PlatformIO instalado en $VENV_DIR"
fi

step "Buscando el ESP32 por USB"
find_ports() {
  ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true
}

if [ -z "$PORT" ]; then
  PORTS="$(find_ports)"
  COUNT="$(printf '%s\n' "$PORTS" | grep -c . || true)"
  if [ "$COUNT" -eq 0 ]; then
    fail "No hay placa conectada. Revisa el cable USB (debe ser de datos), el driver CH340/CP210x y vuelve a intentar.
   Si ya sabes el puerto: ./instalar.sh --port /dev/ttyUSB0"
  elif [ "$COUNT" -eq 1 ]; then
    PORT="$PORTS"
  else
    warn "Hay varios puertos serie:"
    printf '   %s\n' $PORTS
    printf 'Elige uno y ejecuta: ./instalar.sh --port /dev/ttyUSB0\n'
    exit 1
  fi
fi

[ -e "$PORT" ] || fail "El puerto $PORT no existe."
[ -r "$PORT" ] && [ -w "$PORT" ] || fail "Sin permisos sobre $PORT. Ejecuta:
   sudo usermod -aG dialout \$USER
   y vuelve a entrar a la sesión (o reinicia)."
ok "Placa en $PORT"

step "Compilando el firmware"
(cd "$SCRIPT_DIR" && "$PIO" run)

if [ "$DO_FLASH" -eq 1 ]; then
  step "Flasheando el firmware en el ESP32"
  (cd "$SCRIPT_DIR" && "$PIO" run -t upload --upload-port "$PORT")

  step "Subiendo el panel web (LittleFS)"
  (cd "$SCRIPT_DIR" && "$PIO" run -t uploadfs --upload-port "$PORT")
else
  warn "Compilado sin flashear (--sin-flash)."
fi

printf '\n%s========================================%s\n' "$C_OK" "$C_OFF"
ok "Listo. La placa ya sirve el panel."
printf '  1. Conéctate a la WiFi %sBuss-PANEL%s (clave %sJuanita123%s)\n' "$C_OK" "$C_OFF" "$C_OK" "$C_OFF"
printf '  2. Abre %shttp://192.168.4.1%s en el navegador\n' "$C_OK" "$C_OFF"
printf '  3. Pestaña AJUSTES -> Escanear adaptadores -> elige tu ELM327 (PIN 1234)\n'
printf '  4. El ELM327 debe ser Bluetooth Classic, no BLE. Contacto del auto puesto.\n'
printf '%s========================================%s\n' "$C_OK" "$C_OFF"

if [ "$DO_MONITOR" -eq 1 ]; then
  step "Monitor serie (Ctrl+C para salir)"
  (cd "$SCRIPT_DIR" && "$PIO" device monitor --port "$PORT" --baud "$BAUD")
fi
