# Micro Ohm Meter App

App Android con React Native, Expo Dev Build y `react-native-ble-plx` para conectarse al ESP32 `MicroOhmMeter-E3T`.

## Ejecutar en desarrollo

```bash
npm install
npx expo install expo-dev-client
npx expo start --dev-client
```

Instala primero un dev build en el telefono:

```bash
npm install --global eas-cli
eas login
eas build:configure
eas build --platform android --profile development
```

Cuando EAS termine, descarga e instala el APK generado en el dispositivo Android. Despues abre la app y ejecuta:

```bash
npx expo start --dev-client
```

## Compilar APK instalable

El perfil `preview` de `eas.json` genera un APK:

```bash
eas build --platform android --profile preview
```

Tambien puedes usar el script:

```bash
npm run build:apk
```

## BLE configurado

- Nombre del dispositivo: `MicroOhmMeter-E3T`
- Servicio: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- Caracteristica notify: `6e400003-b5a3-f393-e0a9-e50e24dcca9e`

La app acumula fragmentos BLE hasta encontrar `}`, decodifica base64, parsea el JSON y muestra las mediciones en pantalla.

## Experimento sin PCB

El firmware incluye `SIMULATION_MODE = true` para probar la app usando solo el ESP32 y un celular Android. En este modo el ESP32 no depende del circuito analogico ni del ADS1115: envia una secuencia de resistencias simuladas entre 0 mOhm y 1000 mOhm, calcula un voltaje ADC coherente con la ecuacion de calibracion y simula variables de bateria.

Tambien se dejo `SIMULATE_FRAGMENTED_BLE = true`, que divide cada JSON en dos notificaciones BLE. Esto permite comprobar que la app reconstruye correctamente los fragmentos antes de parsear el JSON.

Pasos:

```text
1. Cargar CodigoESP/CodigoESP.ino en el ESP32.
2. Encender Bluetooth y ubicacion en el celular Android.
3. Instalar y abrir MicroOhmMeter-preview.apk.
4. Aceptar permisos.
5. Presionar Buscar ESP32.
6. Verificar que la app se conecte a MicroOhmMeter-E3T.
7. Observar la secuencia simulada de mediciones en las tarjetas.
```

Para volver a usar el hardware real, cambiar `SIMULATION_MODE` a `false`.
