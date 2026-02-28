#define FIREBASE_DISABLE_CLOUD_MESSAGING
#define FIREBASE_DISABLE_FCM_LEGACY
#define FIREBASE_DISABLE_STORAGE
#define FIREBASE_DISABLE_FIRESTORE
// -------------------------------------------

#include <WiFi.h>
#include <WiFiManager.h>
#include <Wire.h>             // ไลบรารี I2C สำหรับ SHTC3
#include "Adafruit_SHTC3.h"   // ไลบรารีเซ็นเซอร์ SHTC3
#include <ESP32Servo.h>
#include <FirebaseESP32.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

/* ================= FIREBASE ================= */
#define API_KEY "YOR_FIREBASE_API_KEY" 
#define DATABASE_URL "YORR_FIREBASE_DB_URL"

/* ================= TELEGRAM ================= */
#define BOT_TOKEN "YOUR_TELEGRAM_BOT_TOKEN"
#define CHAT_ID "YOUR_TELEGRAM_CHAT_ID"

/* ================= PIN CONFIGURATION ================= */
// สายเซ็นเซอร์ SHTC3 ต้องต่อที่ SDA = 21, SCL = 22
#define TRIG_PIN 5
#define ECHO_PIN 18
#define SERVO_PIN 13
#define RELAY_PIN 14    
#define LED_WIFI 2      
#define LED_ACTION 27  

/* ================= OBJECTS ================= */
Adafruit_SHTC3 shtc3 = Adafruit_SHTC3(); 
Servo feederServo;
FirebaseData fbdo;
FirebaseData stream;
FirebaseAuth auth;
FirebaseConfig config;
WiFiClientSecure client;

/* ================= VARIABLES ================= */
float temp = 0, humi = 0;
int foodLevel = 0;
bool fanState = false;

// ค่า Setting เกณฑ์ Auto เริ่มต้น (โหมด Offline)
int autoTemp = 30; 
int autoFood = 20; 

// ตัวแปรรับคำสั่งจากเว็บ
String fanMode = "auto";
bool manualFanState = false;
bool feedPause = false;
bool triggerFeedFromWeb = false; // ธงรับคำสั่งให้อาหารจากเว็บ

unsigned long lastSensorUpdate = 0;
unsigned long lastHistorySave = 0;
unsigned long lastFeedTime = 0;
unsigned long lastTelegramTime = 0;

const unsigned long sensorInterval = 3000; // เช็คเซ็นเซอร์ทุก 3 วินาที
const unsigned long historyInterval = 300000; // เซฟกราฟทุก 5 นาที
const unsigned long feedCooldown = 5000; // ป้องกันให้อาหารรัวๆ (5 วินาที)
const unsigned long telegramCooldown = 5000;

/* ===================================================== */

void sendTelegram(String message) {
  if (WiFi.status() != WL_CONNECTED) return; 
  if (millis() - lastTelegramTime < telegramCooldown) return;
  lastTelegramTime = millis();
  
  client.setInsecure(); 
  HTTPClient https;
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/sendMessage?chat_id=" + String(CHAT_ID) + "&text=" + message;

  https.begin(client, url);
  int httpCode = https.GET();
  if (httpCode > 0) Serial.println("✅ Telegram sent: " + message);
  else Serial.println("❌ Telegram error: " + String(httpCode));
  https.end();
}

void readSensors() {
  // --- อ่านค่าจาก SHTC3 ---
  sensors_event_t humidity, temp_event;
  shtc3.getEvent(&humidity, &temp_event);
  
  if (!isnan(temp_event.temperature) && !isnan(humidity.relative_humidity)) {
    temp = temp_event.temperature;
    humi = humidity.relative_humidity;
  }

  // --- อ่านค่า Ultrasonic ---
  digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000); 
  int distance = duration * 0.034 / 2;
  foodLevel = constrain(map(distance, 21, 13, 0, 100), 0, 100); 
  // 21 คือความสูงเพดาน 13 คือ ความสูงเพดาน - ความสูงถังอาหาร
}

void updateFirebaseStatus() {
  FirebaseJson json;
  json.set("temp", temp);
  json.set("humi", humi);
  json.set("food", foodLevel);
  json.set("fan", fanState);
  
  if (!Firebase.updateNode(fbdo, "/current_status", json)) {
    Serial.println("Update failed: " + fbdo.errorReason());
  }
}

void saveHistory() {
  FirebaseJson history;
  history.set("temp", temp);
  history.set("humi", humi);
  history.set("food", foodLevel);
  history.set("timestamp/.sv", "timestamp"); 

  Firebase.pushJSON(fbdo, "/history", history);
  Serial.println("📊 History Saved to Firebase!");
}

void feedNow() {
  if (millis() - lastFeedTime < feedCooldown) {
    Serial.println("⏳ Feed cooldown active. Please wait.");
    return;
  }

  // เปิดไฟ LED D27 แสดงสถานะว่ากำลังให้อาหาร
  digitalWrite(LED_ACTION, HIGH); 
  Serial.println(">> ACTION: Feeding Servo Moving! <<");
  
  feederServo.write(180);
  delay(1000); // เปิดฝาอาหาร 1 วินาที
  feederServo.write(0);
  
  lastFeedTime = millis();
  
  // ปิดไฟ LED D27 เมื่อให้อาหารเสร็จ
  digitalWrite(LED_ACTION, LOW); 
  
  sendTelegram("🥣 ให้อาหารสัตว์เลี้ยงแล้ว!");
}

void controlFan(bool state) {
  if (fanState != state) {
    fanState = state;
    // โมดูล Relay ส่วนใหญ่เป็นแบบ Active LOW (สั่ง LOW คือเปิด, สั่ง HIGH คือปิด)
    digitalWrite(RELAY_PIN, state ? LOW : HIGH); 

    if (state) sendTelegram("🌡 เปิดพัดลม (อุณหภูมิ " + String(temp) + "°C)");
    else sendTelegram("❄ ปิดพัดลมแล้ว");
  }
}

/* ===================================================== */

// Stream รับคำสั่งด่วนจากเว็บ แบบครอบคลุมทุกรูปแบบ (ดักจับ Json/String/Bool)
void streamCallback(StreamData data) {
  String path = data.dataPath();
  String type = data.dataType();
  
  Serial.print("🔔 Stream Event -> Path: " + path + " | Type: " + type);

  // กรณีส่งมาเป็นก้อน JSON
  if (type == "json") {
    FirebaseJson &json = data.jsonObject();
    FirebaseJsonData jsonData;
    json.get(jsonData, "feed_trigger");
    if (jsonData.success) {
      if (jsonData.typeNum == FirebaseJson::JSON_BOOL && jsonData.boolValue) triggerFeedFromWeb = true;
      else if (jsonData.stringValue == "true" || jsonData.stringValue == "1") triggerFeedFromWeb = true;
    }
  } 
  // กรณีส่งมาเฉพาะปุ่ม feed_trigger
  else if (path.indexOf("feed_trigger") >= 0) {
    if (type == "boolean" && data.boolData() == true) triggerFeedFromWeb = true;
    else if (type == "int" && data.intData() == 1) triggerFeedFromWeb = true;
    else if (type == "string" && (data.stringData() == "true" || data.stringData() == "1")) triggerFeedFromWeb = true;
  }

  if (triggerFeedFromWeb) {
    Serial.println(" => รับคำสั่งให้อาหารแล้ว!");
  } else {
    Serial.println();
  }
}

void streamTimeout(bool timeout) {
  if (timeout) {
    Serial.println("Stream timeout, reconnecting...");
    Firebase.beginStream(stream, "/commands");
  }
}

/* ===================================================== */

void setup() {
  Serial.begin(115200);

  // ตั้งค่า Pin
  pinMode(TRIG_PIN, OUTPUT); 
  pinMode(ECHO_PIN, INPUT);
  
  pinMode(RELAY_PIN, OUTPUT); 
  digitalWrite(RELAY_PIN, HIGH); // 👈 สำคัญ: สั่ง HIGH เพื่อปิดพัดลมไว้ก่อนตอนเปิดเครื่อง
  
  pinMode(LED_WIFI, OUTPUT); 
  pinMode(LED_ACTION, OUTPUT);
  digitalWrite(LED_WIFI, LOW); 
  digitalWrite(LED_ACTION, LOW); // ปิดไฟให้อาหารไว้ก่อน

  // --- เริ่มต้นเซ็นเซอร์ SHTC3 ---
  if (!shtc3.begin()) {
    Serial.println("❌ SHTC3 Sensor not found! เช็คสาย SDA(21), SCL(22) อีกรอบครับ");
  } else {
    Serial.println("✅ SHTC3 Sensor is ready!");
  }
  
  // ของจริง ESP32Servo ควรกำหนด Timer ก่อน attach
  ESP32PWM::allocateTimer(0);
  feederServo.setPeriodHertz(50);
  feederServo.attach(SERVO_PIN, 500, 2400); 
  feederServo.write(0);

  // --- ระบบ WiFiManager ---
  WiFiManager wm;
  Serial.println("Connecting to WiFi...");
  bool res = wm.autoConnect("SmartFeeder_Setup", "12345678");

  if (!res) {
    Serial.println("❌ Failed to connect WiFi. Restarting...");
    delay(3000);
    ESP.restart(); 
  }
  
  digitalWrite(LED_WIFI, HIGH); // เน็ตติด ไฟสีน้ำเงินบนบอร์ดสว่าง
  Serial.println("✅ WiFi Connected! IP: " + WiFi.localIP().toString());

  // --- ระบบ Firebase ---
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("✅ Firebase Auth OK");
  } else {
    Serial.println("❌ Firebase Auth Error: " + String(config.signer.signupError.message.c_str()));
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // เริ่มต้น Stream
  Firebase.beginStream(stream, "/commands");
  Firebase.setStreamCallback(stream, streamCallback, streamTimeout);
}

/* ===================================================== */

void loop() {
  // 1. รับคำสั่งให้อาหารจากเว็บ
  if (triggerFeedFromWeb) {
    triggerFeedFromWeb = false; // เอาธงลง
    feedNow(); // สั่งเซอร์โว + ไฟ D27
    
    // รีเซ็ตปุ่มใน Firebase กลับเป็น false
    if (Firebase.ready()) {
      Firebase.setBool(fbdo, "/commands/feed_trigger", false);
    }
  }

  unsigned long currentMillis = millis();

  // 2. เช็คสถานะ WiFi สำหรับไฟ LED บนบอร์ด
  if (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_WIFI, LOW); // เน็ตหลุด ไฟดับ
  } else {
    digitalWrite(LED_WIFI, HIGH); // เน็ตติด ไฟสว่าง
  }

  // 3. ลูปหลักเช็คเซ็นเซอร์และการทำงานอัตโนมัติ (ทุก 3 วินาที)
  if (currentMillis - lastSensorUpdate > sensorInterval) {
    lastSensorUpdate = currentMillis;
    readSensors();  

    Serial.printf("Mode: %s | T: %.1fC | H: %.1f%% | Food: %d%%\n", (WiFi.status() == WL_CONNECTED) ? "ONLINE" : "OFFLINE", temp, humi, foodLevel);

    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
      // 🟢 โหมด ONLINE 🟢
      if (Firebase.getInt(fbdo, "/settings/auto_temp")) autoTemp = fbdo.intData();
      if (Firebase.getInt(fbdo, "/settings/auto_food")) autoFood = fbdo.intData();
      
      if (Firebase.getString(fbdo, "/commands/fan_mode")) fanMode = fbdo.stringData();
      if (Firebase.getBool(fbdo, "/commands/fan_state")) manualFanState = fbdo.boolData();
      if (Firebase.getBool(fbdo, "/commands/feed_pause")) feedPause = fbdo.boolData();

      // คุมพัดลม
      if (fanMode == "auto") controlFan(temp >= autoTemp);
      else controlFan(manualFanState);

      // คุมอาหาร
      if (!feedPause && foodLevel <= autoFood) {
        feedNow();
      } 

      updateFirebaseStatus();
    } 
    else if (WiFi.status() != WL_CONNECTED) {
      // 🔴 โหมด OFFLINE (เซฟตี้สัตว์เลี้ยง) 🔴
      controlFan(temp >= autoTemp); 
      if (foodLevel <= autoFood) feedNow();
    }
  }

  // 4. เซฟประวัติลงกราฟ (ทุก 5 นาที)
  if (currentMillis - lastHistorySave > historyInterval) {
    lastHistorySave = currentMillis;
    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
      saveHistory(); 
    }
  }
}
