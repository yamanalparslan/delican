/**
 * ============================================
 *  ESP32-S3 Sesli Asistan - FINAL v3
 * ============================================
 *  Donanım:
 *   - ESP32-S3 DevKit (8MB PSRAM)
 *   - INMP441 I2S Mikrofon
 *   - MAX98357A I2S Hoparlör Amplifikatör
 *
 *  Gerekli kütüphane:
 *   - ArduinoJson (Library Manager'dan kur)
 *
 *  Bağlantı:
 *   INMP441 → VDD:3.3V, GND, SD:GPIO4, WS:GPIO5, SCK:GPIO6, L/R:GND
 *   MAX98357A → VIN:3.3V, GND, DIN:GPIO8, BCLK:GPIO9, LRC:GPIO10
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
#define WIFI_SSID       "Ağ_Adın"
#define WIFI_PASSWORD   "Şifren"
#define GOOGLE_API_KEY  "YOUR_GOOGLE_API_KEY"

// ============================================
//  PIN TANIMLAMALARI
// ============================================
#define MIC_WS_PIN    5
#define MIC_SCK_PIN   6
#define MIC_SD_PIN    4
#define MIC_PORT      I2S_NUM_0

#define SPK_BCLK_PIN  9
#define SPK_LRC_PIN   10
#define SPK_DIN_PIN   8
#define SPK_PORT      I2S_NUM_1

// ============================================
//  SABİTLER
// ============================================
#define SAMPLE_RATE           16000
#define BUFFER_LENGTH         512
#define MAX_RECORD_SAMPLES    (SAMPLE_RATE * 5)
#define WAKE_THRESHOLD        1500
#define WAKE_CONFIRM_MS       300
#define SILENCE_TIMEOUT_MS    1500

#define STT_URL "https://speech.googleapis.com/v1/speech:recognize?key=" GOOGLE_API_KEY
#define TTS_URL "https://texttospeech.googleapis.com/v1/text:synthesize?key=" GOOGLE_API_KEY
#define LLM_URL "https://generativelanguage.googleapis.com/v1beta/models/gemini-1.5-flash:generateContent?key=" GOOGLE_API_KEY

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
int16_t* recordBuffer = nullptr;
int      recordIndex  = 0;

unsigned long wakeStartTime = 0;
unsigned long lastSoundTime = 0;
bool soundDetected = false;

// ============================================
//  BASE64
// ============================================
static const char b64chars[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void base64EncodeToBuf(const uint8_t* data, size_t len, char* out) {
  size_t pos = 0;
  for (size_t i = 0; i < len; i += 3) {
    uint8_t b0 = data[i];
    uint8_t b1 = (i + 1 < len) ? data[i + 1] : 0;
    uint8_t b2 = (i + 2 < len) ? data[i + 2] : 0;
    out[pos++] = b64chars[b0 >> 2];
    out[pos++] = b64chars[((b0 & 3) << 4) | (b1 >> 4)];
    out[pos++] = (i + 1 < len) ? b64chars[((b1 & 15) << 2) | (b2 >> 6)] : '=';
    out[pos++] = (i + 2 < len) ? b64chars[b2 & 63] : '=';
  }
  out[pos] = '\0';
}

size_t base64Decode(const char* input, size_t inputLen, uint8_t* output) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return 0;
  };
  size_t outLen = 0;
  for (size_t i = 0; i + 3 < inputLen; i += 4) {
    int v0 = val(input[i]);
    int v1 = val(input[i + 1]);
    int v2 = val(input[i + 2]);
    int v3 = val(input[i + 3]);
    output[outLen++] = (v0 << 2) | (v1 >> 4);
    if (input[i + 2] != '=') output[outLen++] = ((v1 & 15) << 4) | (v2 >> 2);
    if (input[i + 3] != '=') output[outLen++] = ((v2 & 3) << 6) | v3;
  }
  return outLen;
}

// ============================================
//  SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32-S3 Sesli Asistan v3 ===");

  recordBuffer = (int16_t*)ps_malloc(MAX_RECORD_SAMPLES * sizeof(int16_t));
  if (!recordBuffer) {
    Serial.println("HATA: PSRAM bulunamadı! Tools > PSRAM > OPI PSRAM seç.");
    while (1);
  }
  Serial.printf("PSRAM tampon hazır: %d KB\n",
    (MAX_RECORD_SAMPLES * sizeof(int16_t)) / 1024);

  i2s_mic_init();
  i2s_speaker_init();
  wifi_connect();

  Serial.println("\n[Sistem] Hazır! Konuşmak için ses çıkar.");
  setState(STATE_IDLE);
}

// ============================================
//  ANA DÖNGÜ
// ============================================
void loop() {
  size_t bytesRead = 0;
  i2s_read(MIC_PORT, &rawBuffer, sizeof(rawBuffer), &bytesRead, portMAX_DELAY);
  if (bytesRead == 0) return;

  float rms = calculateRMS(bytesRead / sizeof(int32_t));

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

    case STATE_LISTENING: {
      if (rms > WAKE_THRESHOLD / 2) lastSoundTime = millis();

      int samplesRead = bytesRead / sizeof(int32_t);
      for (int i = 0; i < samplesRead && recordIndex < MAX_RECORD_SAMPLES; i++) {
        recordBuffer[recordIndex++] = (int16_t)(rawBuffer[i] >> 16);
      }

      bool silenceEnd = (millis() - lastSoundTime > SILENCE_TIMEOUT_MS);
      bool bufferFull = (recordIndex >= MAX_RECORD_SAMPLES);

      if (silenceEnd || bufferFull) {
        Serial.printf("[Kayıt] Bitti: %.1f sn\n", (float)recordIndex / SAMPLE_RATE);
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
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Bağlantı yok, yeniden deneniyor...");
    wifi_connect();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Bağlanamadı, IDLE'a dönülüyor.");
      setState(STATE_IDLE);
      return;
    }
  }

  String transcript = speechToText();
  if (transcript.isEmpty()) {
    Serial.println("[STT] Anlaşılamadı.");
    setState(STATE_IDLE);
    return;
  }
  Serial.println("Sen     : " + transcript);

  String aiResponse = askGemini(transcript);
  if (aiResponse.isEmpty()) {
    Serial.println("[Gemini] Cevap alınamadı.");
    setState(STATE_IDLE);
    return;
  }
  Serial.println("Asistan : " + aiResponse);

  setState(STATE_SPEAKING);
  textToSpeech(aiResponse);
  setState(STATE_IDLE);
}

// ============================================
//  SPEECH TO TEXT — PSRAM tabanlı
// ============================================
String speechToText() {
  Serial.println("[STT] Gönderiliyor...");

  size_t audioBytes = recordIndex * sizeof(int16_t);
  size_t b64Len = ((audioBytes + 2) / 3) * 4 + 1;

  char* b64Buf = (char*)ps_malloc(b64Len);
  if (!b64Buf) {
    Serial.println("[STT] HATA: PSRAM base64 yetersiz!");
    return "";
  }
  base64EncodeToBuf((const uint8_t*)recordBuffer, audioBytes, b64Buf);

  size_t bodyLen = b64Len + 256;
  char* bodyBuf = (char*)ps_malloc(bodyLen);
  if (!bodyBuf) {
    Serial.println("[STT] HATA: PSRAM body yetersiz!");
    free(b64Buf);
    return "";
  }

  snprintf(bodyBuf, bodyLen,
    "{\"config\":{"
      "\"encoding\":\"LINEAR16\","
      "\"sampleRateHertz\":%d,"
      "\"languageCode\":\"tr-TR\""
    "},\"audio\":{\"content\":\"%s\"}}",
    SAMPLE_RATE, b64Buf
  );
  free(b64Buf);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, STT_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);

  int code = http.POST((uint8_t*)bodyBuf, strlen(bodyBuf));
  free(bodyBuf);

  String response = "";
  if (code == 200) {
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, http.getStream());
    if (!err) {
      auto t = doc["results"][0]["alternatives"][0]["transcript"];
      if (!t.isNull()) response = t.as<String>();
      else Serial.println("[STT] Transkript boş.");
    } else {
      Serial.printf("[STT] JSON hatası: %s\n", err.c_str());
    }
  } else {
    Serial.printf("[STT] HTTP Hata: %d\n", code);
  }

  http.end();
  return response;
}

// ============================================
//  GEMİNİ 1.5 FLASH
// ============================================
String askGemini(const String& userText) {
  Serial.println("[Gemini] İstek gönderiliyor...");

  String body = "{\"contents\":[{\"parts\":[{\"text\":"
                "\"Sen Alex adinda Turkce konusan yardimci bir sesli asistansin. "
                "Cevaplarini kisa ve net tut. Kullanici: "
                + userText + "\"}]}]}";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, LLM_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(20000);

  int code = http.POST(body);
  String response = "";

  if (code == 200) {
    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, http.getStream());
    if (!err) {
      auto t = doc["candidates"][0]["content"]["parts"][0]["text"];
      if (!t.isNull()) {
        response = t.as<String>();
        if (response.length() > 500) response = response.substring(0, 500);
      }
    } else {
      Serial.printf("[Gemini] JSON hatası: %s\n", err.c_str());
    }
  } else {
    Serial.printf("[Gemini] HTTP Hata: %d\n", code);
  }

  http.end();
  return response;
}

// ============================================
//  TEXT TO SPEECH — Stream ile PSRAM'a
// ============================================
void textToSpeech(const String& text) {
  Serial.println("[TTS] Sentezleniyor...");

  String body = "{\"input\":{\"text\":\"" + text + "\"},"
                "\"voice\":{\"languageCode\":\"tr-TR\","
                "\"name\":\"tr-TR-Wavenet-A\"},"
                "\"audioConfig\":{\"audioEncoding\":\"LINEAR16\","
                "\"sampleRateHertz\":" + String(SAMPLE_RATE) + "}}";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, TTS_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);

  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("[TTS] HTTP Hata: %d\n", code);
    http.end();
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  const String token = "\"audioContent\":\"";
  String searchBuf = "";
  bool found = false;
  unsigned long timeout = millis() + 10000;

  while (millis() < timeout) {
    if (stream->available()) {
      char c = stream->read();
      searchBuf += c;
      if (searchBuf.length() > (unsigned)token.length())
        searchBuf.remove(0, 1);
      if (searchBuf == token) { found = true; break; }
    }
  }

  if (!found) {
    Serial.println("[TTS] audioContent bulunamadı!");
    http.end();
    return;
  }

  const size_t maxB64 = 300 * 1024;
  char* b64Buf = (char*)ps_malloc(maxB64);
  if (!b64Buf) {
    Serial.println("[TTS] HATA: PSRAM yetersiz!");
    http.end();
    return;
  }

  size_t b64Pos = 0;
  timeout = millis() + 15000;
  while (millis() < timeout && b64Pos < maxB64 - 1) {
    if (stream->available()) {
      char c = stream->read();
      if (c == '"') break;
      b64Buf[b64Pos++] = c;
    }
  }
  b64Buf[b64Pos] = '\0';
  Serial.printf("[TTS] Base64 alındı: %d KB\n", b64Pos / 1024);
  http.end();

  size_t maxDecoded = (b64Pos * 3) / 4;
  uint8_t* audioBuf = (uint8_t*)ps_malloc(maxDecoded);
  if (!audioBuf) {
    Serial.println("[TTS] HATA: Decode için PSRAM yetersiz!");
    free(b64Buf);
    return;
  }

  size_t actualLen = base64Decode(b64Buf, b64Pos, audioBuf);
  free(b64Buf);

  Serial.printf("[TTS] Ses hazır: %.1f sn\n",
    (float)(actualLen / sizeof(int16_t)) / SAMPLE_RATE);

  playAudio((int16_t*)audioBuf, actualLen / sizeof(int16_t));
  free(audioBuf);
}

// ============================================
//  SES ÇALMA
// ============================================
void playAudio(int16_t* audioData, size_t sampleCount) {
  Serial.printf("[SPK] Çalınıyor: %.1f sn\n", (float)sampleCount / SAMPLE_RATE);
  size_t offset = 0;
  while (offset < sampleCount) {
    size_t toWrite = min((size_t)BUFFER_LENGTH, sampleCount - offset);
    size_t written = 0;
    i2s_write(SPK_PORT, audioData + offset,
              toWrite * sizeof(int16_t), &written, portMAX_DELAY);
    if (written == 0) break;
    offset += written / sizeof(int16_t);
  }
}

// ============================================
//  WI-FI
// ============================================
void wifi_connect() {
  Serial.printf("[WiFi] Bağlanıyor: %s\n", WIFI_SSID);
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt++ < 40) {
    delay(500); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("[WiFi] Bağlandı! IP: %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("[WiFi] HATA! SSID/şifre kontrol et.");
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
  Serial.printf("\n[DURUM] >>> %s\n", names[s]);
  if (s == STATE_IDLE || s == STATE_LISTENING) {
    lastSoundTime = millis();
    soundDetected = false;
  }
}

void i2s_mic_init() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = BUFFER_LENGTH,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = MIC_SCK_PIN,
    .ws_io_num    = MIC_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = MIC_SD_PIN
  };
  i2s_driver_install(MIC_PORT, &cfg, 0, NULL);
  i2s_set_pin(MIC_PORT, &pins);
  i2s_zero_dma_buffer(MIC_PORT);
  Serial.println("[I2S] Mikrofon hazır.");
}

void i2s_speaker_init() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = BUFFER_LENGTH,
    .use_apll             = false,
    .tx_desc_auto_clear   = true,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = SPK_BCLK_PIN,
    .ws_io_num    = SPK_LRC_PIN,
    .data_out_num = SPK_DIN_PIN,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(SPK_PORT, &cfg, 0, NULL);
  i2s_set_pin(SPK_PORT, &pins);
  i2s_zero_dma_buffer(SPK_PORT);
  Serial.println("[I2S] Hoparlör hazır.");
}