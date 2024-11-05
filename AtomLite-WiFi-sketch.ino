/*
 * SDI-12 Sensor with MQTT Publishing for Home Assistant
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
 *    Simple: EC_pW = EC_raw / 500
 *    Epsilon: ε = (2.887×10^-9 × RAW³ - 2.080×10^-5 × RAW² + 5.276×10^-2 × RAW - 43.39)²
 *    Where: ε is dielectric permittivity, RAW is calibrated ADC reading
 * 
 * Required Libraries:
 * - SDI12
 * - WiFi
 * - PubSubClient
 */

#include <SDI12.h>
#include <WiFi.h>
#include <PubSubClient.h>

// Debug Configuration
#define DEBUG true                  // Set to false to disable detailed debug output
#define MQTT_MAX_PACKET_SIZE 1024   // Increased for larger payloads

// Device Configuration
#define HOSTNAME_PREFIX "sdi12sensor-"
#define HOSTNAME_SUFFIX "greenhouse1"  // Change this for each sensor
#define SDI12_DATA_PIN 26             // SDI-12 data pin

// Calibration Configuration
// EC Simple calibration factor (EC raw to pW)
#define EC_SIMPLE_FACTOR 500.0

// WiFi Configuration
const char* ssid = "YOUR_WIFI_SSID"; // Change this
const char* password = "YOUR_WIFI_PASSWORD"; // Change this
const char* hostname = HOSTNAME_PREFIX HOSTNAME_SUFFIX; // Leave this (probably)

// MQTT Configuration
const char* mqtt_server = "YOUR_MQTT_SERVER"; // Change this
const int mqtt_port = 1883;
const char* mqtt_username = "YOUR_MQTT_USERNAME"; // Change this
const char* mqtt_password = "YOUR_MQTT_PASSWORD"; // Change this
const char* client_id = hostname; // Leave this
const char* discovery_prefix = "homeassistant"; // Leave this for auto discovery
const char* availability_topic = "sensors/sdi12sensor/status"; // Leave this (probably)

// Timing Configuration
const unsigned long MQTT_PUBLISH_INTERVAL = 10000;  // Publish every 10 seconds
unsigned long lastPublishTime = 0;
unsigned long readingNumber = 0;  // Counter for readings

// Data structure for sensor readings
struct SensorData {
  float vwc_raw;          // Raw VWC reading
  float vwc_calibrated;   // Calibrated VWC (0-100%)
  float temperature;      // Temperature in °C
  float ec_raw;          // Raw EC in µS/cm
  float ec_simple;       // EC using simple division (pW)
  float ec_epsilon;      // EC using epsilon calculation (pW)
  bool valid;            // Data validity flag
  unsigned long timestamp; // Time since boot in milliseconds
};

// Initialize objects
SDI12 sdi12(SDI12_DATA_PIN);
WiFiClient espClient;
PubSubClient mqtt(espClient);

// Function declarations
void debugPrint(String message, bool forceOutput);
void debugTable(const SensorData& data);
String getStateTopic();
bool mqtt_publish(const char* topic, const char* payload, bool retained);
void setupWiFi();
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

// Format time string (MM:SS)
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
  Serial.printf("   ├─ Simple Cal: %.2f pW\n", data.ec_simple);
  Serial.printf("   └─ Epsilon Cal: %.2f pW\n", data.ec_epsilon);
  Serial.println("─────────────────────────────────────\n");
}

// Calculate VWC using manufacturer's formula
float calculateVWCNonSoil(float raw) {
  // VWC = 6.771×10^-10 × RAW³ - 5.105×10^-6 × RAW² + 1.302×10^-2 × RAW - 10.848
  float result = (6.771e-10 * pow(raw, 3) - 
                 5.105e-6 * pow(raw, 2) + 
                 1.302e-2 * raw - 
                 10.848) * 100;  // Multiply by 100 to convert to percentage
  return constrain(result, 0, 100);
}


// Calculate EC using simple division
float calculateECSimple(float raw_ec) {
  return raw_ec / EC_SIMPLE_FACTOR;
}

// Calculate EC using epsilon calculation
float calculateECEpsilon(float raw) {
  // ε = (2.887×10^-9 × RAW³ - 2.080×10^-5 × RAW² + 5.276×10^-2 × RAW - 43.39)²
  float epsilon = 2.887e-9 * pow(raw, 3) - 
                 2.080e-5 * pow(raw, 2) + 
                 5.276e-2 * raw - 
                 43.39;
  return pow(epsilon, 2);
}

String getStateTopic() {
  return String("sensors/sdi12sensor/") + hostname + "/state";
}

void setup() {
  Serial.begin(115200);
  while (!Serial);

  Serial.println("\nSDI-12 Sensor Starting Up");
  Serial.println("Debug output is " + String(DEBUG ? "enabled" : "disabled"));

  // Set the hostname for the ESP32
  WiFi.setHostname(hostname);
  
  // Initialize SDI-12
  sdi12.begin();
  debugPrint("SDI-12 Sensor Initialized", true);
  debugPrint("Device hostname: " + String(hostname), true);

  // Connect to WiFi
  setupWiFi();

  // Configure MQTT
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setBufferSize(1024);
  
  debugPrint("Setup complete");
}

void setupWiFi() {
  debugPrint("Connecting to WiFi network: " + String(ssid), true);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi connected");
  Serial.printf("Hostname: %s\n", WiFi.getHostname());
  Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
}

void publishDiscoveryConfigs() {
  debugPrint("Publishing discovery configurations...");
  
  String state_topic = getStateTopic();
  String device_config = "{\"identifiers\":[\"" + String(hostname) + "\"],"
                        "\"name\":\"" + String(hostname) + "\","
                        "\"model\":\"SDI-12 Substrate Sensor\","
                        "\"manufacturer\":\"Chill Division\"}";

  // Configuration for each sensor type
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

  // Publish config for each sensor
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
    delay(50);  // Short delay between publishes
  }

  // Publish initial availability
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
      data.ec_epsilon = calculateECEpsilon(data.vwc_raw);  // Note: Using VWC raw value for epsilon calculation
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
