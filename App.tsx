import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  ActivityIndicator,
  Alert,
  PermissionsAndroid,
  Platform,
  Pressable,
  SafeAreaView,
  ScrollView,
  StatusBar,
  StyleSheet,
  Text,
  View,
} from 'react-native';
import { decode as decodeBase64 } from 'base-64';
import { BleError, BleManager, Device, Subscription } from 'react-native-ble-plx';

const DEVICE_NAME = 'MicroOhmMeter-E3T';
const SERVICE_UUID = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const CHARACTERISTIC_UUID = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';

type MeterReading = {
  resistance_mohm: number;
  adc_voltage: number;
  adc_counts: number;
  battery_voltage: number;
  battery_percent: number;
  charging_status: number;
  charge_time_min: number;
};

type ConnectionState =
  | 'Desconectado'
  | 'Pidiendo permisos'
  | 'Buscando ESP32'
  | 'Conectando'
  | 'Suscrito'
  | 'Error';

const emptyReading: MeterReading = {
  resistance_mohm: 0,
  adc_voltage: 0,
  adc_counts: 0,
  battery_voltage: 0,
  battery_percent: 0,
  charging_status: 0,
  charge_time_min: -1,
};

export default function App() {
  const manager = useMemo(() => new BleManager(), []);
  const [connectionState, setConnectionState] = useState<ConnectionState>('Desconectado');
  const [statusMessage, setStatusMessage] = useState('Listo para buscar el ESP32.');
  const [isScanning, setIsScanning] = useState(false);
  const [device, setDevice] = useState<Device | null>(null);
  const [reading, setReading] = useState<MeterReading>(emptyReading);
  const [lastPacketAt, setLastPacketAt] = useState<string>('Sin datos');

  const notificationSubRef = useRef<Subscription | null>(null);
  const disconnectSubRef = useRef<Subscription | null>(null);
  const jsonBufferRef = useRef('');
  const isConnectingRef = useRef(false);

  useEffect(() => {
    return () => {
      cleanupSubscriptions();
      manager.stopDeviceScan();
      manager.destroy();
    };
  }, [manager]);

  const cleanupSubscriptions = () => {
    notificationSubRef.current?.remove();
    disconnectSubRef.current?.remove();
    notificationSubRef.current = null;
    disconnectSubRef.current = null;
  };

  const requestBluetoothPermissions = async () => {
    if (Platform.OS !== 'android') {
      return true;
    }

    setConnectionState('Pidiendo permisos');

    const permissions = [
      PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION,
      PermissionsAndroid.PERMISSIONS.ACCESS_COARSE_LOCATION,
    ];

    if (Platform.Version >= 31) {
      permissions.push(
        PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
        PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
      );
    }

    const result = await PermissionsAndroid.requestMultiple(permissions);
    const allGranted = permissions.every(
      permission => result[permission] === PermissionsAndroid.RESULTS.GRANTED,
    );

    if (!allGranted) {
      setConnectionState('Error');
      setStatusMessage('Permisos denegados. Activa Bluetooth, ubicacion y dispositivos cercanos.');
      Alert.alert(
        'Permisos requeridos',
        'Para escanear el ESP32 se necesitan permisos de Bluetooth, ubicacion y dispositivos cercanos.',
      );
    }

    return allGranted;
  };

  const parseIncomingText = useCallback((text: string) => {
    jsonBufferRef.current += text;

    while (jsonBufferRef.current.includes('}')) {
      const startIndex = jsonBufferRef.current.indexOf('{');
      const endIndex = jsonBufferRef.current.indexOf('}', startIndex);

      if (startIndex < 0 || endIndex < 0) {
        jsonBufferRef.current = '';
        return;
      }

      const jsonText = jsonBufferRef.current.slice(startIndex, endIndex + 1);
      jsonBufferRef.current = jsonBufferRef.current.slice(endIndex + 1);

      try {
        const parsed = JSON.parse(jsonText) as MeterReading;
        setReading(parsed);
        setLastPacketAt(new Date().toLocaleTimeString());
        setStatusMessage('Recibiendo mediciones del ESP32.');
      } catch {
        setStatusMessage('Se recibio un fragmento BLE incompleto o invalido.');
      }
    }
  }, []);

  const subscribeToMeasurements = useCallback(
    (connectedDevice: Device) => {
      notificationSubRef.current = connectedDevice.monitorCharacteristicForService(
        SERVICE_UUID,
        CHARACTERISTIC_UUID,
        (error: BleError | null, characteristic) => {
          if (error) {
            setConnectionState('Error');
            setStatusMessage(`Error en notificacion BLE: ${error.message}`);
            return;
          }

          const value = characteristic?.value;
          if (!value) {
            return;
          }

          const decodedText = decodeBase64(value);
          parseIncomingText(decodedText);
        },
      );
    },
    [parseIncomingText],
  );

  const connectToDevice = useCallback(
    async (foundDevice: Device) => {
      if (isConnectingRef.current) {
        return;
      }

      isConnectingRef.current = true;
      setConnectionState('Conectando');
      setStatusMessage(`Conectando a ${DEVICE_NAME}...`);

      try {
        const connected = await foundDevice.connect({ timeout: 10000 });
        if (Platform.OS === 'android') {
          await connected.requestMTU(256).catch(() => connected);
        }
        const readyDevice = await connected.discoverAllServicesAndCharacteristics();

        cleanupSubscriptions();
        setDevice(readyDevice);
        setConnectionState('Suscrito');
        setStatusMessage('Conectado. Esperando mediciones...');
        jsonBufferRef.current = '';

        disconnectSubRef.current = manager.onDeviceDisconnected(readyDevice.id, () => {
          cleanupSubscriptions();
          setDevice(null);
          setConnectionState('Desconectado');
          setStatusMessage('El ESP32 se desconecto.');
        });

        subscribeToMeasurements(readyDevice);
      } catch (error) {
        const message = error instanceof Error ? error.message : 'No se pudo conectar al ESP32.';
        setConnectionState('Error');
        setStatusMessage(message);
      } finally {
        isConnectingRef.current = false;
      }
    },
    [manager, subscribeToMeasurements],
  );

  const scanForDevice = useCallback(async () => {
    const hasPermissions = await requestBluetoothPermissions();
    if (!hasPermissions) {
      return;
    }

    const bluetoothState = await manager.state();
    if (bluetoothState !== 'PoweredOn') {
      setConnectionState('Error');
      setStatusMessage('Bluetooth esta apagado. Enciendelo e intenta de nuevo.');
      return;
    }

    cleanupSubscriptions();
    jsonBufferRef.current = '';
    setIsScanning(true);
    setDevice(null);
    setConnectionState('Buscando ESP32');
    setStatusMessage(`Escaneando dispositivos BLE hasta encontrar ${DEVICE_NAME}...`);

    manager.startDeviceScan(null, { allowDuplicates: false }, (error, scannedDevice) => {
      if (error) {
        manager.stopDeviceScan();
        setIsScanning(false);
        setConnectionState('Error');
        setStatusMessage(`Error escaneando BLE: ${error.message}`);
        return;
      }

      const scannedName = scannedDevice?.name ?? scannedDevice?.localName;
      if (scannedDevice && scannedName === DEVICE_NAME) {
        manager.stopDeviceScan();
        setIsScanning(false);
        connectToDevice(scannedDevice);
      }
    });
  }, [connectToDevice, manager]);

  const disconnect = useCallback(async () => {
    manager.stopDeviceScan();
    setIsScanning(false);
    cleanupSubscriptions();
    jsonBufferRef.current = '';

    if (device) {
      try {
        await manager.cancelDeviceConnection(device.id);
      } catch {
        // The device may already be disconnected.
      }
    }

    setDevice(null);
    setConnectionState('Desconectado');
    setStatusMessage('Desconectado. Puedes buscar el ESP32 otra vez.');
  }, [device, manager]);

  const isConnected = connectionState === 'Suscrito';
  const isBusy = isScanning || connectionState === 'Conectando' || connectionState === 'Pidiendo permisos';

  return (
    <SafeAreaView style={styles.safeArea}>
      <StatusBar barStyle="dark-content" backgroundColor="#f4f7fb" />
      <ScrollView contentContainerStyle={styles.container}>
        <View style={styles.header}>
          <Text style={styles.title}>Micro Ohm Meter</Text>
          <View style={[styles.statusPill, isConnected ? styles.statusConnected : styles.statusIdle]}>
            <View style={[styles.statusDot, isConnected ? styles.dotConnected : styles.dotIdle]} />
            <Text style={styles.statusText}>{connectionState}</Text>
          </View>
        </View>

        <Text style={styles.subtitle}>{statusMessage}</Text>

        <View style={styles.actions}>
          <Pressable
            disabled={isBusy}
            onPress={scanForDevice}
            style={({ pressed }) => [
              styles.button,
              styles.primaryButton,
              (pressed || isBusy) && styles.buttonPressed,
            ]}
          >
            {isBusy ? <ActivityIndicator color="#ffffff" /> : <Text style={styles.primaryButtonText}>Buscar ESP32</Text>}
          </Pressable>

          <Pressable
            disabled={!device && !isScanning}
            onPress={disconnect}
            style={({ pressed }) => [
              styles.button,
              styles.secondaryButton,
              (pressed || (!device && !isScanning)) && styles.secondaryButtonDisabled,
            ]}
          >
            <Text style={styles.secondaryButtonText}>Desconectar</Text>
          </Pressable>
        </View>

        <View style={styles.section}>
          <MetricCard label="Resistencia" value={reading.resistance_mohm.toFixed(2)} unit="mΩ" featured />
          <MetricCard label="Voltaje ADC" value={reading.adc_voltage.toFixed(4)} unit="V" />
          <MetricCard label="Cuentas ADC" value={String(reading.adc_counts)} unit="counts" />
          <MetricCard label="Voltaje bateria" value={reading.battery_voltage.toFixed(3)} unit="V" />
          <MetricCard label="Porcentaje bateria" value={String(reading.battery_percent)} unit="%" />
          <MetricCard label="Estado de carga" value={reading.charging_status === 1 ? 'Si' : 'No'} unit="" />
          <MetricCard
            label="Tiempo restante"
            value={reading.charge_time_min >= 0 ? String(reading.charge_time_min) : '--'}
            unit="min"
          />
        </View>

        <View style={styles.footerCard}>
          <Text style={styles.footerLabel}>Ultimo paquete</Text>
          <Text style={styles.footerValue}>{lastPacketAt}</Text>
          <Text style={styles.footerLabel}>Dispositivo</Text>
          <Text style={styles.footerValue}>{device?.name ?? DEVICE_NAME}</Text>
        </View>
      </ScrollView>
    </SafeAreaView>
  );
}

function MetricCard({
  label,
  value,
  unit,
  featured = false,
}: {
  label: string;
  value: string;
  unit: string;
  featured?: boolean;
}) {
  return (
    <View style={[styles.card, featured && styles.featuredCard]}>
      <Text style={[styles.cardLabel, featured && styles.featuredLabel]}>{label}</Text>
      <View style={styles.metricRow}>
        <Text style={[styles.metricValue, featured && styles.featuredValue]}>{value}</Text>
        {!!unit && <Text style={[styles.metricUnit, featured && styles.featuredUnit]}>{unit}</Text>}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  safeArea: {
    flex: 1,
    backgroundColor: '#f4f7fb',
  },
  container: {
    padding: 20,
    paddingBottom: 34,
  },
  header: {
    alignItems: 'flex-start',
    flexDirection: 'row',
    justifyContent: 'space-between',
    gap: 14,
  },
  title: {
    color: '#14213d',
    flex: 1,
    fontSize: 30,
    fontWeight: '800',
  },
  subtitle: {
    color: '#536176',
    fontSize: 15,
    lineHeight: 22,
    marginTop: 10,
  },
  statusPill: {
    alignItems: 'center',
    borderRadius: 999,
    flexDirection: 'row',
    gap: 8,
    paddingHorizontal: 12,
    paddingVertical: 8,
  },
  statusConnected: {
    backgroundColor: '#dff7e8',
  },
  statusIdle: {
    backgroundColor: '#e8edf5',
  },
  statusDot: {
    borderRadius: 5,
    height: 10,
    width: 10,
  },
  dotConnected: {
    backgroundColor: '#1d9a57',
  },
  dotIdle: {
    backgroundColor: '#718096',
  },
  statusText: {
    color: '#24324a',
    fontSize: 13,
    fontWeight: '700',
  },
  actions: {
    flexDirection: 'row',
    gap: 12,
    marginTop: 22,
  },
  button: {
    alignItems: 'center',
    borderRadius: 8,
    flex: 1,
    justifyContent: 'center',
    minHeight: 50,
    paddingHorizontal: 14,
  },
  primaryButton: {
    backgroundColor: '#1769e0',
  },
  buttonPressed: {
    opacity: 0.72,
  },
  primaryButtonText: {
    color: '#ffffff',
    fontSize: 16,
    fontWeight: '800',
  },
  secondaryButton: {
    backgroundColor: '#ffffff',
    borderColor: '#cad3df',
    borderWidth: 1,
  },
  secondaryButtonDisabled: {
    opacity: 0.48,
  },
  secondaryButtonText: {
    color: '#233149',
    fontSize: 16,
    fontWeight: '800',
  },
  section: {
    gap: 12,
    marginTop: 24,
  },
  card: {
    backgroundColor: '#ffffff',
    borderColor: '#e1e7ef',
    borderRadius: 8,
    borderWidth: 1,
    padding: 18,
  },
  featuredCard: {
    backgroundColor: '#14213d',
    borderColor: '#14213d',
  },
  cardLabel: {
    color: '#66758a',
    fontSize: 14,
    fontWeight: '700',
  },
  featuredLabel: {
    color: '#b9c7dc',
  },
  metricRow: {
    alignItems: 'flex-end',
    flexDirection: 'row',
    gap: 8,
    marginTop: 8,
  },
  metricValue: {
    color: '#172033',
    fontSize: 28,
    fontWeight: '800',
  },
  featuredValue: {
    color: '#ffffff',
    fontSize: 42,
  },
  metricUnit: {
    color: '#66758a',
    fontSize: 16,
    fontWeight: '700',
    marginBottom: 4,
  },
  featuredUnit: {
    color: '#dbe7f7',
    marginBottom: 8,
  },
  footerCard: {
    backgroundColor: '#ffffff',
    borderColor: '#e1e7ef',
    borderRadius: 8,
    borderWidth: 1,
    gap: 4,
    marginTop: 18,
    padding: 16,
  },
  footerLabel: {
    color: '#7a8798',
    fontSize: 12,
    fontWeight: '800',
    marginTop: 4,
    textTransform: 'uppercase',
  },
  footerValue: {
    color: '#24324a',
    fontSize: 15,
    fontWeight: '700',
  },
});
