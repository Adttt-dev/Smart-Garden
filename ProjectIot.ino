#include <DHT.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ===== WIFI & API CONFIG =====
const char* ssid = "haha";
const char* password = "87654321";
String api_base_url = "http://192.168.39.89:8080/api";
String auth_token = ""; // Will be fetched on startup
String device_id = "4";
bool api_connected = false;

// ===== PIN DEFINITIONS =====
#define DHTPIN 2
#define DHTTYPE DHT22
#define SOIL_PIN 34
#define POMPA_PIN 15
#define TRIG_PIN 5
#define ECHO_PIN 18

// ===== TIMING CONSTANTS =====
const unsigned long SENSOR_READ_INTERVAL = 1000;
const unsigned long DHT_READ_INTERVAL = 5000;
const unsigned long ULTRASONIC_READ_INTERVAL = 2000;
const unsigned long API_SEND_INTERVAL = 5000;
const unsigned long COMMAND_CHECK_INTERVAL = 3000;
const unsigned long SIMPLE_DISPLAY_INTERVAL = 1000;

// ===== PENGATURAN UTAMA =====
struct Settings {
  int soil_dry = 4095, soil_wet = 0;
  float tank_height = 19.0;

  // --- Batas Kelembapan Tanah (%) ---
  float soil_kering_max = 45.0;      // Tanah Kering: 0-45%
  float soil_sedang_min = 40.0;      // Tanah Sedang: 40-65%
  float soil_sedang_max = 65.0;
  float soil_basah_min = 60.0;       // Tanah Basah: 60-75%
  float soil_basah_max = 75.0;
  float soil_sangat_basah_min = 75.0; // Sangat Basah: >75%

  // --- Batas Suhu (°C) ---
  float temp_dingin_max = 31.0;      // Suhu Dingin: ≤31°C
  float temp_normal_min = 29.0;      // Suhu Normal: 29-36°C
  float temp_normal_max = 36.0;
  float temp_panas_min = 34.0;       // Suhu Panas: >34°C

  // --- PWM Pompa ---
  int pump_pwm_med = 140;    // 50% - Sedang
  int pump_pwm_high = 204;   // 80% - Tinggi
  int pump_pwm_max = 255;    // 100% - Maksimal

  // --- Keamanan ---
  float min_water_level = 13.0;
  int max_duration_min = 10;
  int cooldown_min = 3;

  // --- Mode Kontrol ---
  bool auto_mode = true;
  bool debug_mode = false;
  bool simple_display = true;
  bool enable_fuzzy_logic = true;
} settings;

struct PumpState {
  int pwm = 0, percent = 0;
  String status = "OFF";
  String internal_status = "OFF";
  String rule_explanation = "";  // Penjelasan rule yang digunakan
  bool active = false;
  unsigned long start_time = 0, cooldown_start = 0;
  bool in_cooldown = false;
} pump;

// ===== GLOBAL VARIABLES =====
DHT dht(DHTPIN, DHTTYPE);
float temp = 25.0, humidity = 60.0, soil_percent = 0.0;
float water_level = 0.0, water_percent = 0.0;
int soil_raw = 0;
bool water_ok = true;
String system_status = "INIT";

// ===== TIMERS =====
unsigned long last_sensor = 0, last_dht = 0, last_api = 0;
unsigned long last_ultrasonic = 0;
unsigned long last_simple_display = 0;
unsigned long last_command_check = 0;

// ===== API CREDENTIALS =====
const String api_username = "admin";
const String api_password = "adminiot123";
const String api_email = "admin@gmail.com";

// ===== DEKLARASI FUNGSI =====
void printSimpleHeader();
void printSimpleData();
void readSensors();
void readUltrasonicSensor();
float measureDistance();
void updatePump();
void stopPump();
void checkPumpCooldown();
void updateSystemStatus();
void handleSerialCommand();
void calibrateUltrasonic();
void printDetailedStatus();
void printHelp();
void checkServerCommands();
void acknowledgeCommand();
void authenticateAPI();
void sendData();
void applySimpleFuzzyLogic(float soil, float temp);
void calculatePumpClassic();

// ===== FUNGSI KONVERSI STATUS =====
String convertStatusForDatabase(String internal_status) {
  if (internal_status.indexOf("OFF") != -1 || internal_status == "NO_RULE" || 
      internal_status == "COOLDOWN" || internal_status == "TIMEOUT" || 
      internal_status == "MANUAL_OFF") {
    return "OFF";
  }
  if (internal_status.indexOf("MED") != -1) return "MED";
  if (internal_status.indexOf("HIGH") != -1) return "HIGH";
  if (internal_status.indexOf("MAX") != -1 || internal_status == "MANUAL_ON") return "MAX";
  if (internal_status == "NO_WATER") return "NO_WATER";
  return "OFF";
}

void setup() {
  Serial.begin(115200);
  Serial.println("🌱 Smart Irrigation with Simple Fuzzy Logic Starting...");
  
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi Connected! IP: " + WiFi.localIP().toString());
  
  dht.begin();
  ledcAttach(POMPA_PIN, 1000, 8);
  ledcWrite(POMPA_PIN, 0);
  
  pinMode(SOIL_PIN, INPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  
  Serial.println("🔐 Authenticating with API...");
  authenticateAPI();
  
  system_status = "READY";
  Serial.println("🚀 System Ready with Simple Fuzzy Logic!");
  printSimpleHeader();
}

void loop() {
  unsigned long now = millis();
  
  if (now - last_sensor >= SENSOR_READ_INTERVAL) {
    readSensors();
    last_sensor = now;
  }
  
  if (now - last_ultrasonic >= ULTRASONIC_READ_INTERVAL) {
    readUltrasonicSensor();
    last_ultrasonic = now;
  }
  
  if (api_connected && (now - last_command_check >= COMMAND_CHECK_INTERVAL)) {
    checkServerCommands();
    last_command_check = now;
  }

  if (now - last_api >= API_SEND_INTERVAL) {
    if (api_connected) {
      sendData();
    } else {
      authenticateAPI();
    }
    last_api = now;
  }
  
  if (settings.simple_display && now - last_simple_display >= SIMPLE_DISPLAY_INTERVAL) {
    printSimpleData();
    last_simple_display = now;
  }
  
  checkPumpCooldown();
  handleSerialCommand();
  delay(100);
}

// ===== FUZZY LOGIC YANG DISEDERHANAKAN =====
void applySimpleFuzzyLogic(float soil, float temp) {
  if (!settings.enable_fuzzy_logic) {
    calculatePumpClassic();
    return;
  }
  
  // Tentukan kategori tanah
  String soil_category = "";
  if (soil <= settings.soil_kering_max) {
    soil_category = "KERING";
  } else if (soil >= settings.soil_sedang_min && soil <= settings.soil_sedang_max) {
    soil_category = "SEDANG";
  } else if (soil >= settings.soil_basah_min && soil <= settings.soil_basah_max) {
    soil_category = "BASAH";
  } else if (soil > settings.soil_sangat_basah_min) {
    soil_category = "SANGAT_BASAH";
  }
  
  // Tentukan kategori suhu
  String temp_category = "";
  if (temp <= settings.temp_dingin_max) {
    temp_category = "DINGIN";
  } else if (temp >= settings.temp_normal_min && temp <= settings.temp_normal_max) {
    temp_category = "NORMAL";
  } else if (temp > settings.temp_panas_min) {
    temp_category = "PANAS";
  }
  
  // ===== ATURAN FUZZY SEDERHANA =====
  // Format: "Jika [KONDISI TANAH] dan [KONDISI SUHU] maka [AKSI]"
  
  if (soil_category == "KERING" && temp_category == "DINGIN") {
    // Tanah kering + suhu dingin = siram tinggi (tidak perlu terlalu banyak)
    pump.pwm = settings.pump_pwm_high;
    pump.internal_status = "HIGH_FUZZY";
    pump.rule_explanation = "Tanah KERING + Suhu DINGIN → Siram TINGGI (80%)";
    
  } else if (soil_category == "KERING" && temp_category == "NORMAL") {
    // Tanah kering + suhu normal = siram maksimal
    pump.pwm = settings.pump_pwm_max;
    pump.internal_status = "MAX_FUZZY";
    pump.rule_explanation = "Tanah KERING + Suhu NORMAL → Siram MAKSIMAL (100%)";
    
  } else if (soil_category == "KERING" && temp_category == "PANAS") {
    // Tanah kering + suhu panas = siram maksimal (bahaya kekeringan!)
    pump.pwm = settings.pump_pwm_max;
    pump.internal_status = "MAX_FUZZY";
    pump.rule_explanation = "Tanah KERING + Suhu PANAS → Siram MAKSIMAL (100%) - DARURAT!";
    
  } else if (soil_category == "SEDANG" && temp_category == "DINGIN") {
    // Tanah sedang + suhu dingin = siram sedang saja
    pump.pwm = settings.pump_pwm_med;
    pump.internal_status = "MED_FUZZY";
    pump.rule_explanation = "Tanah SEDANG + Suhu DINGIN → Siram SEDANG (50%)";
    
  } else if (soil_category == "SEDANG" && temp_category == "NORMAL") {
    // Tanah sedang + suhu normal = siram tinggi
    pump.pwm = settings.pump_pwm_high;
    pump.internal_status = "HIGH_FUZZY";
    pump.rule_explanation = "Tanah SEDANG + Suhu NORMAL → Siram TINGGI (80%)";
    
  } else if (soil_category == "SEDANG" && temp_category == "PANAS") {
    // Tanah sedang + suhu panas = siram maksimal (evaporasi tinggi)
    pump.pwm = settings.pump_pwm_max;
    pump.internal_status = "MAX_FUZZY";
    pump.rule_explanation = "Tanah SEDANG + Suhu PANAS → Siram MAKSIMAL (100%) - Evaporasi Tinggi";
    
  } else if (soil_category == "BASAH" && temp_category == "DINGIN") {
    // Tanah basah + suhu dingin = jangan siram (bisa busuk akar)
    pump.pwm = 0;
    pump.internal_status = "OFF_FUZZY";
    pump.rule_explanation = "Tanah BASAH + Suhu DINGIN → JANGAN SIRAM - Cegah Busuk Akar";
    
  } else if (soil_category == "BASAH" && temp_category == "NORMAL") {
    // Tanah basah + suhu normal = siram sedikit untuk menjaga
    pump.pwm = settings.pump_pwm_med;
    pump.internal_status = "MED_FUZZY";
    pump.rule_explanation = "Tanah BASAH + Suhu NORMAL → Siram SEDANG (50%) - Maintain";
    
  } else if (soil_category == "BASAH" && temp_category == "PANAS") {
    // Tanah basah + suhu panas = siram tinggi (evaporasi cepat)
    pump.pwm = settings.pump_pwm_high;
    pump.internal_status = "HIGH_FUZZY";
    pump.rule_explanation = "Tanah BASAH + Suhu PANAS → Siram TINGGI (80%) - Antisipasi Evaporasi";
    
  } else if (soil_category == "SANGAT_BASAH") {
    // Tanah sangat basah = jangan siram apapun suhunya
    pump.pwm = 0;
    pump.internal_status = "OFF_FUZZY";
    pump.rule_explanation = "Tanah SANGAT BASAH → JANGAN SIRAM - Hindari Genangan";
    
  } else {
    // Kondisi tidak dikenali
    pump.pwm = 0;
    pump.internal_status = "NO_RULE";
    pump.rule_explanation = "Kondisi Tidak Dikenali - Soil:" + soil_category + " Temp:" + temp_category;
  }
  
  // Update status pompa
  pump.percent = (pump.pwm * 100) / 255;
  pump.active = pump.pwm > 0;
  pump.status = convertStatusForDatabase(pump.internal_status);
  
  // Tampilkan penjelasan rule jika debug mode aktif
  if (settings.debug_mode && pump.rule_explanation.length() > 0) {
    Serial.println("🧠 FUZZY RULE: " + pump.rule_explanation);
  }
}

void calculatePumpClassic() {
  // Logika sederhana tanpa fuzzy
  if (soil_percent <= 46.0) {
    pump.pwm = settings.pump_pwm_max;
    pump.internal_status = "CLASSIC_HIGH";
    pump.rule_explanation = "Mode Klasik: Tanah Kering → Siram Penuh";
  } else if (soil_percent > 46.0 && soil_percent <= 65.0) {
    pump.pwm = settings.pump_pwm_med;
    pump.internal_status = "CLASSIC_MED";
    pump.rule_explanation = "Mode Klasik: Tanah Sedang → Siram Sedang";
  } else {
    pump.pwm = 0;
    pump.internal_status = "CLASSIC_OFF";
    pump.rule_explanation = "Mode Klasik: Tanah Basah → Jangan Siram";
  }
  
  pump.percent = (pump.pwm * 100) / 255;
  pump.active = pump.pwm > 0;
  pump.status = convertStatusForDatabase(pump.internal_status);
  
  if (settings.debug_mode) {
    Serial.println("📊 CLASSIC RULE: " + pump.rule_explanation);
  }
}

// ===== DISPLAY FUNCTIONS =====
void printSimpleHeader() {
  Serial.println("\n" + String('=').substring(0, 85));
  Serial.println("              SMART IRRIGATION - SIMPLE FUZZY LOGIC MONITOR");
  Serial.println(String('=').substring(0, 85));
  Serial.println("Time\t\tTemp\tHumid\tSoil\tWater\tPump Status\tRule Applied");
  Serial.println(String('-').substring(0, 85));
}

void printSimpleData() {
  unsigned long runtime_sec = millis() / 1000;
  int minutes = (runtime_sec / 60) % 60;
  int seconds = runtime_sec % 60;
  
  Serial.printf("%02d:%02d\t\t%.1fC\t%.1f%%\t%.1f%%\t%.1f%%\t%s (%d%%)\t",
                minutes, seconds, temp, humidity, soil_percent, water_percent,
                pump.internal_status.c_str(), pump.percent);
  
  // Tampilkan rule yang sedang aktif (versi singkat)
  if (pump.rule_explanation.length() > 50) {
    Serial.println(pump.rule_explanation.substring(0, 47) + "...");
  } else {
    Serial.println(pump.rule_explanation);
  }
  
  if (pump.in_cooldown) {
    unsigned long remaining = (settings.cooldown_min * 60) - ((millis() - pump.cooldown_start) / 1000);
    Serial.printf("❄️  COOLDOWN: %lu seconds remaining\n", remaining);
  }
}

// ===== SENSOR FUNCTIONS =====
void readSensors() {
  soil_raw = analogRead(SOIL_PIN);
  soil_percent = map(soil_raw, settings.soil_dry, settings.soil_wet, 0, 100);
  soil_percent = constrain(soil_percent, 0, 100);
  
  unsigned long now = millis();
  if (now - last_dht >= DHT_READ_INTERVAL) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      temp = t;
      humidity = h;
    }
    last_dht = now;
  }
  
  if (settings.auto_mode) {
    if (water_ok && !pump.in_cooldown) {
      applySimpleFuzzyLogic(soil_percent, temp);
      updatePump();
    } else if (!water_ok) {
      pump.internal_status = "NO_WATER";
      pump.rule_explanation = "Air Habis - Pompa Dimatikan";
      stopPump();
    } else if (pump.in_cooldown) {
      pump.internal_status = "COOLDOWN";
      pump.rule_explanation = "Pompa Istirahat - Cegah Overheat";
      stopPump();
    }
    pump.status = convertStatusForDatabase(pump.internal_status);
  }
  
  updateSystemStatus();
}

void readUltrasonicSensor() {
  float distance = measureDistance();
  if (distance > 0 && distance <= 50) {
    water_level = settings.tank_height - distance;
    if (water_level < 0) water_level = 0;
    water_percent = (water_level / settings.tank_height) * 100.0;
    water_ok = water_percent >= settings.min_water_level;
    
    if (settings.debug_mode) {
      Serial.printf("🔹 Ultrasonik: Jarak=%.2fcm, Level=%.2fcm, Persen=%.1f%%, Status=%s\n", 
                    distance, water_level, water_percent, water_ok ? "OK" : "LOW");
    }
  }
}

float measureDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 15000);
  if (duration == 0) return -1;
  
  return (duration * 0.0343) / 2.0;
}

void updatePump() {
    if (pump.active) {
        if (pump.start_time == 0) {
            pump.start_time = millis();
        } else {
            unsigned long duration = millis() - pump.start_time;
            if (duration > (unsigned long)settings.max_duration_min * 60000UL) {
                pump.internal_status = "TIMEOUT";
                pump.rule_explanation = "Timeout - Pompa Dipaksa Mati";
                stopPump();
                pump.in_cooldown = true;
                pump.cooldown_start = millis();
                return;
            }
        }
    } else {
        pump.start_time = 0;
    }
    ledcWrite(POMPA_PIN, pump.pwm);
}

void stopPump() {
  pump.pwm = 0;
  pump.percent = 0;
  pump.active = false;
  pump.start_time = 0;
  ledcWrite(POMPA_PIN, 0);
}

void checkPumpCooldown() {
  if (pump.in_cooldown) {
    unsigned long duration = millis() - pump.cooldown_start;
    if (duration > (unsigned long)settings.cooldown_min * 60000UL) {
      pump.in_cooldown = false;
      pump.cooldown_start = 0;
    }
  }
}

void updateSystemStatus() {
  if (!water_ok) system_status = "WATER_LOW";
  else if (pump.in_cooldown) system_status = "COOLDOWN";
  else if (pump.active) system_status = "PUMPING";
  else if (!settings.auto_mode) system_status = "MANUAL_MODE";
  else system_status = "AUTO_MONITOR";
}

// ===== SERIAL COMMANDS =====
void handleSerialCommand() {
  if (Serial.available()) {
    String command = Serial.readString();
    command.trim();
    command.toUpperCase();
    
    if (command == "TOGGLE") {
      settings.simple_display = !settings.simple_display;
      if (settings.simple_display) printSimpleHeader();
    }
    else if (command == "STATUS") printDetailedStatus();
    else if (command == "FUZZY") {
      settings.enable_fuzzy_logic = !settings.enable_fuzzy_logic;
      Serial.println("🧠 Fuzzy Logic: " + String(settings.enable_fuzzy_logic ? "AKTIF" : "NONAKTIF"));
    }
    else if (command == "DEBUG") {
      settings.debug_mode = !settings.debug_mode;
      Serial.println("🔍 Debug Mode: " + String(settings.debug_mode ? "AKTIF" : "NONAKTIF"));
    }
    else if (command == "RULES") printFuzzyRules();
    else if (command == "HELP") printHelp();
    else if (command == "CALIBRATE") calibrateUltrasonic();
  }
}

void printFuzzyRules() {
  Serial.println("\n========== ATURAN FUZZY LOGIC ==========");
  Serial.println("🌱 KONDISI TANAH:");
  Serial.println("   • Kering: 0-45%");
  Serial.println("   • Sedang: 40-65%");
  Serial.println("   • Basah: 60-75%");
  Serial.println("   • Sangat Basah: >75%");
  Serial.println();
  Serial.println("🌡️ KONDISI SUHU:");
  Serial.println("   • Dingin: ≤31°C");
  Serial.println("   • Normal: 29-36°C");
  Serial.println("   • Panas: >34°C");
  Serial.println();
  Serial.println("💧 ATURAN PENYIRAMAN:");
  Serial.println("   1. Tanah KERING + Suhu DINGIN → Siram TINGGI (80%)");
  Serial.println("   2. Tanah KERING + Suhu NORMAL → Siram MAKSIMAL (100%)");
  Serial.println("   3. Tanah KERING + Suhu PANAS → Siram MAKSIMAL (100%) - DARURAT!");
  Serial.println("   4. Tanah SEDANG + Suhu DINGIN → Siram SEDANG (50%)");
  Serial.println("   5. Tanah SEDANG + Suhu NORMAL → Siram TINGGI (80%)");
  Serial.println("   6. Tanah SEDANG + Suhu PANAS → Siram MAKSIMAL (100%)");
  Serial.println("   7. Tanah BASAH + Suhu DINGIN → JANGAN SIRAM");
  Serial.println("   8. Tanah BASAH + Suhu NORMAL → Siram SEDANG (50%)");
  Serial.println("   9. Tanah BASAH + Suhu PANAS → Siram TINGGI (80%)");
  Serial.println("  10. Tanah SANGAT BASAH → JANGAN SIRAM");
  Serial.println("========================================");
}

void calibrateUltrasonic() {
  Serial.println("\n🔧 === KALIBRASI SENSOR ULTRASONIK ===");
  Serial.println("Kosongkan wadah, lalu tekan tombol apa saja...");
  while (!Serial.available()) delay(100);
  Serial.readString();
  
  float empty_distance = 0;
  for (int i = 0; i < 10; i++) {
    float dist = measureDistance();
    if (dist > 0) empty_distance += dist;
    delay(200);
  }
  empty_distance /= 10.0;
  Serial.printf("Jarak saat kosong: %.2fcm\n", empty_distance);
  
  Serial.println("Isi penuh wadah, lalu tekan tombol apa saja...");
  while (!Serial.available()) delay(100);
  Serial.readString();
  
  float full_distance = 0;
  for (int i = 0; i < 10; i++) {
    float dist = measureDistance();
    if (dist > 0) full_distance += dist;
    delay(200);
  }
  full_distance /= 10.0;
  float calculated_height = empty_distance - full_distance;
  Serial.printf("Jarak saat penuh: %.2fcm, Tinggi air terhitung: %.2fcm\n", full_distance, calculated_height);
  
  if (calculated_height > 10 && calculated_height < 20) {
    Serial.printf("✅ Kalibrasi berhasil! Update 'tank_height' di kode menjadi %.2f.\n", calculated_height);
  } else {
    Serial.println("⚠️ Kalibrasi gagal. Cek penempatan sensor.");
  }
}

void printDetailedStatus() {
  Serial.println("\n========== STATUS DETAIL ==========");
  Serial.printf("🌡️  Suhu: %.2f°C\n", temp);
  Serial.printf("💧  Kelembapan Udara: %.2f%%\n", humidity);
  Serial.printf("🌱  Kelembapan Tanah: %.2f%% (Mentah: %d)\n", soil_percent, soil_raw);
  Serial.printf("🪣  Level Air: %.2fcm (%.2f%%) - Tinggi Wadah: %.1fcm\n", water_level, water_percent, settings.tank_height);
  Serial.printf("⚙️  Status Internal Pompa: %s\n", pump.internal_status.c_str());
  Serial.printf("📤  Status Pompa (DB): %s - %d%% (PWM: %d)\n", pump.status.c_str(), pump.percent, pump.pwm);
  Serial.printf("🧠  Rule Aktif: %s\n", pump.rule_explanation.c_str());
  Serial.printf("🔧  Mode Logika: %s\n", settings.enable_fuzzy_logic ? "FUZZY" : "KLASIK");
  Serial.printf("🎮  Mode Kontrol: %s\n", settings.auto_mode ? "AUTO" : "MANUAL");
  Serial.printf("📡  API Terhubung: %s\n", api_connected ? "YA" : "TIDAK");
  Serial.printf("⏰  Waktu Aktif: %lu detik\n", millis() / 1000);
  Serial.println("======================================");
}

void printHelp() {
  Serial.println("\n========== PERINTAH SERIAL ==========");
  Serial.println("TOGGLE    - Ganti mode tampilan");
  Serial.println("STATUS    - Tampilkan status detail");
  Serial.println("FUZZY     - Aktif/nonaktifkan logika fuzzy");
  Serial.println("DEBUG     - Aktif/nonaktifkan mode debug");
  Serial.println("RULES     - Tampilkan semua aturan fuzzy");
  Serial.println("CALIBRATE - Jalankan kalibrasi sensor ultrasonik");
  Serial.println("HELP      - Tampilkan menu ini");
  Serial.println("====================================");
}

// ===== API FUNCTIONS =====
void checkServerCommands() {
  HTTPClient http;
  String url = api_base_url + "/devices/" + device_id;
  http.begin(url);
  http.addHeader("Authorization", "Bearer " + auth_token);

  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      JsonObject device = doc["device"];
      const char* command = device["last_command"];
      bool server_auto_mode = device["auto_mode"];

      if (String(command).isEmpty()) {
        settings.auto_mode = server_auto_mode;
      }

      if (command != nullptr && String(command).length() > 0) {
        Serial.println("-> Perintah diterima: " + String(command));
        
        if (String(command) == "PUMP_ON") {
          settings.auto_mode = false;
          if (water_ok) {
              pump.pwm = settings.pump_pwm_max;
              pump.internal_status = "MANUAL_ON";
              pump.active = true;
              ledcWrite(POMPA_PIN, pump.pwm);
          }
        } else if (String(command) == "PUMP_OFF") {
          settings.auto_mode = false;
          pump.internal_status = "MANUAL_OFF";
          stopPump();
        } else if (String(command) == "AUTO_ON") {
          settings.auto_mode = true;
          pump.internal_status = "SWITCH_TO_AUTO";
        }
        
        acknowledgeCommand(); 
      }
    }
  } else if (code == 401) {
    api_connected = false;
  }
  http.end();
}

void acknowledgeCommand() {
  HTTPClient http;
  String url = api_base_url + "/devices/" + device_id + "/command-ack";
  http.begin(url);
  http.addHeader("Authorization", "Bearer " + auth_token);
  http.addHeader("Content-Type", "application/json");

  int code = http.PUT("{}");
  if (code == 200) {
    Serial.println("<- Perintah berhasil dikonfirmasi.");
  } else {
    Serial.println("<- Gagal konfirmasi perintah, error: " + String(code));
  }
  http.end();
}

void authenticateAPI() {
  HTTPClient http;
  http.begin(api_base_url + "/auth/login");
  http.addHeader("Content-Type", "application/json");
  
  StaticJsonDocument<200> doc;
  doc["username"] = api_username;
  doc["password"] = api_password;
  doc["email"] = api_email;
  
  String payload;
  serializeJson(doc, payload);
  
  int code = http.POST(payload);
  if (code == 200) {
    String response = http.getString();
    StaticJsonDocument<300> resp;
    if (!deserializeJson(resp, response) && resp.containsKey("token")) {
      auth_token = resp["token"].as<String>();
      api_connected = true;
      Serial.println("✅ API Terhubung!");
    }
  } else {
    api_connected = false;
  }
  http.end();
}

void sendData() {
  HTTPClient http;
  http.begin(api_base_url + "/sensor-readings");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + auth_token);
  
  StaticJsonDocument<600> doc;
  doc["device_id"] = device_id.toInt();
  doc["temperature"] = round(temp * 100) / 100.0;
  doc["humidity"] = round(humidity * 100) / 100.0;
  doc["soil_moisture_percent"] = round(soil_percent * 100) / 100.0;
  doc["water_percentage"] = round(water_percent * 100) / 100.0;
  doc["pump_status"] = pump.status;
  doc["pump_percentage"] = pump.percent;
  doc["system_status"] = system_status;
  
  String payload;
  serializeJson(doc, payload);
  
  int code = http.POST(payload);
  if (code == 401) {
    api_connected = false;
    auth_token = "";
  }
  http.end();
}