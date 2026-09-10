#include <WiFi.h>
#include <WebServer.h>
#include <Firebase_ESP_Client.h>
#include "DHT.h"
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"]

// --- WIFI & FIREBASE CREDENTIALS ---
#define WIFI_SSID "Sew"
#define WIFI_PASSWORD "12345678"
#define API_KEY "AIzaSyC7Kbb5kjUeZDs9hbzVS2c9vqc26wcIQRM"
#define DATABASE_URL "https://smartgaurd-robot-default-rtdb.asia-southeast1.firebasedatabase.app"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;

WebServer server(80);

// --- PIN DEFINITIONS (UPDATED) ---
#define DHTPIN 14
#define DHTTYPE DHT11
#define GAS_PIN 32   // Analog (ADC1 - Safe with Wi-Fi)
#define FLAME_PIN 35 // Analog (ADC1 - Safe with Wi-Fi)
#define BUZZER_PIN 13 

#define TRIG_PIN 26
#define ECHO_PIN 27

#define IR_S1 34
#define IR_S2 4    // Digital Read (Safe with Wi-Fi)
#define IR_S3 2    // Boot Pin (Ensure it isn't pulled HIGH during startup)
#define IR_S4 33
#define IR_S5 25

#define ENA 5
#define IN1 23
#define IN2 22
#define ENB 18
#define IN3 21
#define IN4 19

// ==========================================
// ⚠️ IR SENSOR TUNING ⚠️
// Most sensors output 0 for Black and 1 for White. 
// If your robot behaves backwards, change this to 1!
#define LINE_COLOR 0 
// ==========================================

DHT dht(DHTPIN, DHTTYPE);

int speedVal = 150; 
long distance = 0;
unsigned long previousMillis = 0;
unsigned long previousControlMillis = 0;
String robotStatus = "Online";
String currentMode = "Manual";       
String currentCommand = "S";       

const char controlHTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: sans-serif; background: #121212; color: white; text-align: center; padding-top: 50px; }
  .btn { background: #333; color: white; border: none; border-radius: 10px; width: 80px; height: 80px; font-size: 24px; margin: 5px; cursor: pointer; }
  .btn:active { background: #ffaa00; }
  .stop-btn { background: #ff4444; }
  .mode-btn { background: #00ffcc; color: black; padding: 15px; font-weight: bold; width: 80%; border-radius: 5px; margin-bottom: 20px; cursor: pointer; }
  .grid { display: grid; grid-template-columns: repeat(3, 90px); justify-content: center; }
</style>
</head><body>
  <h2>SmartGuard IP Remote</h2>
  <button id="mBtn" class="mode-btn" onclick="toggleMode()">MODE: MANUAL</button>
  <div class="grid">
    <div></div><button class="btn" onmousedown="s('F')" onmouseup="s('S')" ontouchstart="s('F')" ontouchend="s('S')">▲</button><div></div>
    <button class="btn" onmousedown="s('L')" onmouseup="s('S')" ontouchstart="s('L')" ontouchend="s('S')">◀</button>
    <button class="btn stop-btn" onclick="s('S')">■</button>
    <button class="btn" onmousedown="s('R')" onmouseup="s('S')" ontouchstart="s('R')" ontouchend="s('S')">▶</button>
    <div></div><button class="btn" onmousedown="s('B')" onmouseup="s('S')" ontouchstart="s('B')" ontouchend="s('S')">▼</button><div></div>
  </div>
  <script>
    let m = "Manual";
    function toggleMode() {
      m = (m === "Auto") ? "Manual" : "Auto";
      document.getElementById("mBtn").innerText = "MODE: " + m.toUpperCase();
      fetch(`/setMode?m=${m}`);
    }
    function s(c) { if(m === "Manual" || c === "S") fetch(`/action?c=${c}`); }
  </script>
</body></html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  pinMode(GAS_PIN, INPUT); pinMode(FLAME_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT); pinMode(TRIG_PIN, OUTPUT); pinMode(ECHO_PIN, INPUT);
  pinMode(IR_S1, INPUT); pinMode(IR_S2, INPUT); pinMode(IR_S3, INPUT);
  pinMode(IR_S4, INPUT); pinMode(IR_S5, INPUT);
  pinMode(ENA, OUTPUT); pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(ENB, OUTPUT); pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);

  dht.begin();

  // FIX FOR BATTERY POWER: Force Station mode and prevent Wi-Fi sleep
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); 
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nIP Address: "); Serial.println(WiFi.localIP());

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  if (Firebase.signUp(&config, &auth, "", "")) signupOK = true;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  server.on("/", []() { server.send(200, "text/html", controlHTML); });
  
  server.on("/setMode", []() { 
    currentMode = server.arg("m"); 
    if(currentMode == "Auto") currentCommand = "S";
    if(Firebase.ready()) {
      Firebase.RTDB.setString(&fbdo, "SmartGuard/Mode", currentMode);
      Firebase.RTDB.setString(&fbdo, "SmartGuard/Command", currentCommand);
    }
    previousControlMillis = millis(); 
    server.send(200); 
  });
  
  server.on("/action", []() { 
    currentCommand = server.arg("c"); 
    if(Firebase.ready()) {
      Firebase.RTDB.setString(&fbdo, "SmartGuard/Command", currentCommand);
    }
    previousControlMillis = millis(); 
    server.send(200); 
  });
  
  server.begin();
}

void loop() {
  server.handleClient();
  unsigned long now = millis();

  // 1. Firebase Command Sync
  if (now - previousControlMillis >= 600) {
    previousControlMillis = now;
    if (Firebase.ready()) {
      if (Firebase.RTDB.getString(&fbdo, "SmartGuard/Mode")) currentMode = fbdo.stringData();
      if (currentMode == "Manual") {
        if (Firebase.RTDB.getString(&fbdo, "SmartGuard/Command")) currentCommand = fbdo.stringData();
      }
    }
  }

  // Read sensors
  int s1 = digitalRead(IR_S1);
  int s2 = digitalRead(IR_S2);
  int s3 = digitalRead(IR_S3);
  int s4 = digitalRead(IR_S4);
  int s5 = digitalRead(IR_S5);

  distance = getDistance();

  // -------- OBSTACLE CONTROL --------
  if (distance > 0 && distance <= 10) {
    stopMotors();
    robotStatus = "Obstacle Close!";
  } 
  else {
    if (distance > 10 && distance <= 50) {
      speedVal = 80; 
    } else {
      speedVal = 150; 
    }

    if (currentMode == "Auto") {
      // -------- LINE FOLLOWING --------
      if (s1 == LOW && s2 == LOW && s3 == LOW && s4 == LOW && s5 == LOW) {
        robotStatus = "Marker Paused";
        stopMotors();
        
        unsigned long pauseStart = millis();
        while(millis() - pauseStart < 5000) { server.handleClient(); }
        
        moveForward(speedVal); 
        pauseStart = millis();
        while(millis() - pauseStart < 400) { server.handleClient(); }
      }
      else if (s3 == LOW) {
        moveForward(speedVal);
        robotStatus = "Auto: Forward";
      }
      else if (s1 == LOW || s2 == LOW) {
        turnLeft(200); 
        robotStatus = "Auto: Left";
      }
      else if (s5 == LOW || s4 == LOW) {
        turnRight(200);
        robotStatus = "Auto: Right";
      }
      else {
        stopMotors(); 
        robotStatus = "Auto: Line Lost";
      }
    } 
    else {
      // -------- MANUAL CONTROL --------
      robotStatus = "Manual Mode";
      if (currentCommand == "F") moveForward(speedVal);
      else if (currentCommand == "B") moveBackward(speedVal);
      else if (currentCommand == "L") turnLeft(200); 
      else if (currentCommand == "R") turnRight(200);
      else stopMotors();
    }
  }

  // -------- SENSOR MANAGEMENT --------
  if (now - previousMillis >= 2000) {
    previousMillis = now;
    
    int g = analogRead(GAS_PIN);     
    int rawFlame = analogRead(FLAME_PIN);  
    float h = dht.readHumidity();
    float t = dht.readTemperature();

    int flamePercent = map(rawFlame, 4095, 0, 0, 100); 
    flamePercent = constrain(flamePercent, 0, 100); 

    if (flamePercent > 60) tone(BUZZER_PIN, 1000, 200);       
    else if (g > 2000) tone(BUZZER_PIN, 1500, 200);           
    else if (t > 40.0) tone(BUZZER_PIN, 500, 200);            
    else if (h > 85.0) tone(BUZZER_PIN, 2000, 200);           
    else noTone(BUZZER_PIN);                                  
    
    if (Firebase.ready() && signupOK) {
      Firebase.RTDB.setInt(&fbdo, "SmartGuard/Gas", g);
      Firebase.RTDB.setInt(&fbdo, "SmartGuard/Flame", flamePercent); 
      Firebase.RTDB.setFloat(&fbdo, "SmartGuard/Temperature", t);
      Firebase.RTDB.setFloat(&fbdo, "SmartGuard/Humidity", h);
      Firebase.RTDB.setString(&fbdo, "SmartGuard/Status", robotStatus);
    }
  }
}

// -------- MOTOR CONTROL --------
void moveForward(int spd) {
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
  analogWrite(ENA, spd);
  analogWrite(ENB, spd);
}

void turnLeft(int spd) {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
  analogWrite(ENA, 0);
  analogWrite(ENB, spd);
}

void turnRight(int spd) {
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
  analogWrite(ENA, spd);
  analogWrite(ENB, 0);
}

void moveBackward(int spd) {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
  analogWrite(ENA, spd);
  analogWrite(ENB, spd);
}

void stopMotors() {
  analogWrite(ENA, 0);
  analogWrite(ENB, 0);
}

// -------- ULTRASONIC SENSOR --------
long getDistance() { 
  digitalWrite(TRIG_PIN, LOW); 
  delayMicroseconds(2); 
  digitalWrite(TRIG_PIN, HIGH); 
  delayMicroseconds(10); 
  digitalWrite(TRIG_PIN, LOW); 
  return pulseIn(ECHO_PIN, HIGH, 30000) * 0.034 / 2; 
}