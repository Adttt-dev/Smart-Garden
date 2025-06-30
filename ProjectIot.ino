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
const unsigned long ULTRASONIC_READ_INTERVAL = 2000; // <--- TAMBAHAN: Interval khusus untuk ultrasonik (2 detik)
const unsigned long API_SEND_INTERVAL = 5000;
const unsigned long COMMAND_CHECK_INTERVAL = 3000;
const unsigned long SIMPLE_DISPLAY_INTERVAL = 1000;

// =================================================================================
// <--- PERUBAHAN DI SINI: PENGATURAN UTAMA SESUAI PERMINTAAN ANDA
// =================================================================================
struct Settings {
  int soil_dry = 4095, soil_wet = 0;
  float tank_height = 19.0;

  // --- Batas Kelembapan Tanah ---
  float soil_dry_max        = 45.0;   // Kering: 0 – 45
  float soil_medium_min     = 40.0;   // Sedang: 40 – 65 (overlap 40–45)
  float soil_medium_max     = 65.0;
  float soil_moist_min      = 60.0;   // Basah: 60 – 75 (overlap 60–65)
  float soil_moist_max      = 75.0;
  float soil_very_wet_min   = 75.0;   // Sangat Basah: >75 (mulai dari >75, tidak tumpang tindih)

  // --- Batas Suhu ---
  float temp_cold_max       = 31.0;   // Dingin: ≤31
  float temp_normal_min     = 29.0;   // Normal: 29 – 36 (overlap 29–31)
  float temp_normal_max     = 36.0;
  float temp_hot_min        = 34.0;   // Panas: >34 (overlap 34–36)

  // --- Pengaturan Pompa dan Keamanan ---
  int pump_pwm_med = 130, pump_pwm_high = 204, pump_pwm_max = 255;
  float min_water_level = 13.0;
  int max_duration_min = 10;
  int cooldown_min = 3;

  // --- Bendera Kontrol (Flag) ---
  bool auto_mode = true;
  bool debug_mode = false;      // <--- PERBAIKAN: Menambahkan kembali debug_mode
  bool simple_display = true;
  bool enable_fuzzy_logic = true;
} settings;
// =================================================================================

struct PumpState {
  int pwm = 0, percent = 0;
  String status = "OFF";
  String internal_status = "OFF";
  String fuzzy_explanation = "";
  bool active = false;
  unsigned long start_time = 0, cooldown_start = 0;
  bool in_cooldown = false;
  float fuzzy_confidence = 0.0;
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
unsigned long last_ultrasonic = 0; // <--- TAMBAHAN: Timer khusus untuk sensor ultrasonik
unsigned long last_simple_display = 0;
unsigned long last_command_check = 0;

// ===== API CREDENTIALS =====
const String api_username = "admin";
const String api_password = "adminiot123";
const String api_email = "admin@gmail.com";

// ===== DEKLARASI FUNGSI (PROTOTYPES) UNTUK MENGHINDARI ERROR =====
void printSimpleHeader();
void printSimpleData();
void readSensors();
void readUltrasonicSensor(); // <--- TAMBAHAN: Fungsi khusus untuk membaca sensor ultrasonik
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
void applyFuzzyRules(float soil, float temp);
void calculatePumpClassic();


// ===== FUNGSI KONVERSI STATUS UNTUK DATABASE =====
String convertStatusForDatabase(String internal_status) {
  if (internal_status.indexOf("OFF") != -1 || internal_status == "NO_RULE" || internal_status == "COOLDOWN" || internal_status == "TIMEOUT" || internal_status == "MANUAL_OFF") {
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
  Serial.println("🌱 Smart Irrigation with Manual Control Starting...");
  
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
  Serial.println("🚀 System Ready with Fuzzy Logic!");
  printSimpleHeader();
}

void loop() {
  unsigned long now = millis();
  
  // Membaca sensor tanah setiap 1 detik (tanpa ultrasonik)
  if (now - last_sensor >= SENSOR_READ_INTERVAL) {
    readSensors();
    last_sensor = now;
  }
  
  // <--- TAMBAHAN: Membaca sensor ultrasonik setiap 2 detik terpisah
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
  handleSerialCommand(); // Memastikan perintah serial dicek setiap loop
  delay(100);
}

// ===== FUZZY LOGIC FUNCTIONS =====
float trapezoidalMembership(float x, float a, float b, float c, float d) {
  if (x <= a || x >= d) return 0.0;
  if (x >= b && x <= c) return 1.0;
  if (x > a && x < b) return (x - a) / (b - a);
  return (d - x) / (d - c);
}

float triangularMembership(float x, float a, float b, float c) {
  if (x <= a || x >= c) return 0.0;
  if (x == b) return 1.0;
  if (x > a && x < b) return (x - a) / (b - a);
  return (c - x) / (c - b);
}

void applyFuzzyRules(float soil, float temp) {
  if (!settings.enable_fuzzy_logic) {
    calculatePumpClassic();
    return;
  }
  
  // Menghitung nilai puncak segitiga secara dinamis
  float soil_sedang_peak = (settings.soil_medium_min + settings.soil_medium_max) / 2.0;
  float soil_basah_peak = (settings.soil_moist_min + settings.soil_moist_max) / 2.0;
  float temp_normal_peak = (settings.temp_normal_min + settings.temp_normal_max) / 2.0;

  // Fungsi Keanggotaan
  float soil_kering = trapezoidalMembership(soil, 0, 0, 35, settings.soil_dry_max);
  float soil_sedang = triangularMembership(soil, settings.soil_medium_min, soil_sedang_peak, settings.soil_medium_max);
  float soil_basah = triangularMembership(soil, settings.soil_moist_min, soil_basah_peak, settings.soil_moist_max);
  float soil_sangat_basah = trapezoidalMembership(soil, settings.soil_very_wet_min, 85, 100, 100);
  
  float temp_dingin = trapezoidalMembership(temp, 0, 0, 25, settings.temp_cold_max);
  float temp_normal = triangularMembership(temp, settings.temp_normal_min, temp_normal_peak, settings.temp_normal_max);
  float temp_panas = trapezoidalMembership(temp, settings.temp_hot_min, 38, 50, 50);
  
  float off_strength = 0.0, med_strength = 0.0, high_strength = 0.0, max_strength = 0.0;
  
  float rules[12] = {
    min(soil_kering, temp_dingin),       // R1 -> HIGH
    min(soil_kering, temp_normal),       // R2 -> MAX
    min(soil_kering, temp_panas),        // R3 -> MAX
    min(soil_sedang, temp_dingin),       // R4 -> MED
    min(soil_sedang, temp_normal),       // R5 -> HIGH
    min(soil_sedang, temp_panas),        // R6 -> MAX
    min(soil_basah, temp_dingin),        // R7 -> OFF
    min(soil_basah, temp_normal),        // R8 -> MED
    min(soil_basah, temp_panas),         // R9 -> HIGH
    min(soil_sangat_basah, temp_dingin), // R10 -> OFF
    min(soil_sangat_basah, temp_normal), // R11 -> OFF
    min(soil_sangat_basah, temp_panas)   // R12 -> OFF
  };
  
  high_strength = max(high_strength, rules[0]);
  max_strength = max(max_strength, max(rules[1], rules[2]));
  med_strength = max(med_strength, max(rules[3], rules[7]));
  high_strength = max(high_strength, max(rules[4], rules[8]));
  max_strength = max(max_strength, rules[5]);
  off_strength = max(off_strength, rules[6]);
  off_strength = max(off_strength, max(rules[9], max(rules[10], rules[11])));
  
  // OPSI 1: Menggunakan logika MAX (winner-takes-all)
  // Pilih output berdasarkan strength tertinggi
  if (max_strength >= high_strength && max_strength >= med_strength && max_strength >= off_strength) {
    pump.pwm = settings.pump_pwm_max;
    pump.internal_status = "MAX_FUZZY";
  } else if (high_strength >= med_strength && high_strength >= off_strength) {
    pump.pwm = settings.pump_pwm_high;
    pump.internal_status = "HIGH_FUZZY";
  } else if (med_strength >= off_strength) {
    pump.pwm = settings.pump_pwm_med;  // SELALU 127 (50%)
    pump.internal_status = "MED_FUZZY";
  } else {
    pump.pwm = 0;
    pump.internal_status = "OFF_FUZZY";
  }
  
  /* OPSI 2: Menggunakan weighted average dengan threshold
  float total_strength = off_strength + med_strength + high_strength + max_strength;
  
  if (total_strength == 0) {
    pump.pwm = 0;
    pump.internal_status = "NO_RULE";
  } else {
    float weighted_pwm = (off_strength * 0 + 
                          med_strength * settings.pump_pwm_med + 
                          high_strength * settings.pump_pwm_high + 
                          max_strength * settings.pump_pwm_max) / total_strength;
    
    // Tentukan kategori berdasarkan threshold
    if (weighted_pwm < 30) {
      pump.pwm = 0;
      pump.internal_status = "OFF_FUZZY";
    } else if (weighted_pwm < 100) {
      pump.pwm = settings.pump_pwm_med;  // TETAP 127 (50%)
      pump.internal_status = "MED_FUZZY";
    } else if (weighted_pwm < 200) {
      pump.pwm = settings.pump_pwm_high; // TETAP 204 (80%)
      pump.internal_status = "HIGH_FUZZY";
    } else {
      pump.pwm = settings.pump_pwm_max;  // TETAP 255 (100%)
      pump.internal_status = "MAX_FUZZY";
    }
  }
  */
  
  pump.percent = (pump.pwm * 100) / 255;
  pump.active = pump.pwm > 0;
  pump.status = convertStatusForDatabase(pump.internal_status);
}


void calculatePumpClassic() {
  // Fungsi ini adalah fallback jika fuzzy logic dimatikan
  // Logikanya tidak mengikuti aturan fuzzy yang baru, tetapi aturan sederhana
  if (soil_percent <= 46.0) { // Kering
    pump.pwm = settings.pump_pwm_max;
  } else if (soil_percent > 46.0 && soil_percent <= 65.0) { // Sedang
    pump.pwm = settings.pump_pwm_med;
  } else { // Basah atau sangat basah
    pump.pwm = 0;
  }
  
  pump.percent = (pump.pwm * 100) / 255;
  pump.active = pump.pwm > 0;
  pump.internal_status = pump.active ? "CLASSIC_ON" : "CLASSIC_OFF";
  pump.status = convertStatusForDatabase(pump.internal_status);
}

// ===== DISPLAY FUNCTIONS (YANG SEBELUMNYA HILANG) =====
void printSimpleHeader() {
  Serial.println("\n" + String('=').substring(0, 80));
  Serial.println("                  SMART IRRIGATION - REALTIME MONITOR");
  Serial.println(String('=').substring(0, 80));
  Serial.println("Time\t\tTemp\tHumid\tSoil\tWater\tPump Status\tSystem");
  Serial.println(String('-').substring(0, 80));
}

void printSimpleData() {
  unsigned long runtime_sec = millis() / 1000;
  int minutes = (runtime_sec / 60) % 60;
  int seconds = runtime_sec % 60;
  
  Serial.printf("%02d:%02d\t\t%.1fC\t%.1f%%\t%.1f%%\t%.1f%%\t%s (%d%%)\t%s\n",
                minutes, seconds, temp, humidity, soil_percent, water_percent,
                pump.internal_status.c_str(), pump.percent, system_status.c_str());
  
  if (pump.in_cooldown) {
    unsigned long remaining = (settings.cooldown_min * 60) - ((millis() - pump.cooldown_start) / 1000);
    Serial.printf("❄️  COOLDOWN: %lu seconds remaining\n", remaining);
  }
}

// ===== SENSOR FUNCTIONS =====
void readSensors() {
  // Membaca sensor tanah dan DHT (TANPA sensor ultrasonik)
  soil_raw = analogRead(SOIL_PIN);
  soil_percent = map(soil_raw, settings.soil_dry, settings.soil_wet, 0, 100);
  soil_percent = constrain(soil_percent, 0, 100);
  
  // Membaca DHT22 dengan interval terpisah
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
  
  // Logika pompa otomatis (menggunakan data water_ok yang sudah ada)
  if (settings.auto_mode) {
    if (water_ok && !pump.in_cooldown) {
      applyFuzzyRules(soil_percent, temp);
      updatePump();
    } else if (!water_ok) {
      pump.internal_status = "NO_WATER";
      stopPump();
    } else if (pump.in_cooldown) {
      pump.internal_status = "COOLDOWN";
      stopPump();
    }
    pump.status = convertStatusForDatabase(pump.internal_status);
  }
  
  updateSystemStatus();
}

// <--- TAMBAHAN: Fungsi khusus untuk membaca sensor ultrasonik setiap 2 detik
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
  } else {
    if (settings.debug_mode) {
      Serial.println("⚠️ Ultrasonik: Gagal membaca atau jarak > 50cm");
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
            pump.start_time = millis(); // Mulai timer saat pompa pertama kali aktif
        } else {
            unsigned long duration = millis() - pump.start_time;
            if (duration > (unsigned long)settings.max_duration_min * 60000UL) {
                pump.internal_status = "TIMEOUT";
                stopPump();
                pump.in_cooldown = true;
                pump.cooldown_start = millis();
                return;
            }
        }
    } else {
        pump.start_time = 0; // Reset timer jika pompa tidak aktif
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
    else if (command == "FUZZY") settings.enable_fuzzy_logic = !settings.enable_fuzzy_logic;
    else if (command == "DEBUG") settings.debug_mode = !settings.debug_mode;
    else if (command == "HELP") printHelp();
    else if (command == "CALIBRATE") calibrateUltrasonic();
  }
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
  Serial.printf("🧠  Mode Logika: %s\n", settings.auto_mode ? "AUTO" : "MANUAL");
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