# Política de seguridad

## Alcance

Esta política cubre el firmware ESP32, el panel web servido desde LittleFS, el instalador y la configuración de PlatformIO.

## Reportar una vulnerabilidad

No publiques contraseñas Wi-Fi, PIN, tokens, datos del vehículo ni detalles de explotación en issues o pull requests. Utiliza **Report a vulnerability** en la pestaña **Security** de GitHub para enviar un aviso privado. Si esa opción no está disponible, solicita primero un canal privado al mantenedor.

Incluye commit o versión, componente, pasos mínimos de reproducción, impacto y mitigación sugerida. Redacta MAC, VIN, matrículas, registros y cualquier dato personal.

## Notas de seguridad

Cada ESP32 genera su propia clave Wi-Fi inicial a partir de su identidad eFuse; el repositorio no contiene una contraseña compartida. La clave se muestra solo por el monitor serie local durante el arranque. El panel HTTP está pensado para la red de configuración local del dispositivo, no para exponerse a Internet.
