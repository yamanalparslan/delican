/**
 * ============================================
 *  ESP32-S3 + INMP441 I2S Mikrofon Test Kodu
 *  Hedef: Serial Plotter'da ses dalgası görmek
 * ============================================
 * 
 * BAĞLANTI ŞEMASI (INMP441):
 * --------------------------
 *  INMP441  -->  ESP32-S3
 *  VDD      -->  3.3V
 *  GND      -->  GND
 *  SD       -->  GPIO 4   (Data)
 *  WS       -->  GPIO 5   (Word Select / LR Clock)
 *  SCK      -->  GPIO 6   (Bit Clock)
 *  L/R      -->  GND      (Sol kanal için)
 */

#include <driver/i2s.h>

// === PIN TANIMLAMALARI ===
#define I2S_WS_PIN   5   // Word Select (LRCLK)
#define I2S_SCK_PIN  6   // Bit Clock (BCLK)
#define I2S_SD_PIN   4   // Serial Data (DOUT mikrofondan)

// === I2S AYARLARI ===
#define I2S_PORT         I2S_NUM_0
#define SAMPLE_RATE      16000    // 16 kHz (konuşma için ideal)
#define SAMPLE_BITS      32       // INMP441 32-bit gönderir, biz 24 kullanırız
#define BUFFER_LENGTH    512      // Her okumada kaç sample alacağız

// Okuma tamponu
int32_t rawBuffer[BUFFER_LENGTH];

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("=== ESP32-S3 I2S Mikrofon Testi ===");
  Serial.println("Serial Plotter'ı açmak için: Tools > Serial Plotter");
  Serial.println("Beklenen: Ortamda ses varsa dalgalı, sessizlikte düz çizgi");
  Serial.println("-------------------------------------------");

  i2s_init();
  
  Serial.println("I2S başlatıldı! Okuma başlıyor...");
}

void loop() {
  size_t bytesRead = 0;
  
  // I2S'den veri oku
  esp_err_t result = i2s_read(
    I2S_PORT,
    &rawBuffer,
    sizeof(rawBuffer),
    &bytesRead,
    portMAX_DELAY  // Veri gelene kadar bekle (blokla)
  );

  if (result == ESP_OK && bytesRead > 0) {
    int samplesRead = bytesRead / sizeof(int32_t);
    
    // Her sample'ı Serial'a yaz (Plotter için)
    for (int i = 0; i < samplesRead; i++) {
      // INMP441 veriyi 32-bitin üst 24'üne koyar
      // Sağa kaydırarak 24-bit'e indir, sonra 16-bit'e normalize et
      int32_t sample = rawBuffer[i] >> 8;   // 24-bit'e getir
      int16_t sample16 = sample >> 8;        // 16-bit'e normalize et
      
      // Serial Plotter için tek değer yaz
      Serial.println(sample16);
    }
  } else {
    Serial.println("HATA: I2S okuma başarısız!");
    delay(100);
  }
}

// === I2S BAŞLATMA FONKSİYONU ===
void i2s_init() {
  // I2S konfigürasyonu
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),  // Master + Alıcı
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // L/R = GND -> Sol kanal
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,          // DMA buffer sayısı
    .dma_buf_len = BUFFER_LENGTH, // Her DMA buffer boyutu
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  // Pin konfigürasyonu
  i2s_pin_config_t pin_config = {
    .bck_io_num   = I2S_SCK_PIN,
    .ws_io_num    = I2S_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,  // TX yok (sadece mikrofon)
    .data_in_num  = I2S_SD_PIN          // RX: mikrofon verisi
  };

  // I2S sürücüsünü kur
  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("I2S driver kurulum HATASI: %d\n", err);
    while(1); // Durdur
  }

  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("I2S pin ayar HATASI: %d\n", err);
    while(1);
  }

  // DMA buffer'ı temizle
  i2s_zero_dma_buffer(I2S_PORT);
}