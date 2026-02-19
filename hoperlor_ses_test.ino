/**
 * ============================================
 *  ESP32-S3 + MAX98357A I2S Hoparlör Testi
 *  Hedef: Hoparlörden 1000 Hz bip sesi çalmak
 * ============================================
 */

#include <driver/i2s.h>
#include <math.h>

// === PIN TANIMLAMALARI ===
#define I2S_BCLK_PIN   9    // Bit Clock
#define I2S_LRC_PIN    10   // Left/Right Clock (WS)
#define I2S_DIN_PIN    8    // Data Out (ESP32 → Hoparlör)

#define I2S_PORT       I2S_NUM_1   // Mikrofon NUM_0 kullandığı için NUM_1
#define SAMPLE_RATE    16000
#define BUFFER_LENGTH  512

// === BİP SESİ AYARLARI ===
#define TONE_FREQ      1000   // Hz
#define TONE_VOLUME    20000  // 0-32767 arası (ses yüksekliği)
#define TONE_DURATION  1000   // ms

int16_t sineBuffer[BUFFER_LENGTH];

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== MAX98357A Hoparlör Testi ===");

  i2s_speaker_init();
  generateSineWave(TONE_FREQ, TONE_VOLUME);

  Serial.println("Hazır! Her 2 saniyede bir bip sesi çalacak.");
}

void loop() {
  Serial.println("BİP!");
  playTone(TONE_DURATION);   // 1 saniye çal
  delay(1000);               // 1 saniye bekle
}

// === SİNÜS DALGASI OLUŞTUR ===
void generateSineWave(float frequency, int16_t amplitude) {
  for (int i = 0; i < BUFFER_LENGTH; i++) {
    float angle = 2.0 * M_PI * frequency * i / SAMPLE_RATE;
    sineBuffer[i] = (int16_t)(amplitude * sin(angle));
  }
}

// === TONU ÇAL ===
void playTone(int durationMs) {
  int totalSamples = (SAMPLE_RATE * durationMs) / 1000;
  int samplesWritten = 0;

  while (samplesWritten < totalSamples) {
    size_t bytesWritten = 0;
    int remaining = totalSamples - samplesWritten;
    int toWrite = min(remaining, BUFFER_LENGTH);

    i2s_write(
      I2S_PORT,
      sineBuffer,
      toWrite * sizeof(int16_t),
      &bytesWritten,
      portMAX_DELAY
    );

    samplesWritten += bytesWritten / sizeof(int16_t);
  }
}

// === I2S HOPARLÖR BAŞLATMA ===
void i2s_speaker_init() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),  // Sadece gönderici
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = BUFFER_LENGTH,
    .use_apll = false,
    .tx_desc_auto_clear = true,   // TX için true (ses kesilmesi önlenir)
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num   = I2S_BCLK_PIN,
    .ws_io_num    = I2S_LRC_PIN,
    .data_out_num = I2S_DIN_PIN,       // TX: hoparlöre gönder
    .data_in_num  = I2S_PIN_NO_CHANGE  // RX yok
  };

  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("I2S Speaker HATA: %d\n", err);
    while(1);
  }

  i2s_set_pin(I2S_PORT, &pin_config);
  i2s_zero_dma_buffer(I2S_PORT);
  Serial.println("I2S Speaker başlatıldı!");
}