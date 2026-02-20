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

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h> // Kalıcı hafıza için
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h> // WiFi Manager kütüphanesi (tzapu)
#include <driver/i2s.h>

#include "config.h"

// ============================================
//  WAKE WORD AYARLARI (ESP-SR)
// ============================================
// Eğer kütüphane yüklü değilse bu satırı yorum satırı yapın:
// #define USE_WAKE_WORD

#ifdef USE_WAKE_WORD
#include <ESP_I2S.h>
#include <dl_lib_coefgetter_if.h>
#include <esp_wn_iface.h>
#include <esp_wn_models.h>
#endif

// ============================================
//  PIN TANIMLAMALARI
// ============================================
#define MIC_WS_PIN 5
#define MIC_SCK_PIN 6
#define MIC_SD_PIN 4
#define MIC_PORT I2S_NUM_0

#define SPK_BCLK_PIN 9
#define SPK_LRC_PIN 10
#define SPK_DIN_PIN 8
#define SPK_PORT I2S_NUM_1

// ============================================
//  SABİTLER
// ============================================
#define SAMPLE_RATE 16000
#define BUFFER_LENGTH 512
#define MAX_RECORD_SAMPLES (SAMPLE_RATE * 5)
#define WAKE_THRESHOLD 1500
#define WAKE_CONFIRM_MS 300
#define SILENCE_TIMEOUT_MS 1500

#define STT_URL_BASE "https://speech.googleapis.com/v1/speech:recognize?key="
#define TTS_URL_BASE                                                           \
  "https://texttospeech.googleapis.com/v1/text:synthesize?key="
#define LLM_URL_BASE                                                           \
  "https://generativelanguage.googleapis.com/v1beta/models/"                   \
  "gemini-1.5-flash:generateContent?key="

// Not: URL'leri dinamik oluşturacağız, bu yüzden String birleştirme yapacağız.

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
int32_t rawBuffer[BUFFER_LENGTH];
int16_t *recordBuffer = nullptr;
int recordIndex = 0;

// ============================================
//  FONKSİYON PROTOTİPLERİ (Forward Declarations)
// ============================================
void i2s_mic_init();
void i2s_speaker_init();
void wifi_connect();
void setState(SystemState s);
void processVoiceCommand();
String speechToText();
String askGemini(const String &userText);
void textToSpeech(const String &text);
void playAudio(int16_t *audioData, size_t sampleCount);
float calculateRMS(int samplesRead);
bool detectWakeWord(int32_t *buffer, size_t length);

unsigned long wakeStartTime = 0;
unsigned long lastSoundTime = 0;
bool soundDetected = false;

// ============================================
//  AYARLAR (NVS)
// ============================================
Preferences preferences;
char googleApiKey[100] = "";
char picovoiceKey[100] = "";
char webhookUrl[150] = "";

// Varsayılan değerler config.h içinden gelmeyebilir, boş bırakıyoruz.
// Kullanıcı arayüzden girecek.

// ============================================
//  AYARLAR (Memory/History)
// ============================================
#define MAX_HISTORY_TURNS 4 // Son 2 soru-cevap (User, Model, User, Model)
struct ChatMessage {
  String role; // "user" veya "model"
  String text;
};
ChatMessage chatHistory[MAX_HISTORY_TURNS];
int historyCount = 0;

// ============================================
//  AYARLAR (LED)
// ============================================
#define LED_PIN 17 // Kullanıcı isteği üzerine GPIO 17
#define LED_COUNT 1
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

void handleLedEffects(); // Loop içinde çağrılacak

// ============================================
//  BASE64
// ============================================
static const char b64chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void base64EncodeToBuf(const uint8_t *data, size_t len, char *out) {
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

void executeSmartHomeCommand(String action, String device);

size_t base64Decode(const char *input, size_t inputLen, uint8_t *output) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z')
      return c - 'A';
    if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
    if (c >= '0' && c <= '9')
      return c - '0' + 52;
    if (c == '+')
      return 62;
    if (c == '/')
      return 63;
    return 0;
  };
  size_t outLen = 0;
  for (size_t i = 0; i + 3 < inputLen; i += 4) {
    int v0 = val(input[i]);
    int v1 = val(input[i + 1]);
    int v2 = val(input[i + 2]);
    int v3 = val(input[i + 3]);
    output[outLen++] = (v0 << 2) | (v1 >> 4);
    if (input[i + 2] != '=')
      output[outLen++] = ((v1 & 15) << 4) | (v2 >> 2);
    if (input[i + 3] != '=')
      output[outLen++] = ((v2 & 3) << 6) | v3;
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

  strip.begin();
  strip.setBrightness(50); // %20 parlaklık yeterli
  strip.show();            // Söndür (siyah)

  recordBuffer = (int16_t *)ps_malloc(MAX_RECORD_SAMPLES * sizeof(int16_t));
  if (!recordBuffer) {
    Serial.println("HATA: PSRAM bulunamadı! Tools > PSRAM > OPI PSRAM seç.");
    while (1)
      ;
  }
  Serial.printf("PSRAM tampon hazır: %d KB\n",
                (MAX_RECORD_SAMPLES * sizeof(int16_t)) / 1024);

  i2s_mic_init();
  i2s_speaker_init();
  wifi_connect();

#ifdef USE_WAKE_WORD
  pv_status_t status = pv_porcupine_init(
      PICOVOICE_ACCESS_KEY, sizeof(KEYWORD_MODEL), KEYWORD_MODEL,
      0.5f, // Hassasiyet (0..1)
      memory_buffer, MEMORY_BUFFER_SIZE, &porcupine);

  if (status != PV_STATUS_SUCCESS) {
    Serial.printf("[Porcupine] Init Hatası: %s\n", pv_status_to_string(status));
  } else {
    Serial.println("[Porcupine] 'Hey Alex' motoru hazır!");
  }
#endif

  Serial.println("\n[Sistem] Hazır! Konuşmak için ses çıkar.");
  setState(STATE_IDLE);
}

// ============================================
//  ANA DÖNGÜ
// ============================================
void loop() {
  handleLedEffects(); // LED animasyonlarını güncelle

  size_t bytesRead = 0;
  i2s_read(MIC_PORT, &rawBuffer, sizeof(rawBuffer), &bytesRead, portMAX_DELAY);
  if (bytesRead == 0)
    return;

  float rms = calculateRMS(bytesRead / sizeof(int32_t));

  switch (currentState) {

  case STATE_IDLE:
#ifdef USE_WAKE_WORD
    // Wake Word (Uyandırma Kelimesi) Kontrolü
    // Bu kısım ESP-SR kütüphanesi gerektirir.
    // Şimdilik simülasyon veya placeholder kodu:
    if (detectWakeWord(rawBuffer, bytesRead)) {
      Serial.println("[WakeWord] 'Hi ESP' algılandı!");
      recordIndex = 0;
      setState(STATE_LISTENING);
    }
#else
    // Eski RMS (Ses Şiddeti) Yöntemi
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
#endif
    break;

  case STATE_LISTENING: {
    if (rms > WAKE_THRESHOLD / 2)
      lastSoundTime = millis();

    int samplesRead = bytesRead / sizeof(int32_t);
    for (int i = 0; i < samplesRead && recordIndex < MAX_RECORD_SAMPLES; i++) {
      recordBuffer[recordIndex++] = (int16_t)(rawBuffer[i] >> 16);
    }

    bool silenceEnd = (millis() - lastSoundTime > SILENCE_TIMEOUT_MS);
    bool bufferFull = (recordIndex >= MAX_RECORD_SAMPLES);

    if (silenceEnd || bufferFull) {
      Serial.printf("[Kayıt] Bitti: %.1f sn\n",
                    (float)recordIndex / SAMPLE_RATE);
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
  Serial.println("Asistan Ham Cevap: " + aiResponse);

  // --- GEÇMİŞİ GÜNCELLE ---
  // Kaydırma (Shift) yap: dizi doluysa en eskiyi sil
  if (historyCount >= MAX_HISTORY_TURNS) {
    for (int i = 0; i < MAX_HISTORY_TURNS - 2; i++) {
      chatHistory[i] = chatHistory[i + 2]; // 2 birim kaydır (User+Model çifti)
    }
    historyCount -= 2;
  }
  // User ekle
  if (historyCount < MAX_HISTORY_TURNS) {
    chatHistory[historyCount].role = "user";
    chatHistory[historyCount].text = transcript;
    historyCount++;
  }
  // Model ekle (JSON değilse)

  // JSON kontrolü (Basitçe '{' ile başlıyorsa JSON kabul ediyoruz)
  aiResponse.trim();
  if (aiResponse.startsWith("{") && aiResponse.endsWith("}")) {
    // ... Smart Home kodu aynen kalıyor ...
    // Smart home komutlarını geçmişe eklemek istemeyebiliriz veya
    // ekleyebiliriz. Şimdilik eklemiyoruz, çünkü bağlamı bozabilir.
    Serial.println("[Gemini] Akıllı Ev Komutu Algılandı!");

    DynamicJsonDocument cmdDoc(1024);
    DeserializationError error = deserializeJson(cmdDoc, aiResponse);

    if (!error) {
      String cmd = cmdDoc["cmd"].as<String>();
      String device = cmdDoc["device"].as<String>();
      String speech = cmdDoc["speech"].as<String>();

      if (speech.isEmpty())
        speech = "Tamam, hallediyorum.";

      // Webhook tetikle
      executeSmartHomeCommand(cmd, device);

      // Onay konuşması yap
      setState(STATE_SPEAKING);
      textToSpeech(speech);
      setState(STATE_IDLE);
      return;
    } else {
      Serial.println("[Gemini] JSON Parse Hatası! Normal konuşma yapılıyor.");
    }
  }

  // Normal cevabı geçmişe ekle
  if (historyCount < MAX_HISTORY_TURNS) {
    chatHistory[historyCount].role = "model";
    chatHistory[historyCount].text = aiResponse;
    historyCount++;
  }

  // Normal konuşma
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

  char *b64Buf = (char *)ps_malloc(b64Len);
  if (!b64Buf) {
    Serial.println("[STT] HATA: PSRAM base64 yetersiz!");
    return "";
  }
  base64EncodeToBuf((const uint8_t *)recordBuffer, audioBytes, b64Buf);

  size_t bodyLen = b64Len + 256;
  char *bodyBuf = (char *)ps_malloc(bodyLen);
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
           SAMPLE_RATE, b64Buf);
  free(b64Buf);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = String(STT_URL_BASE) + String(googleApiKey);
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);

  int code = http.POST((uint8_t *)bodyBuf, strlen(bodyBuf));
  free(bodyBuf);

  String response = "";
  if (code == 200) {
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, http.getStream());
    if (!err) {
      auto t = doc["results"][0]["alternatives"][0]["transcript"];
      if (!t.isNull())
        response = t.as<String>();
      else
        Serial.println("[STT] Transkript boş.");
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
String askGemini(const String &userText) {
  Serial.println("[Gemini] İstek gönderiliyor...");

  String body =
      "\"Sen Alex adinda Turkce konusan yardimci bir sesli asistansin. "
      "Eger kullanici bir akilli ev cihazini acip kapatmak isterse (isik, priz "
      "vb.), "
      "sohbet etmek yerine SADECE su JSON formatini dondur: "
      "{\\\"cmd\\\": \\\"eylem_adi\\\", \\\"device\\\": \\\"cihaz_adi\\\", "
      "\\\"speech\\\": \\\"kisa_onay_cumlesi\\\"} "
      "Eylem ornekleri: turn_on, turn_off. Cihaz ornekleri: living_room_light, "
      "kitchen_socket. "
      "Sohbet ise kisa ve net normal cevap ver. Kullanici: " +
      userText + "\"}]}]}";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = String(LLM_URL_BASE) + String(googleApiKey);
  http.begin(client, url);
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
        if (response.length() > 500)
          response = response.substring(0, 500);
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
void textToSpeech(const String &text) {
  Serial.println("[TTS] Sentezleniyor...");

  String body = "{\"input\":{\"text\":\"" + text +
                "\"},"
                "\"voice\":{\"languageCode\":\"tr-TR\","
                "\"name\":\"tr-TR-Wavenet-A\"},"
                "\"audioConfig\":{\"audioEncoding\":\"LINEAR16\","
                "\"sampleRateHertz\":" +
                String(SAMPLE_RATE) + "}}";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = String(TTS_URL_BASE) + String(googleApiKey);
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);

  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("[TTS] HTTP Hata: %d\n", code);
    http.end();
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
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
      if (searchBuf == token) {
        found = true;
        break;
      }
    }
  }

  if (!found) {
    Serial.println("[TTS] audioContent bulunamadı!");
    http.end();
    return;
  }

  const size_t maxB64 = 300 * 1024;
  char *b64Buf = (char *)ps_malloc(maxB64);
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
      if (c == '"')
        break;
      b64Buf[b64Pos++] = c;
    }
  }
  b64Buf[b64Pos] = '\0';
  Serial.printf("[TTS] Base64 alındı: %d KB\n", b64Pos / 1024);
  http.end();

  size_t maxDecoded = (b64Pos * 3) / 4;
  uint8_t *audioBuf = (uint8_t *)ps_malloc(maxDecoded);
  if (!audioBuf) {
    Serial.println("[TTS] HATA: Decode için PSRAM yetersiz!");
    free(b64Buf);
    return;
  }

  size_t actualLen = base64Decode(b64Buf, b64Pos, audioBuf);
  free(b64Buf);

  Serial.printf("[TTS] Ses hazır: %.1f sn\n",
                (float)(actualLen / sizeof(int16_t)) / SAMPLE_RATE);

  playAudio((int16_t *)audioBuf, actualLen / sizeof(int16_t));
  free(audioBuf);
}

// ============================================
//  SES ÇALMA
// ============================================
void playAudio(int16_t *audioData, size_t sampleCount) {
  Serial.printf("[SPK] Çalınıyor: %.1f sn\n", (float)sampleCount / SAMPLE_RATE);
  size_t offset = 0;
  while (offset < sampleCount) {
    size_t toWrite = min((size_t)BUFFER_LENGTH, sampleCount - offset);
    size_t written = 0;
    i2s_write(SPK_PORT, audioData + offset, toWrite * sizeof(int16_t), &written,
              portMAX_DELAY);
    if (written == 0)
      break;
    offset += written / sizeof(int16_t);
  }
}

// ============================================
//  WI-FI & AYARLAR (WiFiManager)
// ============================================
void wifi_connect() {
  WiFi.mode(WIFI_STA);

  // 1. Ayarları Oku
  preferences.begin("alex-config", false);
  String gKey = preferences.getString("g_key", "");
  String pKey = preferences.getString("p_key", "");
  String wUrl = preferences.getString("w_url", "");

  if (gKey.length() > 0)
    gKey.toCharArray(googleApiKey, 100);
  if (pKey.length() > 0)
    pKey.toCharArray(picovoiceKey, 100);
  if (wUrl.length() > 0)
    wUrl.toCharArray(webhookUrl, 150);

  // 2. WiFiManager Parametreleri
  WiFiManager wm;

  // Özel Parametre kutucukları
  WiFiManagerParameter custom_g_key("g_key", "Google API Key", googleApiKey,
                                    100);
  WiFiManagerParameter custom_p_key("p_key", "Picovoice AccessKey",
                                    picovoiceKey, 100);
  WiFiManagerParameter custom_w_url("w_url", "Webhook URL", webhookUrl, 150);

  wm.addParameter(&custom_g_key);
  wm.addParameter(&custom_p_key);
  wm.addParameter(&custom_w_url);

  // Bağlanmaya çalış veya AP aç
  Serial.println("[WiFi] Bağlanılıyor veya AP açılıyor...");

  // Eğer daha önce kaydedilmiş ağ varsa bağlanır, yoksa "Alex_Setup" ağı kurar.
  bool res = wm.autoConnect("Alex_Setup"); // Şifresiz AP

  if (!res) {
    Serial.println("[WiFi] Bağlantı Hatası veya TimeOut");
    // ESP.restart(); // İsterseniz reset attırabilirsiniz
  } else {
    Serial.println("[WiFi] Bağlandı!");

    // 3. Yeni girilen değerleri kaydet (Eğer değiştiyse)
    String newGKey = custom_g_key.getValue();
    String newPKey = custom_p_key.getValue();
    String newWUrl = custom_w_url.getValue();

    if (newGKey != gKey || newPKey != pKey || newWUrl != wUrl) {
      Serial.println("[Ayarlar] Yeni değerler kaydediliyor...");
      preferences.putString("g_key", newGKey);
      preferences.putString("p_key", newPKey);
      preferences.putString("w_url", newWUrl);

      // RAM'deki değişkenleri de güncelle
      newGKey.toCharArray(googleApiKey, 100);
      newPKey.toCharArray(picovoiceKey, 100);
      newWUrl.toCharArray(webhookUrl, 150);
    }
  }
  preferences.end();

  Serial.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
}

// ============================================
//  AKILLI EV (WEBHOOK) TETİKLEME
// ============================================
void executeSmartHomeCommand(String action, String device) {
  if (String(SMART_HOME_WEBHOOK_URL) == "YOUR_WEBHOOK_URL_HERE" ||
      String(SMART_HOME_WEBHOOK_URL) == "") {
    Serial.println(
        "[SmartHome] HATA: Webhook URL ayarlanmamış! (config.h'a bak)");
    return;
  }

  // URL oluşturma (Basit string değişimi: {event} -> action_device)
  // Örnek: action="turn_on", device="light" -> event="turn_on_light"
  String eventName = action + "_" + device;
  String url = SMART_HOME_WEBHOOK_URL;

  // URL içinde {event} varsa değiştir (IFTTT tarzı)
  url.replace("{event}", eventName);

  // Eğer URL'de {event} yoksa, query parametresi olarak ekleyelim (Home
  // Assistant/Generic tarzı) Bu kısım ihtiyaca göre özelleştirilebilir.
  if (url.indexOf("webhook") == -1 && url.indexOf("trigger") == -1) {
    // Basit ekleme
  }

  Serial.println("[SmartHome] İstek: " + url);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  // Webhook genelde GET veya POST olur. IFTTT GET kullanabilir.
  http.begin(client, url);

  int httpCode = http.GET(); // veya http.POST("");

  if (httpCode > 0) {
    Serial.printf("[SmartHome] Başarılı! Kod: %d\n", httpCode);
  } else {
    Serial.printf("[SmartHome] Hata: %s\n",
                  http.errorToString(httpCode).c_str());
  }

  http.end();
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
  const char *names[] = {"IDLE", "LISTENING", "THINKING", "SPEAKING"};
  Serial.printf("\n[DURUM] >>> %s\n", names[s]);
  if (s == STATE_IDLE || s == STATE_LISTENING) {
    lastSoundTime = millis();
    soundDetected = false;
  }
}

void i2s_mic_init() {
  i2s_config_t cfg = {.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
                      .sample_rate = SAMPLE_RATE,
                      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
                      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
                      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
                      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
                      .dma_buf_count = 8,
                      .dma_buf_len = BUFFER_LENGTH,
                      .use_apll = false,
                      .tx_desc_auto_clear = false,
                      .fixed_mclk = 0};
  i2s_pin_config_t pins = {.bck_io_num = MIC_SCK_PIN,
                           .ws_io_num = MIC_WS_PIN,
                           .data_out_num = I2S_PIN_NO_CHANGE,
                           .data_in_num = MIC_SD_PIN};
  i2s_driver_install(MIC_PORT, &cfg, 0, NULL);
  i2s_set_pin(MIC_PORT, &pins);
  i2s_zero_dma_buffer(MIC_PORT);
  Serial.println("[I2S] Mikrofon hazır.");
}

void i2s_speaker_init() {
  i2s_config_t cfg = {.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
                      .sample_rate = SAMPLE_RATE,
                      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
                      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
                      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
                      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
                      .dma_buf_count = 8,
                      .dma_buf_len = BUFFER_LENGTH,
                      .use_apll = false,
                      .tx_desc_auto_clear = true,
                      .fixed_mclk = 0};
  i2s_pin_config_t pins = {.bck_io_num = SPK_BCLK_PIN,
                           .ws_io_num = SPK_LRC_PIN,
                           .data_out_num = SPK_DIN_PIN,
                           .data_in_num = I2S_PIN_NO_CHANGE};
  i2s_driver_install(SPK_PORT, &cfg, 0, NULL);
  i2s_set_pin(SPK_PORT, &pins);
  i2s_zero_dma_buffer(SPK_PORT);
  Serial.println("[I2S] Hoparlör hazır.");
}

// ============================================
//  WAKE WORD ALGILAMA (Picovoice)
// ============================================
bool detectWakeWord(int32_t *buffer, size_t length) {
#ifdef USE_WAKE_WORD
  if (porcupine == NULL)
    return false;

  int32_t keyword_index = -1;
  // I2S'den 32-bit geliyor, Porcupine 16-bit ister.
  // Gerçek uygulamada ring buffer veya adam akıllı sample conversion gerekir.
  // Porcupine init edilirken frame_length öğrenilir (genelde 512).

  int32_t frameLength = pv_porcupine_frame_length();

  // Basitçe buffer'ı dönelim (Dikkat: Block blocking veya veri atlama olabilir)
  // Gelen 'length' sample sayısı (32-bit int sayısı)
  // Porcupine her 'frameLength' sample'da bir process çağrılmasını ister.

  // Eğer gelen veri frameLength'ten küçükse işleyemeyiz (bu basit örnek için).
  if (length < frameLength)
    return false;

  // Sadece ilk frame'i alıp deneyelim (Örnek amaçlı basitleştirilmiş)
  int16_t pcmFrame[frameLength];
  for (int i = 0; i < frameLength; i++) {
    pcmFrame[i] = (int16_t)(buffer[i] >> 16);
  }

  pv_status_t status =
      pv_porcupine_process(porcupine, pcmFrame, &keyword_index);

  if (status == PV_STATUS_SUCCESS && keyword_index != -1) {
    return true;
  }
  return false;
#else
  return false;
#endif
}

void resetHistory() {
  historyCount = 0;
  Serial.println("[History] Temizlendi.");
}

// ============================================
//  LED EFEKTLERİ
// ============================================
void handleLedEffects() {
  static unsigned long lastUpdate = 0;
  static int brightness = 0;
  static int fadeAmount = 5;
  static int hue = 0;

  unsigned long currentMillis = millis();

  // Her 20ms'de bir güncelle (50 FPS)
  if (currentMillis - lastUpdate < 20)
    return;
  lastUpdate = currentMillis;

  switch (currentState) {
  case STATE_IDLE:
    // Mavi Nefes Alma Efekti (Breathing)
    strip.setPixelColor(0, strip.Color(0, 0, brightness));
    brightness += fadeAmount;
    if (brightness <= 10 || brightness >= 150) {
      fadeAmount = -fadeAmount; // Yön değiştir
    }
    break;

  case STATE_LISTENING:
    // Parlak Kırmızı (Dinliyorum!)
    strip.setPixelColor(0, strip.Color(200, 0, 0));
    break;

  case STATE_THINKING:
    // Gökkuşağı Dönme (Rainbow Cycle)
    strip.setPixelColor(0, strip.ColorHSV(hue, 255, 150));
    hue += 200; // Renk değiştir
    if (hue >= 65536)
      hue = 0;
    break;

  case STATE_SPEAKING:
    // Mor Rastgele Parlama
    int b = random(50, 200);
    strip.setPixelColor(0, strip.Color(b, 0, b));
    break;
  }
  strip.show();
}
