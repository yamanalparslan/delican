/**
 * ============================================
 *  ESP32-S3 Wake-Word Tespiti (ESP-SR)
 *  "Hilekâr" yöntem: Ses seviyesi + basit
 *  eşik tabanlı tetikleme (ESP-SR yoksa)
 * ============================================
 * 
 * NOT: ESP-SR kurulumu karmaşık olduğu için
 * önce "ses enerjisi tabanlı" basit wake-word
 * simülasyonu yapacağız. Sistem çalışınca
 * gerçek wake-word entegre edeceğiz.
 */

#include <driver/i2s.h>

// === PIN TANIMLAMALARI ===
#define I2S_WS_PIN     5
#define I2S_SCK_PIN    6
#define I2S_SD_PIN     4
#define I2S_PORT       I2S_NUM_0
#define SAMPLE_RATE    16000
#define BUFFER_LENGTH  512

// === DURUM MAKİNESİ ===
typedef enum {
  STATE_IDLE,       // Bekliyor
  STATE_LISTENING,  // Ses algılandı, kayıt yapıyor
  STATE_PROCESSING, // Buluta gönderilecek
} SystemState;

SystemState currentState = STATE_IDLE;

// === WAKE-WORD AYARLARI ===
#define WAKE_THRESHOLD     1500   // Bu değerin üstü "ses var" sayılır
#define WAKE_CONFIRM_MS    300    // Kaç ms boyunca ses olmalı (yanlış tetiklenme önler)
#define SILENCE_TIMEOUT_MS 1500  // Konuşma bitti sayılır

int32_t rawBuffer[BUFFER_LENGTH];

// Zamanlayıcılar
unsigned long wakeStartTime = 0;
unsigned long lastSoundTime = 0;
bool soundDetected = false;

// Kayıt tamponu (PSRAM kullanacak)
#define MAX_RECORD_SAMPLES  (SAMPLE_RATE * 5)  // Max 5 saniyelik kayıt
int16_t* recordBuffer = nullptr;
int recordIndex = 0;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== Wake-Word & Durum Makinesi ===");
  Serial.println("Durumlar: IDLE → LISTENING → PROCESSING");
  Serial.println("------------------------------------------");

  // PSRAM'dan kayıt tamponu al
  recordBuffer = (int16_t*)ps_malloc(MAX_RECORD_SAMPLES * sizeof(int16_t));
  if (recordBuffer == nullptr) {
    Serial.println("HATA: PSRAM bulunamadı! Board ayarlarında PSRAM'ı aktif et.");
    while(1);
  }
  Serial.println("PSRAM tampon hazır: 5 saniyelik kayıt kapasitesi");

  i2s_init();

  setState(STATE_IDLE);
}

void loop() {
  size_t bytesRead = 0;
  esp_err_t result = i2s_read(
    I2S_PORT,
    &rawBuffer,
    sizeof(rawBuffer),
    &bytesRead,
    portMAX_DELAY
  );

  if (result != ESP_OK || bytesRead == 0) return;

  int samplesRead = bytesRead / sizeof(int32_t);
  float rms = calculateRMS(samplesRead);

  switch (currentState) {

    case STATE_IDLE:
      // Ses eşiği aşıldıysa sayaç başlat
      if (rms > WAKE_THRESHOLD) {
        if (!soundDetected) {
          soundDetected = true;
          wakeStartTime = millis();
        }
        // Yeterince uzun süre ses varsa gerçek ses say
        if (millis() - wakeStartTime > WAKE_CONFIRM_MS) {
          Serial.println(">>> WAKE-WORD TETİKLENDİ! Dinleniyor...");
          recordIndex = 0;  // Kayıt sıfırla
          setState(STATE_LISTENING);
        }
      } else {
        soundDetected = false;  // Kısa ses → sıfırla
      }
      break;

    case STATE_LISTENING:
      // Ses varsa kaydet
      if (rms > WAKE_THRESHOLD / 2) {
        lastSoundTime = millis();
      }

      // Kayıt tamponuna ekle
      for (int i = 0; i < samplesRead && recordIndex < MAX_RECORD_SAMPLES; i++) {
        recordBuffer[recordIndex++] = (int16_t)(rawBuffer[i] >> 16);
      }

      // Sessizlik zaman aşımı → konuşma bitti
      if (millis() - lastSoundTime > SILENCE_TIMEOUT_MS) {
        Serial.printf(">>> Konuşma bitti. %d sample kaydedildi (%.1f sn)\n",
          recordIndex,
          (float)recordIndex / SAMPLE_RATE
        );
        setState(STATE_PROCESSING);
      }

      // Tampon doldu
      if (recordIndex >= MAX_RECORD_SAMPLES) {
        Serial.println(">>> Max kayıt süresi doldu!");
        setState(STATE_PROCESSING);
      }
      break;

    case STATE_PROCESSING:
      // Bir sonraki aşamada buraya API çağrısı gelecek
      Serial.println(">>> [PROCESSING] Buluta gönderilecek...");
      Serial.println(">>> (Sonraki aşamada Wi-Fi + API eklenecek)");
      delay(2000);
      setState(STATE_IDLE);
      break;
  }
}

// === DURUM DEĞİŞTİRME ===
void setState(SystemState newState) {
  currentState = newState;
  switch (newState) {
    case STATE_IDLE:
      Serial.println("[DURUM] IDLE - Uyanma kelimesi bekleniyor...");
      lastSoundTime = millis();
      soundDetected = false;
      break;
    case STATE_LISTENING:
      Serial.println("[DURUM] LISTENING - Kayıt başladı!");
      lastSoundTime = millis();
      break;
    case STATE_PROCESSING:
      Serial.println("[DURUM] PROCESSING - İşleniyor...");
      break;
  }
}

// === RMS HESAPLA ===
float calculateRMS(int samplesRead) {
  long long sumSquares = 0;
  for (int i = 0; i < samplesRead; i++) {
    int32_t sample = rawBuffer[i] >> 14;
    sumSquares += (long long)sample * sample;
  }
  return sqrt((float)sumSquares / samplesRead);
}

// === I2S BAŞLATMA ===
void i2s_init() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = BUFFER_LENGTH,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num   = I2S_SCK_PIN,
    .ws_io_num    = I2S_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_SD_PIN
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
  i2s_zero_dma_buffer(I2S_PORT);
  Serial.println("I2S Mikrofon hazır!");
}
