/**
 * ============================================
 *  ESP32-S3 Sesli Asistan - FINAL KOD
 *  Mikrofon + Hoparlör + WiFi + Gemini API
 * ============================================
 * Gerekli kütüphaneler:
 *  - ArduinoJson (Library Manager'dan kur)
 * ============================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>

// ============================================
//  KULLANICI AYARLARI — BUNLARI DEĞİŞTİR!
// ============================================
#define WIFI_SSID        "Ağ_Adın"
#define WIFI_PASSWORD    "Şifren"
#define GOOGLE_API_KEY   "YOUR_GOOGLE_API_KEY"

// ============================================
//  PIN TANIMLAMALARI
// ============================================
// Mikrofon (INMP441)
#define MIC_WS_PIN    5
#define MIC_SCK_PIN   6
#define MIC_SD_PIN    4
#define MIC_PORT      I2S_NUM_0

// Hoparlör (MAX98357A)
#define SPK_BCLK_PIN  9
#define SPK_LRC_PIN   10
#define SPK_DIN_PIN   8
#define SPK_PORT      I2S_NUM_1

// ============================================
//  SABİTLER
// ============================================
#define SAMPLE_RATE          16000
#define BUFFER_LENGTH        512
#define MAX_RECORD_SECONDS   5
#define MAX_RECORD_SAMPLES   (SAMPLE_RATE * MAX_RECORD_SECONDS)

#define WAKE_THRESHOLD       1500
#define WAKE_CONFIRM_MS      300
#define SILENCE_TIMEOUT_MS   1500

// API URL'leri
#define STT_URL "https://speech.googleapis.com/v1/speech:recognize?key=" GOOGLE_API_KEY
#define TTS_URL "https://texttospeech.googleapis.com/v1/text:synthesize?key=" GOOGLE_API_KEY
#define LLM_URL "https://generativelanguage.googleapis.com/v1beta/models/gemini-pro:generateContent?key=" GOOGLE_API_KEY

// ============================================
//  DURUM MAKİNESİ
// ============================================
typedef enum {
  STATE_IDLE,
  STATE_LISTENING,
  STATE_THINKING,
  STATE_SPEAKING
} SystemState;

SystemState currentState = STATE_IDLE;

// ============================================
//  GLOBAL DEĞİŞKENLER
// ============================================
int32_t  rawBuffer[BUFFER_LENGTH];
int16_t* recordBuffer  = nullptr;
int      recordIndex   = 0;

unsigned long wakeStartTime = 0;
unsigned long lastSoundTime = 0;
bool soundDetected = false;

// ============================================
//  BASE64 YARDIMCI FONKSİYONLARI
// ============================================
static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

String base64Encode(uint8_t* data, size_t len) {
  String result = "";
  result.reserve((len / 3 + 1) * 4 + 4);
  for (size_t i = 0; i < len; i += 3) {
    uint8_t b0 = data[i];
    uint8_t b1 = (i+1 < len) ? data[i+1] : 0;
    uint8_t b2 = (i+2 < len) ? data[i+2] : 0;
    result += b64chars[b0 >> 2];
    result += b64chars[((b0 & 3) << 4) | (b1 >> 4)];
    result += (i+1 < len) ? b64chars[((b1 & 15) << 2) | (b2 >> 6)] : '=';
    result += (i+2 < len) ? b64chars[b2 & 63] : '=';
  }
  return result;
}

size_t base64Decode(String input, uint8_t* output) {
  auto decodeChar = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  size_t outLen = 0;
  for (size_t i = 0; i < input.length(); i += 4) {
    int b0 = decodeChar(input[i]);
    int b1 = decodeChar(input[i+1]);
    int b2 = (i+2 < input.length()) ? decodeChar(input[i+2]) : 0;
    int b3 = (i+3 < input.length()) ? decodeChar(input[i+3]) : 0;
    if (b0 < 0 || b1 < 0) break;
    output[outLen++] = (b0 << 2) | (b1 >> 4);
    if (input[i+2] != '=') output[outLen++] = ((b1 & 15) << 4) | (b2 >> 2);
    if (input[i+3] != '=') output[outLen++] = ((b2 & 3) << 6) | b3;
  }
  return outLen;
}

// ============================================
//  SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32-S3 Sesli Asistan ===");

  // PSRAM kayıt tamponu
  recordBuffer = (int16_t*)ps_malloc(MAX_RECORD_SAMPLES * sizeof(int16_t));
  if (!recordBuffer) {
    Serial.println("HATA: PSRAM bulunamadı! Tools > PSRAM > OPI PSRAM seç.");
    while(1);
  }
  Serial.printf("PSRAM hazır: %d saniyelik kayıt kapasitesi\n", MAX_RECORD_SECONDS);

  i2s_mic_init();
  i2s_speaker_init();
  wifi_connect();

  setState(STATE_IDLE);
}

// ============================================
//  ANA DÖNGÜ
// ============================================
void loop() {
  // Wi-Fi kopmuşsa yeniden bağlan
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Bağlantı kesildi, yeniden bağlanıyor...");
    wifi_connect();
  }

  size_t bytesRead = 0;
  i2s_read(MIC_PORT, &rawBuffer, sizeof(rawBuffer), &bytesRead, portMAX_DELAY);
  if (bytesRead == 0) return;

  float rms = calculateRMS(bytesRead / sizeof(int32_t));

  switch (currentState) {
    case STATE_IDLE:
      if (rms > WAKE_THRESHOLD) {
        if (!soundDetected) { soundDetected = true; wakeStartTime = millis(); }
        if (millis() - wakeStartTime > WAKE_CONFIRM_MS) {
          recordIndex = 0;
          setState(STATE_LISTENING);
        }
      } else {
        soundDetected = false;
      }
      break;

    case STATE_LISTENING: {
      if (rms > WAKE_THRESHOLD / 2) lastSoundTime = millis();
      int samplesRead = bytesRead / sizeof(int32_t);
      for (int i = 0; i < samplesRead && recordIndex < MAX_RECORD_SAMPLES; i++) {
        recordBuffer[recordIndex++] = (int16_t)(rawBuffer[i] >> 16);
      }
      bool silenceTimeout = (millis() - lastSoundTime > SILENCE_TIMEOUT_MS);
      bool bufferFull = (recordIndex >= MAX_RECORD_SAMPLES);
      if (silenceTimeout || bufferFull) {
        Serial.printf("Kayıt bitti: %.1f sn\n", (float)recordIndex / SAMPLE_RATE);
        setState(STATE_THINKING);
        processVoiceCommand();
      }
      break;
    }

    case STATE_THINKING:
    case STATE_SPEAKING:
      break;
  }
}

// ============================================
//  ANA İŞLEM FONKSİYONU
// ============================================
void processVoiceCommand() {
  String transcript = speechToText();
  if (transcript.isEmpty()) {
    Serial.println("STT boş döndü, IDLE'a dön");
    setState(STATE_IDLE);
    return;
  }
  Serial.println("Sen: " + transcript);

  String aiResponse = askGemini(transcript);
  if (aiResponse.isEmpty()) {
    Serial.println("Gemini boş döndü");
    setState(STATE_IDLE);
    return;
  }
  Serial.println("Asistan: " + aiResponse);

  setState(STATE_SPEAKING);
  textToSpeech(aiResponse);
  setState(STATE_IDLE);
}

// ============================================
//  SPEECH TO TEXT
// ============================================
String speechToText() {
  Serial.println("[STT] Gönderiliyor...");

  String base64Audio = base64Encode(
    (uint8_t*)recordBuffer,
    recordIndex * sizeof(int16_t)
  );

  // DynamicJsonDocument kullan — büyük ses verisi için
  DynamicJsonDocument requestDoc(base64Audio.length() + 512);
  requestDoc["config"]["encoding"] = "LINEAR16";
  requestDoc["config"]["sampleRateHertz"] = SAMPLE_RATE;
  requestDoc["config"]["languageCode"] = "tr-TR";
  requestDoc["audio"]["content"] = base64Audio;

  String requestBody;
  serializeJson(requestDoc, requestBody);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, STT_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);

  int httpCode = http.POST(requestBody);
  String response = "";

  if (httpCode == 200) {
    DynamicJsonDocument responseDoc(4096);
    deserializeJson(responseDoc, http.getStream());
    if (!responseDoc["results"][0]["alternatives"][0]["transcript"].isNull()) {
      response = responseDoc["results"][0]["alternatives"][0]["transcript"].as<String>();
    }
  } else {
    Serial.printf("[STT] Hata: %d — %s\n", httpCode, http.getString().c_str());
  }
  http.end();
  return response;
}

// ============================================
//  GEMİNİ LLM
// ============================================
String askGemini(String userText) {
  Serial.println("[Gemini] İstek gönderiliyor...");

  DynamicJsonDocument requestDoc(1024);
  JsonArray contents = requestDoc.createNestedArray("contents");
  JsonObject content = contents.createNestedObject();
  JsonArray parts = content.createNestedArray("parts");
  parts.createNestedObject()["text"] =
    "Sen Alex adında yardımcı bir sesli asistansın. "
    "Türkçe, kısa ve net cevap ver. Kullanıcı: " + userText;

  String requestBody;
  serializeJson(requestDoc, requestBody);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, LLM_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(20000);

  int httpCode = http.POST(requestBody);
  String response = "";

  if (httpCode == 200) {
    DynamicJsonDocument responseDoc(8192);
    deserializeJson(responseDoc, http.getStream());
    if (!responseDoc["candidates"][0]["content"]["parts"][0]["text"].isNull()) {
      response = responseDoc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
    }
  } else {
    Serial.printf("[Gemini] Hata: %d — %s\n", httpCode, http.getString().c_str());
  }
  http.end();
  return response;
}

// ============================================
//  TEXT TO SPEECH
// ============================================
void textToSpeech(String text) {
  Serial.println("[TTS] Sentezleniyor...");

  DynamicJsonDocument requestDoc(1024);
  requestDoc["input"]["text"] = text;
  requestDoc["voice"]["languageCode"] = "tr-TR";
  requestDoc["voice"]["name"] = "tr-TR-Wavenet-A";
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
    // Yanıtı doğrudan stream'den oku (RAM tasarrufu)
    DynamicJsonDocument responseDoc(1024);
    // Sadece audioContent alanını al
    String payload = http.getString();
    int start = payload.indexOf("\"audioContent\":\"") + 16;
    int end   = payload.indexOf("\"", start);
    if (start > 16 && end > start) {
      String base64Audio = payload.substring(start, end);
      size_t decodedLen = (base64Audio.length() * 3) / 4;
      uint8_t* audioData = (uint8_t*)ps_malloc(decodedLen);
      if (audioData) {
        size_t actualLen = base64Decode(base64Audio, audioData);
        playAudio((int16_t*)audioData, actualLen / sizeof(int16_t));
        free(audioData);
      } else {
        Serial.println("[TTS] PSRAM yetmedi!");
      }
    }
  } else {
    Serial.printf("[TTS] Hata: %d\n", httpCode);
  }
  http.end();
}

// ============================================
//  SES ÇALMA
// ============================================
void playAudio(int16_t* audioData, size_t sampleCount) {
  Serial.printf("[SPK] Çalınıyor: %d sample (%.1f sn)\n",
    sampleCount, (float)sampleCount / SAMPLE_RATE);
  size_t offset = 0;
  while (offset < sampleCount) {
    size_t toWrite = min((size_t)BUFFER_LENGTH, sampleCount - offset);
    size_t bytesWritten = 0;
    i2s_write(SPK_PORT, audioData + offset, toWrite * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
    offset += bytesWritten / sizeof(int16_t);
  }
}

// ============================================
//  WI-FI
// ============================================
void wifi_connect() {
  Serial.printf("[WiFi] Bağlanıyor: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt++ < 40) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Bağlandı! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] HATA! SSID ve şifreyi kontrol et.");
  }
}

// ============================================
//  YARDIMCI FONKSİYONLAR
// ============================================
float calculateRMS(int samplesRead) {
  long long sum = 0;
  for (int i = 0; i < samplesRead; i++) {
    int32_t s = rawBuffer[i] >> 14;
    sum += (long long)s * s;
  }
  return sqrt((float)sum / samplesRead);
}

void setState(SystemState s) {
  currentState = s;
  const char* names[] = {"IDLE", "LISTENING", "THINKING", "SPEAKING"};
  Serial.printf("[DURUM] >>> %s\n", names[s]);
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
    .bck_io_num = MIC_SCK_PIN, .ws_io_num = MIC_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = MIC_SD_PIN
  };
  i2s_driver_install(MIC_PORT, &cfg, 0, NULL);
  i2s_set_pin(MIC_PORT, &pins);
  i2s_zero_dma_buffer(MIC_PORT);
  Serial.println("[I2S] Mikrofon hazır");
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
    .bck_io_num = SPK_BCLK_PIN, .ws_io_num = SPK_LRC_PIN,
    .data_out_num = SPK_DIN_PIN, .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(SPK_PORT, &cfg, 0, NULL);
  i2s_set_pin(SPK_PORT, &pins);
  i2s_zero_dma_buffer(SPK_PORT);
  Serial.println("[I2S] Hoparlör hazır");
}