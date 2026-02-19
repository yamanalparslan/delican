/**
 * ============================================
 *  ESP32-S3 + INMP441 + NeoPixel
 *  Ses Seviyesine Göre LED Renk Değiştirme
 * ============================================
 * 
 * BAĞLANTI:
 *  INMP441: öncekiyle aynı (GPIO 4,5,6)
 *  NeoPixel DATA --> GPIO 48 (ESP32-S3 dahili LED pini)
 */

#include <driver/i2s.h>
#include <Adafruit_NeoPixel.h>

// === PIN & LED AYARLARI ===
#define LED_PIN        48   // ESP32-S3 DevKit dahili RGB LED
#define LED_COUNT      1

#define I2S_WS_PIN     5
#define I2S_SCK_PIN    6
#define I2S_SD_PIN     4
#define I2S_PORT       I2S_NUM_0
#define SAMPLE_RATE    16000
#define BUFFER_LENGTH  512

// === EŞİK DEĞERLERİ (Kalibre edebilirsin) ===
#define THRESHOLD_SILENT   200    // Altı = sessiz
#define THRESHOLD_LOUD     2000   // Üstü = yüksek ses

int32_t rawBuffer[BUFFER_LENGTH];
Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

void setup() {
  Serial.begin(115200);
  delay(500);

  // LED başlat
  led.begin();
  led.setBrightness(50);  // 0-255, çok parlak olmasın
  setLED(0, 0, 50);       // Başlangıç: mavi (IDLE)

  // I2S başlat
  i2s_init();

  Serial.println("Hazır! Konuş ve LED'i izle.");
  Serial.println("Sessiz=Mavi | Normal=Yeşil | Yüksek=Kırmızı");
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

  // RMS (Ortalama Ses Seviyesi) hesapla
  long long sumSquares = 0;
  for (int i = 0; i < samplesRead; i++) {
    int32_t sample = rawBuffer[i] >> 14;  // Normalize
    sumSquares += (long long)sample * sample;
  }
  float rms = sqrt((float)sumSquares / samplesRead);

  // Serial Plotter için yaz
  Serial.println((int)rms);

  // LED rengini ses seviyesine göre ayarla
  if (rms < THRESHOLD_SILENT) {
    // SESSİZ → Mavi (IDLE modu gibi)
    setLED(0, 0, 80);
  } 
  else if (rms < THRESHOLD_LOUD) {
    // NORMAL SES → Yeşil, ses arttıkça parlaklık artar
    int brightness = map((int)rms, THRESHOLD_SILENT, THRESHOLD_LOUD, 30, 255);
    setLED(0, brightness, 0);
  } 
  else {
    // YÜKSEK SES → Kırmızı
    setLED(255, 0, 0);
  }
}

// === LED YARDIMCI FONKSİYONU ===
void setLED(uint8_t r, uint8_t g, uint8_t b) {
  led.setPixelColor(0, led.Color(r, g, b));
  led.show();
}

// === I2S BAŞLATMA (öncekiyle aynı) ===
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
}