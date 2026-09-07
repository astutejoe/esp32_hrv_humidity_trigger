#include <WiFi.h>
#include <WiFiUdp.h>

// --- Configuration ---
const char* SSID = "Astute Network";
const char* PASSWORD = "<password>"; //ATENTION!! Change this
const int UDP_PORT = 1337;

// --- Pin Definitions ---
const int RELAY_PIN = 22; // DAOKI Relay IN
const int SENSE_PIN = 34; // FZ0430 Sensor S pin

// --- Pulse Timing ---
unsigned long pulseStartTime = 0;
bool isRelayClosed = false;
const unsigned long PULSE_DURATION_MS = 500; // 0.5 second button press

// --- State Threshold ---
// Values above 800 = HRV is ON
// Values below 800 = HRV is OFF
const int THRESHOLD = 800; 

// --- Network Objects ---
WiFiUDP udp;

// Connection watchdog timers
unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 5000;   // Check link every 5s
unsigned long disconnectedSince = 0;
const unsigned long REBOOT_AFTER_OFFLINE_MS = 300000; // Hard reboot if offline > 5 minutes
bool wasConnected = false;

constexpr int MAX_PACKET_SIZE = 256;
char incomingPacket[MAX_PACKET_SIZE];

// ==============================================================================
// --- REMOTE DEBUGGING MACRO ---
// Comment out the line below to completely compile out UDP debug features.
#define ENABLE_REMOTE_DEBUG 
// ==============================================================================

#ifdef ENABLE_REMOTE_DEBUG
  const char* targetDebugIP = "192.168.1.2";
  const int targetDebugPort = 1339;
  unsigned long lastDebugTime = 0;
  const unsigned long DEBUG_INTERVAL_MS = 2000; // Send state every 2 seconds

  // Helper function to blast strings out over UDP
  void sendDebugMessage(String msg) {
    udp.beginPacket(targetDebugIP, targetDebugPort);
    udp.print(msg + "\n");
    udp.endPacket();
    Serial.println(msg); // Mirror to serial as a backup
  }
#else
  // If macro is disabled, calls to sendDebugMessage cost 0 CPU cycles.
  #define sendDebugMessage(msg) 
#endif

void initWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    WiFi.begin(SSID, PASSWORD);
    Serial.println("Initiating Wi-Fi connection...");
}

void bindUDP() {
    udp.stop(); // Clean up any stale socket
    if (udp.begin(UDP_PORT)) {
        Serial.printf("UDP listener bound to port %u\n", UDP_PORT);
    } else {
        Serial.println("Failed to bind UDP socket!");
    }
}

void maintainConnection() {
  unsigned long now = millis();

  if (now - lastWifiCheck < WIFI_CHECK_INTERVAL) {
    return;
  }

  lastWifiCheck = now;

  if (WiFi.status() == WL_CONNECTED) {
    if (!wasConnected) {
      // State transition: Disconnected -> Connected
      Serial.printf("Wi-Fi connected. IP: %s\n", WiFi.localIP().toString().c_str());
      bindUDP();
      wasConnected = true;
      disconnectedSince = 0;
    }
  } else {
    if (wasConnected) {
      // State transition: Connected -> Disconnected
      Serial.println("Wi-Fi link dropped!");
      wasConnected = false;
      disconnectedSince = now;
      udp.stop();
    }

    Serial.println("Attempting Wi-Fi reconnect...");
    WiFi.reconnect();

    // Failsafe: if disconnected longer than threshold, force reboot
    if (disconnectedSince > 0 && (now - disconnectedSince > REBOOT_AFTER_OFFLINE_MS)) {
      Serial.println("Offline limit exceeded. Triggering ESP.restart()...");
      ESP.restart();
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  // Initialize relay LOW immediately to prevent phantom triggers during boot
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  
  pinMode(SENSE_PIN, INPUT);

  Serial.print("Connecting to Wi-Fi");

  initWiFi();

  // Initial wait up to 10s for clean boot
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wasConnected = true;
    bindUDP();
  } else {
    disconnectedSince = millis();
  }
  
  #ifdef ENABLE_REMOTE_DEBUG
    sendDebugMessage("BOOT: ESP32 Initialized. Network IP: " + WiFi.localIP().toString());
  #endif
}

void loop() {
  maintainConnection();

  // --- TASK 1: Read the Master HRV State ---
  int rawVoltageValue = analogRead(SENSE_PIN);
  bool isHrvRunning = rawVoltageValue > THRESHOLD;

  // --- TASK 2: Remote Debug Heartbeat ---
  #ifdef ENABLE_REMOTE_DEBUG
    if (millis() - lastDebugTime >= DEBUG_INTERVAL_MS) {
      String stateStr = isHrvRunning ? "ON" : "OFF";
      String heartbeat = "STATE: " + stateStr + " | ADC: " + String(rawVoltageValue);
      sendDebugMessage(heartbeat);
      lastDebugTime = millis();
    }
  #endif

  // --- TASK 3: The Network Listener ---
  int packetSize = udp.parsePacket();
  if (packetSize) {
    const int len = udp.read(incomingPacket, MAX_PACKET_SIZE);
    if (len > 0) {
      incomingPacket[min(len, MAX_PACKET_SIZE - 1)] = '\0';
    }
    
    if (strncmp(incomingPacket, "BOOST", 5) == 0) {
      if (isHrvRunning) {
        sendDebugMessage("CMD: BOOST received | Result: IGNORED (HRV already ON, ADC: " + String(rawVoltageValue) + ")");
      } else {
        pulseStartTime = millis();
        isRelayClosed = true;
        digitalWrite(RELAY_PIN, HIGH);
        sendDebugMessage("CMD: BOOST received | Result: TAPPING BUTTON (HRV is OFF, ADC: " + String(rawVoltageValue) + ")");
      }
    }
  }

  // --- TASK 4: The Finger Release ---
  if (isRelayClosed && (millis() - pulseStartTime >= PULSE_DURATION_MS)) {
    digitalWrite(RELAY_PIN, LOW); 
    isRelayClosed = false;
    sendDebugMessage("EVENT: Button released.");
  }

  delay(100); // Tiny delay to yield to the FreeRTOS idle task
}