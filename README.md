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
