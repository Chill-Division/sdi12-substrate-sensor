/*
 * SDI-12 Sensor with MQTT Publishing for Home Assistant
 * PoESP32 Version with Ethernet
 * 
 * Features:
 * - Reads SDI-12 sensor data (VWC, Temperature, EC)
 * - Calculates EC using two methods:
 *   1. Simple division (/500) for pW
 *   2. Epsilon calculation from manufacturer's formula
 * - Publishes to MQTT with Home Assistant auto-discovery
 * - Debug output with formatted tables
 * 
 * Calibration Formulas:
 * 1. VWC (Volumetric Water Content):
 *    VWC = 6.771×10^-10 × RAW³ - 5.105×10^-6 × RAW² + 1.302×10^-2 × RAW - 10.848
 *    Where: VWC is 0-100%, RAW is calibrated ADC reading
 * 
 * 2. EC Calculations:
 *    ppm500: EC_pW = EC_raw / 500
 *    Epsilon: ε = (2.887×10^-9 × RAW³ - 2.080×10^-5 × RAW² + 5.276×10^-2 × RAW - 43.39)²
 *    Where: ε is dielectric permittivity, RAW is calibrated ADC reading
 */

#include <ETH.h>
#include <SDI12.h>
#include <PubSubClient.h>

// Debug Configuration
#define DEBUG true
#define MQTT_MAX_PACKET_SIZE 1024

// Device Configuration
#define HOSTNAME_PREFIX "sdi12sensor-"
#define HOSTNAME_SUFFIX "greenhouse1"
#define SDI12_DATA_PIN 16 // This is the yellow PIN on the PoESP32 Grove Port

// Ethernet Configuration
#define ETH_CLK_MODE    ETH_CLOCK_GPIO0_IN
#define ETH_POWER_PIN   5
#define ETH_TYPE        ETH_PHY_IP101
#define ETH_ADDR        1
#define ETH_MDC_PIN     23
#define ETH_MDIO_PIN    18

// Calibration Configuration
#define EC_SIMPLE_FACTOR 500.0

// MQTT Configuration
const char* mqtt_server = "YOUR_MQTT_SERVER";
const int mqtt_port = 1883;
const char* mqtt_username = "YOUR_MQTT_USERNAME";
const char* mqtt_password = "YOUR_MQTT_PASSWORD";
const char* hostname = HOSTNAME_PREFIX HOSTNAME_SUFFIX;
const char* client_id = hostname;
const char* discovery_prefix = "homeassistant";
const char* availability_topic = "sensors/sdi12sensor/status";

// Timing Configuration
const unsigned long MQTT_PUBLISH_INTERVAL = 10000;
unsigned long lastPublishTime = 0;
unsigned long readingNumber = 0;

// Data structure for sensor readings
struct SensorData {
  float vwc_raw;
  float vwc_calibrated;
  float temperature;
  float ec_raw;
  float ec_simple;
  float ec_epsilon;
  bool valid;
  unsigned long timestamp;
};

// Initialize objects
SDI12 sdi12(SDI12_DATA_PIN);
EthernetClient ethClient;
PubSubClient mqtt(ethClient);

// Function declarations
void debugPrint(String message, bool forceOutput);
void debugTable(const SensorData& data);
String getStateTopic();
bool mqtt_publish(const char* topic, const char* payload, bool retained);
void setupEthernet();
void publishDiscoveryConfigs();
void reconnectMQTT();
float calculateVWCNonSoil(float raw);
float calculateECSimple(float raw_ec);
float calculateECEpsilon(float raw);
SensorData readSensor();
void publishData(const SensorData& data);

// Utility function for formatted debug output
void debugPrint(String message, bool forceOutput = false) {
  if (DEBUG || forceOutput) {
    Serial.println("[DEBUG] " + message);
  }
}

// Format time string
String formatTime(unsigned long ms) {
  unsigned long seconds = ms / 1000;
  unsigned long minutes = seconds / 60;
  seconds = seconds % 60;
  char timeStr[10];
  sprintf(timeStr, "%02lu:%02lu", minutes, seconds);
  return String(timeStr);
}

// Format table output for debug
void debugTable(const SensorData& data) {
  if (!DEBUG) return;

  Serial.println("\n─────────────────────────────────────");
  Serial.printf("Time: %s  Reading #%lu\n", formatTime(data.timestamp).c_str(), readingNumber);
  Serial.println("─────────────────────────────────────");
  Serial.println("├─ VWC");
  Serial.printf("│  ├─ Raw: %.2f\n", data.vwc_raw);
  Serial.printf("│  └─ Calibrated: %.2f%%\n", data.vwc_calibrated);
  Serial.printf("├─ Temperature: %.2f°C\n", data.temperature);
  Serial.println("└─ EC");
  Serial.printf("   ├─ Raw: %.2f µS/cm\n", data.ec_raw);
  Serial.printf("   ├─ ppm500 Cal: %.2f pW EC\n", data.ec_simple);
  Serial.printf("   └─ Epsilon Cal: %.2f pW EC\n", data.ec_epsilon);
  Serial.println("─────────────────────────────────────\n");
}

void EthEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      debugPrint("ETH Started");
      ETH.setHostname(hostname);
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      debugPrint("ETH Connected");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      debugPrint("ETH MAC: " + String(ETH.macAddress()));
      debugPrint("ETH IPv4: " + ETH.localIP().toString());
      debugPrint("Speed: " + String(ETH.linkSpeed()) + " Mbps");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      debugPrint("ETH Disconnected");
      break;
    case ARDUINO_EVENT_ETH_STOP:
      debugPrint("ETH Stopped");
      break;
    default:
      break;
  }
}

String getStateTopic() {
  return String("sensors/sdi12sensor/") + hostname + "/state";
}

float calculateVWCNonSoil(float raw) {
  float result = (6.771e-10 * pow(raw, 3) - 
                 5.105e-6 * pow(raw, 2) + 
                 1.302e-2 * raw - 
                 10.848) * 100;
  return constrain(result, 0, 100);
}

float calculateECSimple(float raw_ec) {
  return raw_ec / EC_SIMPLE_FACTOR;
}

float calculateECEpsilon(float raw) {
  float epsilon = 2.887e-9 * pow(raw, 3) - 
                 2.080e-5 * pow(raw, 2) + 
                 5.276e-2 * raw - 
                 43.39;
  return pow(epsilon, 2);
}

void setup() {
  Serial.begin(115200);
  while (!Serial);

  Serial.println("\nSDI-12 Sensor Starting Up");
  Serial.println("Debug output is " + String(DEBUG ? "enabled" : "disabled"));
  
  // Initialize SDI-12
  sdi12.begin();
  debugPrint("SDI-12 Sensor Initialized", true);
  debugPrint("Device hostname: " + String(hostname), true);

  // Register ethernet event handler and initialize
  WiFi.onEvent(EthEvent);
  ETH.begin(ETH_ADDR, ETH_POWER_PIN, ETH_MDC_PIN, ETH_MDIO_PIN, ETH_TYPE, ETH_CLK_MODE);
  
  while (!ETH.localIP()) {
    debugPrint("Waiting for IP address...");
    delay(1000);
  }

  // Configure MQTT
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setBufferSize(1024);
  
  debugPrint("Setup complete");
}

void publishDiscoveryConfigs() {
  debugPrint("Publishing discovery configurations...");
  
  String state_topic = getStateTopic();
  String device_config = "{\"identifiers\":[\"" + String(hostname) + "\"],"
                        "\"name\":\"" + String(hostname) + "\","
                        "\"model\":\"SDI-12 substrate Sensor\","
                        "\"manufacturer\":\"Chill Division\"}";

  struct SensorConfig {
    const char* name;
    const char* id;
    const char* device_class;
    const char* unit;
    const char* value_template;
  };

  SensorConfig configs[] = {
    {"SDI12 VWC", "vwc_calibrated", "moisture", "%", "{{ value_json.vwc_calibrated | float }}"},
    {"SDI12 Raw VWC", "vwc_raw", "", "RAW", "{{ value_json.vwc_raw | float }}"},
    {"SDI12 Temperature", "temperature", "temperature", "°C", "{{ value_json.temperature | float }}"},
    {"SDI12 Raw EC", "ec_raw", "", "µS/cm", "{{ value_json.ec_raw | float }}"},
    {"SDI12 EC ppm500", "ec_simple", "", "pW EC", "{{ value_json.ec_simple | float }}"},
    {"SDI12 EC Epsilon", "ec_epsilon", "", "pW EC", "{{ value_json.ec_epsilon | float }}"}
  };

  for (const auto& sensor : configs) {
    String config_topic = String(discovery_prefix) + "/sensor/" + hostname + "/" + sensor.id + "/config";
    
    String config = "{\"name\":\"" + String(sensor.name) + "\","
                    "\"object_id\":\"" + String(hostname) + "_" + sensor.id + "\","
                    "\"unique_id\":\"" + String(hostname) + "_" + sensor.id + "\"";
    
    if (strlen(sensor.device_class) > 0) {
      config += ",\"device_class\":\"" + String(sensor.device_class) + "\"";
    }
    
    config += ",\"state_class\":\"measurement\","
              "\"unit_of_measurement\":\"" + String(sensor.unit) + "\","
              "\"state_topic\":\"" + state_topic + "\","
              "\"value_template\":\"" + String(sensor.value_template) + "\","
              "\"availability_topic\":\"" + String(availability_topic) + "\","
              "\"payload_available\":\"online\","
              "\"payload_not_available\":\"offline\","
              "\"device\":" + device_config + "}";

    mqtt_publish(config_topic.c_str(), config.c_str(), true);
    delay(50);
  }

  mqtt_publish(availability_topic, "online", true);
}

bool mqtt_publish(const char* topic, const char* payload, bool retained) {
    debugPrint("Publishing to: " + String(topic));
    debugPrint("Payload: " + String(payload));
    
    bool success = mqtt.publish(topic, payload, retained);
    debugPrint("Publish " + String(success ? "successful" : "failed"));
    
    return success;
}

void reconnectMQTT() {
  while (!mqtt.connected()) {
    debugPrint("Attempting MQTT connection...");
    
    if (!ETH.localIP()) {
      debugPrint("Ethernet not connected. Waiting...");
      delay(5000);
      continue;
    }
    
    if (mqtt.connect(client_id, mqtt_username, mqtt_password,
                    availability_topic, 1, true, "offline")) {
      debugPrint("MQTT Connected");
      mqtt.publish(availability_topic, "online", true);
      publishDiscoveryConfigs();
    } else {
      Serial.printf("MQTT connection failed, rc=%d", mqtt.state());
      Serial.println(" retrying in 5 seconds");
      delay(5000);
    }
  }
}

SensorData readSensor() {
  SensorData data = {0, 0, 0, 0, 0, 0, false, millis()};
  char sensorAddress = '0';
  
  debugPrint("Reading sensor data...");
  
  String command = String(sensorAddress) + "M!";
  sdi12.sendCommand(command);
  delay(1000);

  command = String(sensorAddress) + "D0!";
  sdi12.sendCommand(command);
  delay(1000);

  String response = "";
  while (sdi12.available()) {
    response += (char)sdi12.read();
  }

  debugPrint("Raw sensor response: " + response);

  if (response.length() > 1) {
    String sensorData = response.substring(1);
    int plusIndex1 = sensorData.indexOf('+');
    int plusIndex2 = sensorData.indexOf('+', plusIndex1 + 1);
    int plusIndex3 = sensorData.indexOf('+', plusIndex2 + 1);

    if (plusIndex1 != -1 && plusIndex2 != -1 && plusIndex3 != -1) {
      String vwcRaw = sensorData.substring(plusIndex1 + 1, plusIndex2);
      String temperature = sensorData.substring(plusIndex2 + 1, plusIndex3);
      String ec = sensorData.substring(plusIndex3 + 1);

      data.vwc_raw = vwcRaw.toFloat();
      data.vwc_calibrated = calculateVWCNonSoil(data.vwc_raw);
      data.temperature = temperature.toFloat();
      data.ec_raw = ec.toFloat();
      data.ec_simple = calculateECSimple(data.ec_raw);
      data.ec_epsilon = calculateECEpsilon(data.vwc_raw);
      data.valid = true;
    }
  }
  return data;
}

void publishData(const SensorData& data) {
  if (!data.valid) return;

  String state_payload = "{\"vwc_raw\":" + String(data.vwc_raw, 2) + ","
                        "\"vwc_calibrated\":" + String(data.vwc_calibrated, 2) + ","
                        "\"temperature\":" + String(data.temperature, 2) + ","
                        "\"ec_raw\":" + String(data.ec_raw, 2) + ","
                        "\"ec_simple\":" + String(data.ec_simple, 2) + ","
                        "\"ec_epsilon\":" + String(data.ec_epsilon, 2) + "}";
  
  String state_topic = getStateTopic();
  bool success = mqtt_publish(state_topic.c_str(), state_payload.c_str(), true);
  
  if (success) {
    debugTable(data);
    readingNumber++;
  }
}

void loop() {
  if (!mqtt.connected()) {
    reconnectMQTT();
  }
  mqtt.loop();

  unsigned long currentTime = millis();
  if (currentTime - lastPublishTime >= MQTT_PUBLISH_INTERVAL) {
    SensorData data = readSensor();
    
    if (data.valid) {
      publishData(data);
    } else {
      debugPrint("Failed to read sensor data");
    }
    
    lastPublishTime = currentTime;
  }
}
