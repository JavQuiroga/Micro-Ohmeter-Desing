#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <NimBLEDevice.h>

// ===============================
// ADS1115
// ===============================

Adafruit_ADS1115 ads;

const uint8_t ADS1115_ADDR = 0x48;
adsGain_t ADS_GAIN = GAIN_ONE;

bool adsOk = false;

// ===============================
// Modo de simulacion
// ===============================

// Activar en true para validar la app solo con el ESP32 y el celular,
// sin PCB, sin etapa analogica y sin ADS1115 conectado.
const bool SIMULATION_MODE = true;

// Activar en true para enviar cada JSON en dos notificaciones BLE.
// Esto permite verificar que la app reconstruye fragmentos hasta encontrar "}".
const bool SIMULATE_FRAGMENTED_BLE = true;

const float SIM_RESISTANCE_MOHM[] = {
  0.00f,
  25.00f,
  50.00f,
  100.00f,
  250.00f,
  500.00f,
  750.00f,
  1000.00f,
  500.00f,
  100.00f
};

const int SIM_POINTS = sizeof(SIM_RESISTANCE_MOHM) / sizeof(SIM_RESISTANCE_MOHM[0]);
int simIndex = 0;

float adsCountsToVolts(int16_t counts) {
  return counts * 0.125f / 1000.0f; // 0.125 mV/count -> V
}

// ===============================
// BLE
// ===============================

static const char* BLE_DEVICE_NAME = "MicroOhmMeter-E3T";

static const char* SERVICE_UUID        = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char* CHARACTERISTIC_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pCharacteristic = nullptr;

bool deviceConnected = false;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    deviceConnected = true;
    Serial.println("Dispositivo BLE conectado");
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    Serial.println("Dispositivo BLE desconectado");
    NimBLEDevice::startAdvertising();
  }
};

// ===============================
// Pines
// ===============================

const int PIN_BATTERY_ADC = 34;
const int PIN_CHARGING    = 27;

// ===============================
// Batería
// ===============================

const float BATTERY_DIVIDER_FACTOR = 2.0f;

const float ESP32_ADC_REF = 3.3f;
const int   ESP32_ADC_MAX = 4095;

const float BATTERY_VOLT_EMPTY = 3.30f;
const float BATTERY_VOLT_FULL  = 4.20f;

// ===============================
// Calibración resistencia
// ===============================

const float CAL_OFFSET_V = 0.0191f;
const float CAL_GAIN_DIV = 2.0f;

// ===============================
// Tiempo de muestreo
// ===============================

unsigned long lastSampleMs = 0;
const unsigned long SAMPLE_PERIOD_MS = 500;

// ===============================
// Funciones auxiliares
// ===============================

float clampf(float x, float xmin, float xmax) {
  if (x < xmin) return xmin;
  if (x > xmax) return xmax;
  return x;
}

float readBatteryVoltage() {
  int raw = analogRead(PIN_BATTERY_ADC);
  float vPin = (raw * ESP32_ADC_REF) / ESP32_ADC_MAX;
  float vBat = vPin * BATTERY_DIVIDER_FACTOR;
  return vBat;
}

int batteryPercentFromVoltage(float vBat) {
  float pct = (vBat - BATTERY_VOLT_EMPTY) / (BATTERY_VOLT_FULL - BATTERY_VOLT_EMPTY);
  pct = clampf(pct, 0.0f, 1.0f);
  return (int)(pct * 100.0f + 0.5f);
}

bool readChargingStatus() {
  return digitalRead(PIN_CHARGING) == HIGH;
}

int estimateChargeTimeMin(float vBat, bool charging) {
  if (!charging) return -1;

  int pct = batteryPercentFromVoltage(vBat);
  int remainingPct = 100 - pct;

  return remainingPct * 2;
}

float computeResistanceOhms(float vOut) {
  float r = (vOut - CAL_OFFSET_V) / CAL_GAIN_DIV;

  if (r < 0.0f) {
    r = 0.0f;
  }

  return r;
}

int adcCountsFromVolts(float volts) {
  int counts = (int)((volts / 3.3f) * 4095.0f + 0.5f);
  if (counts < 0) return 0;
  if (counts > 4095) return 4095;
  return counts;
}

void buildSimulatedMeasurement(float &rMilliOhms, float &vOut, int16_t &adcCounts,
                               float &vBat, int &batteryPct, bool &charging,
                               int &chargeTimeMin) {
  rMilliOhms = SIM_RESISTANCE_MOHM[simIndex];

  float rOhms = rMilliOhms / 1000.0f;
  vOut = (CAL_GAIN_DIV * rOhms) + CAL_OFFSET_V;
  adcCounts = adcCountsFromVolts(vOut);

  vBat = 4.05f - (0.025f * simIndex);
  charging = simIndex >= 6;

  if (charging) {
    vBat = 3.82f + (0.035f * (simIndex - 6));
  }

  batteryPct = batteryPercentFromVoltage(vBat);
  chargeTimeMin = estimateChargeTimeMin(vBat, charging);

  simIndex++;
  if (simIndex >= SIM_POINTS) {
    simIndex = 0;
  }
}

String buildPayload(float rMilliOhms, float vOut, int16_t adcCounts,
                    float vBat, int batteryPct, bool charging, int chargeTimeMin) {
  String payload = "{";
  payload += "\"resistance_mohm\":" + String(rMilliOhms, 2) + ",";
  payload += "\"adc_voltage\":" + String(vOut, 4) + ",";
  payload += "\"adc_counts\":" + String(adcCounts) + ",";
  payload += "\"battery_voltage\":" + String(vBat, 3) + ",";
  payload += "\"battery_percent\":" + String(batteryPct) + ",";
  payload += "\"charging_status\":" + String(charging ? 1 : 0) + ",";
  payload += "\"charge_time_min\":" + String(chargeTimeMin);
  payload += "}";
  return payload;
}

void notifyPayload(String payload) {
  if (!deviceConnected || pCharacteristic == nullptr) {
    return;
  }

  if (SIMULATION_MODE && SIMULATE_FRAGMENTED_BLE && payload.length() > 20) {
    int splitIndex = payload.length() / 2;
    String firstPart = payload.substring(0, splitIndex);
    String secondPart = payload.substring(splitIndex);

    pCharacteristic->setValue(firstPart.c_str());
    pCharacteristic->notify();
    delay(25);
    pCharacteristic->setValue(secondPart.c_str());
    pCharacteristic->notify();
    return;
  }

  pCharacteristic->setValue(payload.c_str());
  pCharacteristic->notify();
}

// ===============================
// Configuración BLE
// ===============================

void setupBLE() {
  NimBLEDevice::init(BLE_DEVICE_NAME);
  NimBLEDevice::setMTU(256);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService* pService = pServer->createService(SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  pCharacteristic->setValue("{\"status\":\"boot\"}");

  pService->start();

  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->enableScanResponse(true);
  pAdvertising->setName(BLE_DEVICE_NAME);
  pAdvertising->start();

  Serial.println("BLE iniciado");
  Serial.println("Nombre BLE: MicroOhmMeter-E3T");
}

// ===============================
// Setup
// ===============================

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_CHARGING, INPUT);

  analogReadResolution(12);

  Wire.begin();

  setupBLE();

  if (!ads.begin(ADS1115_ADDR)) {
    Serial.println("Advertencia: no se detectó ADS1115");
    Serial.println("El BLE seguirá funcionando con valores de prueba");
    adsOk = false;
  } else {
    adsOk = true;
    ads.setGain(ADS_GAIN);
    Serial.println("ADS1115 detectado correctamente");
  }

  Serial.println("Sistema iniciado");
  if (SIMULATION_MODE) {
    Serial.println("Modo simulacion activo: enviando secuencia de datos BLE sin circuito analogico");
  }
}

// ===============================
// Loop principal
// ===============================

void loop() {
  unsigned long now = millis();

  if (now - lastSampleMs >= SAMPLE_PERIOD_MS) {
    lastSampleMs = now;

    int16_t adcCounts = 0;
    float vOut = 0.0f;
    float rMilliOhms = 0.0f;
    float vBat = 0.0f;
    int batteryPct = 0;
    bool charging = false;
    int chargeTimeMin = -1;

    if (SIMULATION_MODE) {
      buildSimulatedMeasurement(rMilliOhms, vOut, adcCounts, vBat, batteryPct, charging, chargeTimeMin);
    } else {
      if (adsOk) {
        adcCounts = ads.readADC_SingleEnded(0);
      } else {
        adcCounts = 0;
      }

      vOut = adsCountsToVolts(adcCounts);

      float rOhms = computeResistanceOhms(vOut);
      rMilliOhms = rOhms * 1000.0f;

      vBat = readBatteryVoltage();
      batteryPct = batteryPercentFromVoltage(vBat);

      charging = readChargingStatus();
      chargeTimeMin = estimateChargeTimeMin(vBat, charging);
    }

    String payload = buildPayload(rMilliOhms, vOut, adcCounts, vBat, batteryPct, charging, chargeTimeMin);

    Serial.println(payload);

    notifyPayload(payload);
  }
}
