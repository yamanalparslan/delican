/**
 * ============================================
 *  ESP32-S3 Sesli Asistan - DÜZELTILMIŞ v2
 * ============================================
 * Kütüphane: ArduinoJson (Library Manager)
 * ============================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>

// ============================================
//  KULLANICI AYARLARI
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
#define SAMPLE_RATE          16000
#define BUFFER_LENGTH        512
#define MAX_RECORD_SAMPLES   (SAMPLE_RATE * 5)  // 5 sn

#define WAKE_THRESHOLD       1500
#define WAKE_CONFIRM_MS      300
#define SILENCE_TIMEOUT_MS   1500

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
int16_t* recordBuffer = nullptr;
int      recordIndex  = 0;

unsigned long wakeStartTime = 0;
unsigned long lastSoundTime = 0;
bool soundDetected = false;

// ============================================
//  BASE64 (harici kütüphane gerekmez)
// ============================================
static const char b64chars[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

String base64Encode(const uint8_t* data, size_t len) {
  String out;
  out.reserve(((len + 2) / 3) * 4 + 4);
  for (size_t i = 0; i < len; i += 3) {
    uint8_t b0 = data[i];
    uint8_t b1 = (i + 1 < len) ? data[i + 1] : 0;
    uint8_t b2 = (i + 2 < len) ? data[i + 2] : 0;
    out += b64chars[b0 >> 2];
    out += b64chars[((b0 & 3) << 4) | (b1 >> 4)];
    out += (i + 1 < len) ? b64chars[((b1 & 15) << 2) | (b2 >> 6)] : '=';
    out += (i + 2 < len) ? b64chars[b2 & 63] : '=';
  }
  return out;
}

size_t base64Decode(const String& input, uint8_t* output) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return 0;
  };
  size_t outLen = 0;
  size_t len = input.length();
  for (size_t i = 0; i + 3 < len; i += 4) {
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
  Serial.println("\n=== ESP32-S3 Sesli Asistan v2 ===");

  recordBuffer = (int16_t*)ps_malloc(MAX_RECORD_SAMPLES * sizeof(int16_t));
  if (!recordBuffer) {
    Serial.println("HATA: PSRAM bulunamadı! Tools > PSRAM > OPI PSRAM seç.");
    while (1);
  }
  Serial.println("PSRAM tampon hazır.");

  i2s_mic_init();
  i2s_speaker_init();
  wifi_connect();
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
//  ANA İŞLEM
// ============================================
void processVoiceCommand() {
  // Wi-Fi kontrolü
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Bağlantı yok, yeniden bağlanıyor...");
    wifi_connect();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Bağlanamadı, IDLE'a dön.");
      setState(STATE_IDLE);
      return;
    }
  }

  String transcript = speechToText();
  if (transcript.isEmpty()) {
    Serial.println("[STT] Boş döndü, IDLE'a dön.");
    setState(STATE_IDLE);
    return;
  }
  Serial.println("Sen: " + transcript);

  String aiResponse = askGemini(transcript);
  if (aiResponse.isEmpty()) {
    Serial.println("[Gemini] Boş döndü.");
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

  // Sesi base64'e çevir
  String b64 = base64Encode(
    (const uint8_t*)recordBuffer,
    recordIndex * sizeof(int16_t)
  );

  // JSON'u parça parça oluştur — büyük String birleştirmesinden kaçın
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, STT_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);

  // Manuel JSON (DynamicJsonDocument base64 String'i taşıyamaz)
  String body = "{\"config\":{\"encoding\":\"LINEAR16\","
                "\"sampleRateHertz\":" + String(SAMPLE_RATE) + ","
                "\"languageCode\":\"tr-TR\"},"
                "\"audio\":{\"content\":\"" + b64 + "\"}}";

  int code = http.POST(body);
  String response = "";

  if (code == 200) {
    // Stream'den oku — getString() kullanma!
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, http.getStream());
    if (!err && !doc["results"][0]["alternatives"][0]["transcript"].isNull()) {
      response = doc["results"][0]["alternatives"][0]["transcript"].as<String>();
    } else {
      Serial.println("[STT] JSON parse hatası veya boş sonuç.");
    }
  } else {
    Serial.printf("[STT] HTTP Hata: %d\n", code);
  }

  http.end();
  return response;
}

// ============================================
//  GEMİNİ LLM
// ============================================
String askGemini(const String& userText) {
  Serial.println("[Gemini] İstek gönderiliyor...");

  // Sistem promptunu sabit tut, userText'i birleştir
  String prompt = "Sen Alex adinda yardimci bir sesli asistansin. "
                  "Turkce, kisa ve net cevap ver. Kullanici: ";
  prompt += userText;

  String body = "{\"contents\":[{\"parts\":[{\"text\":\""
                + prompt + "\"}]}]}";

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
    if (!err && !doc["candidates"][0]["content"]["parts"][0]["text"].isNull()) {
      response = doc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
    } else {
      Serial.println("[Gemini] JSON parse hatası.");
    }
  } else {
    Serial.printf("[Gemini] HTTP Hata: %d\n", code);
  }

  http.end();
  return response;
}

// ============================================
//  TEXT TO SPEECH — Streaming ile güvenli
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

  if (code == 200) {
    // Sadece audioContent değerini String olarak çek
    // getStream() yerine getString() — TTS yanıtı genelde 50-200KB
    String payload = http.getString();

    // "audioContent":"......." kısmını bul
    int start = payload.indexOf("\"audioContent\":\"") + 16;
    int end   = payload.indexOf('"', start);

    if (start > 16 && end > start) {
      String b64audio = payload.substring(start, end);
      Serial.printf("[TTS] Base64 boyutu: %d karakter\n", b64audio.length());

      // PSRAM'da decode et
      size_t maxDecoded = (b64audio.length() * 3) / 4;
      uint8_t* audioBuf = (uint8_t*)ps_malloc(maxDecoded);

      if (audioBuf) {
        size_t actualLen = base64Decode(b64audio, audioBuf);
        Serial.printf("[TTS] Decode edildi: %d byte\n", actualLen);
        playAudio((int16_t*)audioBuf, actualLen / sizeof(int16_t));
        free(audioBuf);
      } else {
        Serial.println("[TTS] HATA: PSRAM yetersiz! Ses çalınamadı.");
      }
    } else {
      Serial.println("[TTS] audioContent bulunamadı.");
    }
  } else {
    Serial.printf("[TTS] HTTP Hata: %d\n", code);
  }

  http.end();
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
    i2s_write(SPK_PORT, audioData + offset, toWrite * sizeof(int16_t), &written, portMAX_DELAY);
    if (written == 0) break;  // Hata koruması
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
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Bağlandı! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[WiFi] HATA! SSID/şifre kontrol et.");
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
  if (s == STATE_IDLE || s == STATE_LISTENING) {
    lastSoundTime = millis();
    soundDetected = false;
  }
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
  Serial.println("[I2S] Mikrofon hazır.");
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
  Serial.println("[I2S] Hoparlör hazır.");
}