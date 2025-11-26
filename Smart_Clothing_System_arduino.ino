/*
 * Smart Rain-Proof Clothes Protection System - MQTT VERSION
 * Team: Mohsin Sajjad, Shafique Qureshi, Muhammad Hassan, Muhammad Afnan Khan
 * Modified: Converted from Blynk to MQTT for IoT MQTT Panel app
 * Platform: ESP32 with Arduino IDE
 */

// Include necessary libraries
#include <WiFi.h>             // For WiFi connectivity
#include <PubSubClient.h>     // For MQTT protocol
#include <ESP32Servo.h>       // For controlling servo motor
#include <DHT.h>              // For DHT22 temperature & humidity sensor
#include <ArduinoJson.h>      // For creating JSON data payloads

// -------- WiFi Configuration --------
const char* ssid = "StormFiber-35A0";           // WiFi SSID
const char* password = "TSSx5Jd4";    // WiFi Password

// -------- MQTT Broker Configuration --------
const char* mqtt_server = "broker.hivemq.com"; // Public MQTT broker
const int mqtt_port = 1883;                    // Default port
const char* mqtt_user = "";                    // Username (not needed for public broker)
const char* mqtt_password = "";                // Password (not needed)

// -------- MQTT Topics --------
const char* DEVICE_ID = "smart_clothes_001";  // Unique identifier for this ESP32
String TOPIC_PREFIX = "smartclothes/" + String(DEVICE_ID) + "/";

// Define specific topic strings for various data types
String TOPIC_STATUS = TOPIC_PREFIX + "status";
String TOPIC_TEMPERATURE = TOPIC_PREFIX + "temperature";
String TOPIC_HUMIDITY = TOPIC_PREFIX + "humidity";
String TOPIC_RAIN = TOPIC_PREFIX + "rain";
String TOPIC_CLOTHES_POS = TOPIC_PREFIX + "clothes_position";
String TOPIC_SYSTEM_STATE = TOPIC_PREFIX + "system_state";
String TOPIC_MODE = TOPIC_PREFIX + "mode";
String TOPIC_COMMAND = TOPIC_PREFIX + "command";
String TOPIC_ALL_DATA = TOPIC_PREFIX + "all_data";  // JSON payload for all data

// -------- Pin Definitions --------
#define DHT_PIN 4              // DHT22 sensor data pin
#define RAIN_ANALOG_PIN 34     // Analog input for rain sensor
#define RAIN_DIGITAL_PIN 35    // Digital input for rain sensor
#define SERVO_PIN 18           // Servo motor signal pin
#define BUZZER_PIN 19          // Buzzer pin
#define BUTTON_PIN 21          // Manual override button

// -------- DHT Sensor Setup --------
#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);    // Initialize DHT22

// -------- Servo Motor Setup --------
Servo clothesServo;            // Create servo object

// -------- MQTT and WiFi Clients --------
WiFiClient espClient;          // ESP WiFi client
PubSubClient client(espClient); // MQTT client

// -------- System Variables --------
bool rainDetected = false;           // True if rain is detected
bool previousRainState = false;      // Previous state of rain detection
bool clothesInside = false;          // Are clothes inside?
bool manualMode = false;             // Is system in manual mode?
bool buzzerActive = false;           // Is buzzer currently active?
bool servoMoving = false;            // Is servo currently moving?

// -------- Timing Variables --------
unsigned long lastRainCheck = 0;
unsigned long lastDHTRead = 0;
unsigned long lastMQTTUpdate = 0;
unsigned long rainDetectedTime = 0;
unsigned long rainStoppedTime = 0;
unsigned long buzzerStartTime = 0;
unsigned long servoStartTime = 0;
unsigned long stateChangeTime = 0;
unsigned long lastReconnectAttempt = 0;

// -------- Timing Constants (in milliseconds) --------
const unsigned long RAIN_CHECK_INTERVAL = 500;      // Rain sensor check every 0.5 sec
const unsigned long DHT_READ_INTERVAL = 3000;       // DHT sensor read every 3 sec
const unsigned long MQTT_UPDATE_INTERVAL = 2000;    // Publish MQTT data every 2 sec
const unsigned long BUZZER_DURATION = 1500;         // Buzzer rings for 1.5 sec
const unsigned long RAIN_DETECTION_DELAY = 2000;    // Wait before reacting to rain
const unsigned long RAIN_STOP_DELAY = 15000;        // Wait after rain stops
const unsigned long SERVO_MOVE_TIME = 2000;         // Servo takes 2 sec to move
const unsigned long DEBOUNCE_DELAY = 50;            // Debounce delay for button
const unsigned long RECONNECT_INTERVAL = 5000;      // Reconnect MQTT every 5 sec

// -------- Servo Positions --------
const int CLOTHES_OUTSIDE_POS = 0;     // Servo angle for extended clothes
const int CLOTHES_INSIDE_POS = 90;     // Servo angle for retracted clothes

// -------- Rain Sensor Threshold --------
const int RAIN_THRESHOLD = 500;        // Analog threshold for rain detection

// -------- Environmental Readings --------
float temperature = 0.0;
float humidity = 0.0;

// -------- State Machine Definition --------
enum SystemState {
  STATE_MONITORING,          // Normal monitoring
  STATE_RAIN_DETECTED,       // Rain has just been detected
  STATE_BUZZER_ALERT,        // Sounding the buzzer
  STATE_RETRACTING_CLOTHES,  // Retracting clothes inside
  STATE_CLOTHES_PROTECTED,   // Clothes are inside, safe
  STATE_RAIN_STOPPED,        // Rain stopped, waiting
  STATE_EXTENDING_CLOTHES    // Extending clothes back outside
};

SystemState currentState = STATE_MONITORING;      // Initial state
SystemState previousState = STATE_MONITORING;     // Previous state for transition tracking

// ---------------------- SETUP ----------------------
void setup() {
  Serial.begin(115200);  // Start serial monitor

  // Set pin modes
  pinMode(RAIN_ANALOG_PIN, INPUT);
  pinMode(RAIN_DIGITAL_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  dht.begin();                  // Start DHT sensor
  clothesServo.attach(SERVO_PIN);  // Attach servo to pin
  clothesServo.write(CLOTHES_OUTSIDE_POS); // Start with clothes outside
  delay(1000); // Allow servo to position
  digitalWrite(BUZZER_PIN, LOW); // Ensure buzzer is off

  setupWiFi();                 // Connect to WiFi
  client.setServer(mqtt_server, mqtt_port); // Set MQTT server
  client.setCallback(mqttCallback);         // Set callback for MQTT messages

  stateChangeTime = millis();  // Initialize timing
  Serial.println("Smart Clothes Protection System Started!");
  Serial.println("System State: MONITORING");
}

// ---------------------- MAIN LOOP ----------------------
void loop() {
  // Reconnect MQTT if not connected
  if (!client.connected()) {
    if (millis() - lastReconnectAttempt > RECONNECT_INTERVAL) {
      lastReconnectAttempt = millis();
      if (reconnectMQTT()) {
        lastReconnectAttempt = 0;
      }
    }
  } else {
    client.loop();  // Handle incoming MQTT messages
  }

  checkManualButton();                     // Check for button press
  if (millis() - lastRainCheck >= RAIN_CHECK_INTERVAL) {
    checkRainSensor();                    // Check rain sensor
    lastRainCheck = millis();
  }
  if (millis() - lastDHTRead >= DHT_READ_INTERVAL) {
    readDHTSensor();                      // Read temp/humidity
    lastDHTRead = millis();
  }
  if (millis() - lastMQTTUpdate >= MQTT_UPDATE_INTERVAL) {
    publishAllData();                     // Publish MQTT data
    lastMQTTUpdate = millis();
  }

  handleBuzzer();                         // Manage buzzer duration
  handleServo();                          // Manage servo timing
  runSystemStateMachine();                // Core logic
}

// ---------------------- WiFi & MQTT Functions ----------------------
void setupWiFi() {
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.println(WiFi.localIP());
}

boolean reconnectMQTT() {
  if (client.connect(DEVICE_ID, mqtt_user, mqtt_password)) {
    Serial.println("MQTT Connected");
    client.subscribe(TOPIC_COMMAND.c_str());      // Listen for commands
    client.publish(TOPIC_STATUS.c_str(), "ONLINE", true);
    publishInitialStatus();                       // Publish initial state
    return true;
  } else {
    Serial.print("MQTT failed, rc=");
    Serial.println(client.state());
    return false;
  }
}

void publishInitialStatus() {
  // Publish current settings to MQTT
  client.publish(TOPIC_CLOTHES_POS.c_str(), clothesInside ? "INSIDE" : "OUTSIDE", true);
  client.publish(TOPIC_SYSTEM_STATE.c_str(), getSystemStateString().c_str(), true);
  client.publish(TOPIC_MODE.c_str(), manualMode ? "MANUAL" : "AUTO", true);
  client.publish(TOPIC_RAIN.c_str(), rainDetected ? "DETECTED" : "NONE", true);
}

// ---------------------- Sensor & Button Functions ----------------------
void checkRainSensor() {
  int analogValue = analogRead(RAIN_ANALOG_PIN);
  int digitalValue = digitalRead(RAIN_DIGITAL_PIN);
  bool currentRainStatus = (analogValue < RAIN_THRESHOLD) || (digitalValue == LOW);

  if (currentRainStatus != previousRainState) {
    if (currentRainStatus && !rainDetected) {
      rainDetected = true;
      rainDetectedTime = millis();
      Serial.println("Rain detected!");
      if (client.connected()) client.publish(TOPIC_RAIN.c_str(), "1");
    } else if (!currentRainStatus && rainDetected) {
      rainDetected = false;
      rainStoppedTime = millis();
      Serial.println("Rain stopped!");
      if (client.connected()) client.publish(TOPIC_RAIN.c_str(), "0");
    }
    previousRainState = currentRainStatus;
  }
}

void readDHTSensor() {
  float newTemp = dht.readTemperature();
  float newHum = dht.readHumidity();
  if (!isnan(newTemp) && !isnan(newHum)) {
    temperature = newTemp;
    humidity = newHum;
  } else {
    Serial.println("Failed to read from DHT sensor!");
  }
}

void checkManualButton() {
  static unsigned long lastPress = 0;
  static bool pressed = false;
  bool state = digitalRead(BUTTON_PIN) == LOW;

  if (state && !pressed && (millis() - lastPress > DEBOUNCE_DELAY)) {
    pressed = true;
    lastPress = millis();
    toggleClothesPosition();
    manualMode = true;
    publishAllData();
    Serial.println("Manual override activated!");
  } else if (!state && pressed) {
    pressed = false;
  }
}

// ---------------------- MQTT Callback ----------------------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) message += (char)payload[i];

  Serial.print("MQTT Message: [");
  Serial.print(topic);
  Serial.print("] ");
  Serial.println(message);

  if (String(topic) == TOPIC_COMMAND) {
    if (message == "TOGGLE") {
      toggleClothesPosition();
      manualMode = true;
    } else if (message == "AUTO") {
      manualMode = false;
      currentState = STATE_MONITORING;
    } else if (message == "RETRACT") {
      startRetractClothes(); manualMode = true;
    } else if (message == "EXTEND") {
      startExtendClothes(); manualMode = true;
    }
    publishAllData();
  }
}

// ---------------------- System Functions ----------------------

// Publish all sensor data to MQTT
void publishAllData() {
  if (!client.connected()) return;
  
  // Create JSON payload with all data
  StaticJsonDocument<300> doc;
  doc["device_id"] = DEVICE_ID;
  doc["timestamp"] = millis();
  doc["temperature"] = temperature;
  doc["humidity"] = humidity;
  doc["rain_detected"] = rainDetected;
  doc["clothes_inside"] = clothesInside;
  doc["manual_mode"] = manualMode;
  doc["system_state"] = getSystemStateString();
  doc["buzzer_active"] = buzzerActive;
  doc["servo_moving"] = servoMoving;
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  // Publish individual values
  client.publish(TOPIC_TEMPERATURE.c_str(), String(temperature).c_str());
  client.publish(TOPIC_HUMIDITY.c_str(), String(humidity).c_str());
  client.publish(TOPIC_RAIN.c_str(), rainDetected ? "DETECTED" : "NONE");
  client.publish(TOPIC_CLOTHES_POS.c_str(), clothesInside ? "INSIDE" : "OUTSIDE");
  client.publish(TOPIC_SYSTEM_STATE.c_str(), getSystemStateString().c_str());
  client.publish(TOPIC_MODE.c_str(), manualMode ? "MANUAL" : "AUTO");
  
  // Publish combined JSON data
  client.publish(TOPIC_ALL_DATA.c_str(), jsonString.c_str());
}

// Handle buzzer timing
void handleBuzzer() {
  if (buzzerActive && (millis() - buzzerStartTime >= BUZZER_DURATION)) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerActive = false;
    Serial.println("Buzzer stopped");
  }
}

// Handle servo timing
void handleServo() {
  if (servoMoving && (millis() - servoStartTime >= SERVO_MOVE_TIME)) {
    servoMoving = false;
    Serial.println("Servo movement completed");
  }
}

// Main state machine logic
void runSystemStateMachine() {
  if (manualMode) return; // Skip automatic control in manual mode
  
  SystemState nextState = currentState;
  
  switch (currentState) {
    case STATE_MONITORING:
      if (rainDetected && (millis() - rainDetectedTime >= RAIN_DETECTION_DELAY)) {
        nextState = STATE_RAIN_DETECTED;
      }
      break;
      
    case STATE_RAIN_DETECTED:
      if (!clothesInside) {
        nextState = STATE_BUZZER_ALERT;
        startBuzzer();
      } else {
        nextState = STATE_CLOTHES_PROTECTED;
      }
      break;
      
    case STATE_BUZZER_ALERT:
      if (!buzzerActive) {
        nextState = STATE_RETRACTING_CLOTHES;
        startRetractClothes();
      }
      break;
      
    case STATE_RETRACTING_CLOTHES:
      if (!servoMoving) {
        nextState = STATE_CLOTHES_PROTECTED;
      }
      break;
      
    case STATE_CLOTHES_PROTECTED:
      if (!rainDetected && (millis() - rainStoppedTime >= RAIN_STOP_DELAY)) {
        nextState = STATE_RAIN_STOPPED;
      }
      break;
      
    case STATE_RAIN_STOPPED:
      if (clothesInside) {
        nextState = STATE_EXTENDING_CLOTHES;
        startExtendClothes();
      } else {
        nextState = STATE_MONITORING;
      }
      break;
      
    case STATE_EXTENDING_CLOTHES:
      if (!servoMoving) {
        nextState = STATE_MONITORING;
      }
      break;
  }
  
  if (nextState != currentState) {
    changeSystemState(nextState);
  }
}

// Get system state as string
String getSystemStateString() {
  switch (currentState) {
    case STATE_MONITORING: return "MONITORING";
    case STATE_RAIN_DETECTED: return "RAIN_DETECTED";
    case STATE_BUZZER_ALERT: return "BUZZER_ALERT";
    case STATE_RETRACTING_CLOTHES: return "RETRACTING_CLOTHES";
    case STATE_CLOTHES_PROTECTED: return "CLOTHES_PROTECTED";
    case STATE_RAIN_STOPPED: return "RAIN_STOPPED";
    case STATE_EXTENDING_CLOTHES: return "EXTENDING_CLOTHES";
    default: return "UNKNOWN";
  }
}

// Toggle clothes position
void toggleClothesPosition() {
  if (clothesInside) {
    startExtendClothes();
  } else {
    startRetractClothes();
  }
}

// Start retracting clothes
void startRetractClothes() {
  if (servoMoving) return;
  
  clothesServo.write(CLOTHES_INSIDE_POS);
  clothesInside = true;
  servoMoving = true;
  servoStartTime = millis();
  Serial.println("Retracting clothes...");
  
  if (client.connected()) {
    client.publish(TOPIC_CLOTHES_POS.c_str(), "RETRACTING");
  }
}

// Start extending clothes
void startExtendClothes() {
  if (servoMoving) return;
  
  clothesServo.write(CLOTHES_OUTSIDE_POS);
  clothesInside = false;
  servoMoving = true;
  servoStartTime = millis();
  Serial.println("Extending clothes...");
  
  if (client.connected()) {
    client.publish(TOPIC_CLOTHES_POS.c_str(), "EXTENDING");
  }
}

// Start buzzer
void startBuzzer() {
  digitalWrite(BUZZER_PIN, HIGH);
  buzzerActive = true;
  buzzerStartTime = millis();
  Serial.println("Buzzer started");
}

// Change system state
void changeSystemState(SystemState newState) {
  previousState = currentState;
  currentState = newState;
  stateChangeTime = millis();
  
  Serial.print("State changed from ");
  Serial.print(getSystemStateString());
  Serial.print(" to ");
  Serial.println(getSystemStateString());
  
  if (client.connected()) {
    client.publish(TOPIC_SYSTEM_STATE.c_str(), getSystemStateString().c_str());
  }
}