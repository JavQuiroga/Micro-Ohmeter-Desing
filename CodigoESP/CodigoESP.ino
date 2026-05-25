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
}

// ===============================
// Loop principal
// ===============================

void loop() {
  unsigned long now = millis();

  if (now - lastSampleMs >= SAMPLE_PERIOD_MS) {
    lastSampleMs = now;

    int16_t adcCounts = 0;

    if (adsOk) {
      adcCounts = ads.readADC_SingleEnded(0);
    } else {
      adcCounts = 0;
    }

    float vOut = adsCountsToVolts(adcCounts);

    float rOhms = computeResistanceOhms(vOut);
    float rMilliOhms = rOhms * 1000.0f;

    float vBat = readBatteryVoltage();
    int batteryPct = batteryPercentFromVoltage(vBat);

    bool charging = readChargingStatus();
    int chargeTimeMin = estimateChargeTimeMin(vBat, charging);

    String payload = "{";
    payload += "\"resistance_mohm\":" + String(rMilliOhms, 2) + ",";
    payload += "\"adc_voltage\":" + String(vOut, 4) + ",";
    payload += "\"adc_counts\":" + String(adcCounts) + ",";
    payload += "\"battery_voltage\":" + String(vBat, 3) + ",";
    payload += "\"battery_percent\":" + String(batteryPct) + ",";
    payload += "\"charging_status\":" + String(charging ? 1 : 0) + ",";
    payload += "\"charge_time_min\":" + String(chargeTimeMin);
    payload += "}";

    Serial.println(payload);

    if (deviceConnected && pCharacteristic != nullptr) {
      pCharacteristic->setValue(payload.c_str());
      pCharacteristic->notify();
    }
  }
}