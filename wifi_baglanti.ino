/**
 * ============================================
 *  ESP32-S3 + Wi-Fi + Gemini API Entegrasyonu
 *  Akış: Ses → STT → Gemini → TTS → Hoparlör
 * ============================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <base64.h>

// === Wi-Fi BİLGİLERİ ===
#define WIFI_SSID     "Ağ_Adın"
#define WIFI_PASSWORD "Şifren"

// === API ANAHTARLARI ===
#define GOOGLE_API_KEY  "YOUR_GOOGLE_API_KEY"  // console.cloud.google.com

// === API URL'LERİ ===
#define STT_URL  "https://speech.googleapis.com/v1/speech:recognize?key=" GOOGLE_API_KEY
#define TTS_URL  "https://texttospeech.googleapis.com/v1/text:synthesize?key=" GOOGLE_API_KEY
#define LLM_URL  "https://generativelanguage.googleapis.com/v1beta/models/gemini-pro:generateContent?key=" GOOGLE_API_KEY

// === MİKROFON PİNLERİ ===
#define I2S_WS_PIN     5
#define I2S_SCK_PIN    6
#define I2S_SD_PIN     4
#define MIC_PORT       I2S_NUM_0

// === HOPARLÖR PİNLERİ ===
#define I2S_BCLK_PIN   9
#define I2S_LRC_PIN    10
#define I2S_DIN_PIN    8
#define SPK_PORT       I2S_NUM_1

#define SAMPLE_RATE    16000
#define BUFFER_LENGTH  512

// PSRAM kayıt tamponu
#define MAX_RECORD_SAMPLES (SAMPLE_RATE * 5)
int16_t* recordBuffer = nullptr;
int recordIndex = 0;

int32_t rawBuffer[BUFFER_LENGTH];

// Durum makinesi
typedef enum {
  STATE_IDLE,
  STATE_LISTENING,
  STATE_THINKING,
  STATE_SPEAKING
} SystemState;

SystemState currentState = STATE_IDLE;

// Wake-word ayarları
#define WAKE_THRESHOLD     1500
#define WAKE_CONFIRM_MS    300
#define SILENCE_TIMEOUT_MS 1500

unsigned long wakeStartTime = 0;
unsigned long lastSoundTime = 0;
bool soundDetected = false;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== Sesli Asistan Başlatılıyor ===");

  // PSRAM tampon
  recordBuffer = (int16_t*)ps_malloc(MAX_RECORD_SAMPLES * sizeof(int16_t));
  if (!recordBuffer) {
    Serial.println("HATA: PSRAM bulunamadı!");
    while(1);
  }

  // I2S başlat
  i2s_mic_init();
  i2s_speaker_init();

  // Wi-Fi bağlan
  wifi_connect();

  setState(STATE_IDLE);
}

void loop() {
  size_t bytesRead = 0;
  i2s_read(MIC_PORT, &rawBuffer, sizeof(rawBuffer), &bytesRead, portMAX_DELAY);
  if (bytesRead == 0) return;

  int samplesRead = bytesRead / sizeof(int32_t);
  float rms = calculateRMS(samplesRead);

  switch (currentState) {

    case STATE_IDLE:
      if (rms > WAKE_THRESHOLD) {
        if (!soundDetected) {
          soundDetected = true;
          wakeStartTime = millis();
        }
        if (millis() - wakeStartTime > WAKE_CONFIRM_MS) {
          recordIndex = 0;
          setState(STATE_LISTENING);
        }
      } else {
        soundDetected = false;
      }
      break;

    case STATE_LISTENING:
      if (rms > WAKE_THRESHOLD / 2) lastSoundTime = millis();

      for (int i = 0; i < samplesRead && recordIndex < MAX_RECORD_SAMPLES; i++) {
        recordBuffer[recordIndex++] = (int16_t)(rawBuffer[i] >> 16);
      }

      if (millis() - lastSoundTime > SILENCE_TIMEOUT_MS || recordIndex >= MAX_RECORD_SAMPLES) {
        Serial.printf("Kayıt bitti: %.1f sn\n", (float)recordIndex / SAMPLE_RATE);
        setState(STATE_THINKING);
        processVoiceCommand();  // Ana işlem fonksiyonu
      }
      break;

    case STATE_THINKING:
    case STATE_SPEAKING:
      // Bu durumlar processVoiceCommand() içinde yönetiliyor
      break;
  }
}

// ============================================
//  ANA İŞLEM FONKSİYONU
// ============================================
void processVoiceCommand() {
  // 1. Ses → Metin (STT)
  String transcript = speechToText();
  if (transcript.isEmpty()) {
    Serial.println("STT başarısız, IDLE'a dön");
    setState(STATE_IDLE);
    return;
  }
  Serial.println("Sen dedin: " + transcript);

  // 2. Metin → Gemini → Cevap
  String aiResponse = askGemini(transcript);
  if (aiResponse.isEmpty()) {
    Serial.println("Gemini cevap vermedi");
    setState(STATE_IDLE);
    return;
  }
  Serial.println("Asistan: " + aiResponse);

  // 3. Cevap → Ses (TTS) → Hoparlör
  setState(STATE_SPEAKING);
  textToSpeech(aiResponse);

  setState(STATE_IDLE);
}

// ============================================
//  1. SPEECH TO TEXT
// ============================================
String speechToText() {
  Serial.println("[STT] Ses buluta gönderiliyor...");

  // Ses verisini Base64'e çevir
  String base64Audio = base64::encode(
    (uint8_t*)recordBuffer,
    recordIndex * sizeof(int16_t)
  );

  // JSON body oluştur
  StaticJsonDocument<512> requestDoc;
  requestDoc["config"]["encoding"] = "LINEAR16";
  requestDoc["config"]["sampleRateHertz"] = SAMPLE_RATE;
  requestDoc["config"]["languageCode"] = "tr-TR";  // Türkçe!
  requestDoc["audio"]["content"] = base64Audio;

  String requestBody;
  serializeJson(requestDoc, requestBody);

  // HTTPS isteği gönder
  WiFiClientSecure client;
  client.setInsecure();  // Test için sertifika doğrulama kapat
  HTTPClient http;
  http.begin(client, STT_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);

  int httpCode = http.POST(requestBody);
  String response = "";

  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> responseDoc;
    deserializeJson(responseDoc, payload);

    // Transkripsiyonu al
    if (responseDoc["results"][0]["alternatives"][0]["transcript"].is<String>()) {
      response = responseDoc["results"][0]["alternatives"][0]["transcript"].as<String>();
    }
  } else {
    Serial.printf("[STT] HTTP Hata: %d\n", httpCode);
    Serial.println(http.getString());
  }

  http.end();
  return response;
}

// ============================================
//  2. GEMİNİ LLM
// ============================================
String askGemini(String userText) {
  Serial.println("[Gemini] İstek gönderiliyor...");

  StaticJsonDocument<1024> requestDoc;
  JsonArray contents = requestDoc.createNestedArray("contents");
  JsonObject content = contents.createNestedObject();
  JsonArray parts = content.createNestedArray("parts");
  JsonObject part = parts.createNestedObject();
  part["text"] = "Sen yardımcı bir sesli asistansın. Kısa ve net cevap ver. Kullanıcı: " + userText;

  String requestBody;
  serializeJson(requestDoc, requestBody);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, LLM_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);

  int httpCode = http.POST(requestBody);
  String response = "";

  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<2048> responseDoc;
    deserializeJson(responseDoc, payload);

    if (responseDoc["candidates"][0]["content"]["parts"][0]["text"].is<String>()) {
      response = responseDoc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
    }
  } else {
    Serial.printf("[Gemini] HTTP Hata: %d\n", httpCode);
    Serial.println(http.getString());
  }

  http.end();
  return response;
}

// ============================================
//  3. TEXT TO SPEECH
// ============================================
void textToSpeech(String text) {
  Serial.println("[TTS] Ses sentezleniyor...");

  StaticJsonDocument<512> requestDoc;
  requestDoc["input"]["text"] = text;
  requestDoc["voice"]["languageCode"] = "tr-TR";
  requestDoc["voice"]["name"] = "tr-TR-Wavenet-A";  // Türkçe kadın sesi
  requestDoc["audioConfig"]["audioEncoding"] = "LINEAR16";
  requestDoc["audioConfig"]["sampleRateHertz"] = SAMPLE_RATE;

  String requestBody;
  serializeJson(requestDoc, requestBody);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, TTS_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);

  int httpCode = http.POST(requestBody);

  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<4096> responseDoc;
    deserializeJson(responseDoc, payload);

    if (responseDoc["audioContent"].is<String>()) {
      String base64Audio = responseDoc["audioContent"].as<String>();

      // Base64'ten ham ses verisine çevir
      size_t decodedLen = base64Audio.length() * 3 / 4;
      uint8_t* audioData = (uint8_t*)ps_malloc(decodedLen);

      if (audioData) {
        size_t actualLen = base64::decode(base64Audio, audioData, decodedLen);
        playAudio((int16_t*)audioData, actualLen / sizeof(int16_t));
        free(audioData);
      }
    }
  } else {
    Serial.printf("[TTS] HTTP Hata: %d\n", httpCode);
  }

  http.end();
}

// ============================================
//  HOPARLÖRDEN SES ÇAL
// ============================================
void playAudio(int16_t* audioData, size_t sampleCount) {
  Serial.printf("Çalınıyor: %d sample\n", sampleCount);
  size_t bytesWritten = 0;
  size_t offset = 0;

  while (offset < sampleCount) {
    size_t toWrite = min((size_t)BUFFER_LENGTH, sampleCount - offset);
    i2s_write(SPK_PORT, audioData + offset, toWrite * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
    offset += bytesWritten / sizeof(int16_t);
  }
}

// ============================================
//  WI-FI BAĞLANTI
// ============================================
void wifi_connect() {
  Serial.print("Wi-Fi bağlanıyor: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 30) {
    delay(500);
    Serial.print(".");
    attempt++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nBağlandı! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWi-Fi HATASI! Kontrol et.");
    while(1);
  }
}

// ============================================
//  YARDIMCI FONKSİYONLAR
// ============================================
float calculateRMS(int samplesRead) {
  long long sumSquares = 0;
  for (int i = 0; i < samplesRead; i++) {
    int32_t sample = rawBuffer[i] >> 14;
    sumSquares += (long long)sample * sample;
  }
  return sqrt((float)sumSquares / samplesRead);
}

void setState(SystemState newState) {
  currentState = newState;
  const char* states[] = {"IDLE", "LISTENING", "THINKING", "SPEAKING"};
  Serial.printf("[DURUM] %s\n", states[newState]);
  lastSoundTime = millis();
  soundDetected = false;
}

void i2s_mic_init() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8, .dma_buf_len = BUFFER_LENGTH,
    .use_apll = false, .tx_desc_auto_clear = false, .fixed_mclk = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_SCK_PIN, .ws_io_num = I2S_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = I2S_SD_PIN
  };
  i2s_driver_install(MIC_PORT, &cfg, 0, NULL);
  i2s_set_pin(MIC_PORT, &pins);
  i2s_zero_dma_buffer(MIC_PORT);
}

void i2s_speaker_init() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8, .dma_buf_len = BUFFER_LENGTH,
    .use_apll = false, .tx_desc_auto_clear = true, .fixed_mclk = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_BCLK_PIN, .ws_io_num = I2S_LRC_PIN,
    .data_out_num = I2S_DIN_PIN, .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(SPK_PORT, &cfg, 0, NULL);
  i2s_set_pin(SPK_PORT, &pins);
  i2s_zero_dma_buffer(SPK_PORT);
}