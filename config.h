#ifndef CONFIG_H
#define CONFIG_H

// ============================================
//  KULLANICI AYARLARI — BUNLARI DEĞİŞTİR!
// ============================================

// WiFi Ayarları
#define WIFI_SSID "Ag_Adin"
#define WIFI_PASSWORD "Sifren"

// Google API Anahtarı (Speech-to-Text, Text-to-Speech, Gemini)
#define GOOGLE_API_KEY "YOUR_GOOGLE_API_KEY"

// Picovoice AccessKey (Console -> AccessKey)
#define PICOVOICE_ACCESS_KEY "YOUR_ACCESS_KEY_HERE"

// Akıllı Ev Entegrasyonu (Webhook)
// Örnek (IFTTT): "https://maker.ifttt.com/trigger/{event}/with/key/YOUR_KEY"
// Örnek (Home Assistant): "http://homeassistant.local:8123/api/webhook/{event}"
// Kod içinde {event} kısmı eylem adıyla (örn. light_on) değiştirilecektir.
#define SMART_HOME_WEBHOOK_URL "YOUR_WEBHOOK_URL_HERE"

#endif // CONFIG_H
