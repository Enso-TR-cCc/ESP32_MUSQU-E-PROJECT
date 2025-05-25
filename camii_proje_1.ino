#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>
#include "AudioFileSourceSD.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"
#include "ESP32Time.h"

// ====== WiFi Ayarları ======
const char* ssid = "Tenda_9ED230";
const char* password = "Memo350607";

// ====== Zaman ve Ses ======
ESP32Time rtc;
AudioGeneratorWAV *wav = nullptr;
AudioFileSourceSD *file = nullptr;
AudioOutputI2S *out = nullptr;

// ====== SD Kart Pinleri ======
#define SD_CS 5
#define SPI_MOSI 23
#define SPI_MISO 19
#define SPI_SCK 18

SPIClass SPI_1(VSPI); // SD kart için ayrı SPI

// ====== Vakit Tanımı ======
struct Vakit {
  String isim;
  String dosya;
  String saat;
};

Vakit vakitler[] = {
  {"Imsak", "/imsak.wav", ""},
  {"Dhuhr", "/ogle.wav", ""},
  {"Asr", "/ikindi.wav", ""},
  {"Maghrib", "/aksam.wav", ""},
  {"Isha", "/yatsi.wav", ""}
};
const int vakitSayisi = sizeof(vakitler) / sizeof(vakitler[0]);
bool caldiMi[vakitSayisi];

// ====== API ======
const char* apiBaseURL = "https://api.aladhan.com/v1/timingsByCity";
const char* city = "Izmir";
const char* country = "Turkey";
const int method = 13;

// ====== Fonksiyon Prototipleri ======
void ntpGuncelle();
void ezanVakitleriniCek();
void zamanKontrol();
void sesCal(String dosyaAdi);

// ====== Global Değişken ======
String sonGuncelleme = "";

// ====== Setup ======
void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  Serial.print("WiFi bağlanıyor");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi bağlandı");

  // SD başlat
  SPI_1.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI_1)) {
    Serial.println("SD kart başlatılamadı!");
    while (1) delay(1000);
  }
  Serial.println("SD kart hazır");

  // I2S ses çıkışı ayarı
  out = new AudioOutputI2S();
  out->SetPinout(26, 25, 22); // BCLK, LRCLK, DIN
  out->SetGain(1.0);

  // Saat & ezan vakti çek
  ntpGuncelle();
  ezanVakitleriniCek();

  // Vakitlerin çalınma durumu sıfırlansın
  for (int i = 0; i < vakitSayisi; i++) caldiMi[i] = false;

  // Günü baştan kaydet
  sonGuncelleme = rtc.getTime("%d-%m-%Y");
}

// ====== Zamanlayıcı için değişken ======
unsigned long sonZamanKontrol = 0;

// ====== Loop ======
void loop() {
  if (wav && wav->isRunning()) {
    wav->loop();
  } else if (wav) {
    wav->stop();
    delete wav;
    delete file;
    wav = nullptr;
    file = nullptr;
  }

  if (millis() - sonZamanKontrol >= 1000) {
    sonZamanKontrol = millis();
    if (WiFi.status() == WL_CONNECTED) {
      zamanKontrol();
    }
  }
}

// ====== API URL üret ======
String getApiUrl() {
  time_t now = time(nullptr);
  struct tm *timeinfo = localtime(&now);
  char tarih[11];
  snprintf(tarih, sizeof(tarih), "%02d-%02d-%04d", timeinfo->tm_mday, timeinfo->tm_mon + 1, timeinfo->tm_year + 1900);
  String url = String(apiBaseURL) + "/" + String(tarih) + "?city=" + city + "&country=" + country + "&method=" + String(method);
  return url;
}

// ====== HTTP GET (redirect dahil) ======
int httpGetWithRedirect(HTTPClient &http, const String &url, String &response) {
  http.begin(url);
  int code = http.GET();

  if (code == 301 || code == 302) {
    String newLoc = http.getLocation();
    http.end();
    Serial.println("Redirect var, yeni URL: " + newLoc);
    http.begin(newLoc);
    code = http.GET();
  }

  if (code == HTTP_CODE_OK) {
    response = http.getString();
  } else {
    response = "";
  }

  http.end();
  return code;
}

// ====== API'den vakit çek ======
void ezanVakitleriniCek() {
  String url = getApiUrl();
  Serial.println("API URL: " + url);

  HTTPClient http;
  String payload;
  int code = httpGetWithRedirect(http, url, payload);

  Serial.printf("HTTP Code: %d\n", code);

  if (code == HTTP_CODE_OK) {
    DynamicJsonDocument doc(4096);
    auto err = deserializeJson(doc, payload);
    if (err) {
      Serial.print("JSON parse hatası: ");
      Serial.println(err.c_str());
      return;
    }

    JsonObject timings = doc["data"]["timings"];

    for (int i = 0; i < vakitSayisi; i++) {
      String apiVakti = timings[vakitler[i].isim];
      vakitler[i].saat = apiVakti.substring(0, 5);
      Serial.printf("%s saati: %s\n", vakitler[i].isim.c_str(), vakitler[i].saat.c_str());
    }
  } else {
    Serial.println("API bağlantısı başarısız");
  }
}

// ====== NTP ile saat güncelle ======
void ntpGuncelle() {
  configTime(10800, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    rtc.setTimeStruct(timeinfo);
    Serial.println("RTC senkronize edildi.");
  } else {
    Serial.println("Zaman alınamadı.");
  }
}

// ====== Vakit kontrol ======
void zamanKontrol() {
  String simdi = rtc.getTime("%H:%M");
  String bugun = rtc.getTime("%d-%m-%Y");

  for (int i = 0; i < vakitSayisi; i++) {
    if (simdi == vakitler[i].saat && !caldiMi[i]) {
      Serial.printf("Vakit geldi: %s (%s)\n", vakitler[i].isim.c_str(), simdi.c_str());
      sesCal(vakitler[i].dosya);
      caldiMi[i] = true;
    }
  }

  if (simdi == "00:01" && bugun != sonGuncelleme) {
    Serial.println("Yeni gün, vakitler güncelleniyor...");
    ezanVakitleriniCek();
    for (int i = 0; i < vakitSayisi; i++) {
      caldiMi[i] = false;
    }
    sonGuncelleme = bugun;
  }
}

// ====== Ses Çalma Fonksiyonu ======
void sesCal(String dosyaAdi) {
  Serial.println("Ses çalınıyor: " + dosyaAdi);
  file = new AudioFileSourceSD(dosyaAdi.c_str());
  wav = new AudioGeneratorWAV();
  wav->begin(file, out);
}
