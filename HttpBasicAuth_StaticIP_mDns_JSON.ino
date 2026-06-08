/*
  ============================================================================
  ESP32 + DS18B20 Мониторинг температуры
  ============================================================================
  Аппаратная часть:
    - ESP32-WROOM (любая плата)
    - 2x DS18B20 (герметичные, с кабелем)
    - Резистор 4.7 кОм (подтяжка линии данных)
    - Блок питания 5V (USB)
  
  Схема подключения:
    Датчики подключаются параллельно (все VCC к 3.3V, все GND к GND,
    оба DATA к GPIO23). Резистор 4.7 кОм между DATA и 3.3V.
  
  Даташиты:
    - ESP32: https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf
    - DS18B20: https://datasheets.maximintegrated.com/en/ds/DS18B20.pdf
  
  Функционал:
    - Поиск до 4 датчиков на шине 1-Wire (пин 23)
    - WEB-интерфейс с авторизацией (admin/esp32)
    - JSON-API на /data для интеграции с InfluxDB
    - mDNS (доступ по имени esp32-temp.local)
    - Автообновление HTML-страницы каждые 10 секунд
  ============================================================================
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <OneWire.h>

// ================== НАСТРОЙКИ Wi-Fi ==================
const char* ssid     = "YOUR_SSID";          // Имя вашей Wi-Fi сети
const char* password = "YOUR_PASSWORD";      // Пароль от Wi-Fi

// Статический IP (если нужно)
bool useStaticIP = false;                     // true = статика, false = DHCP

// Настройки статического IP (только если useStaticIP = true)
IPAddress local_IP(192, 168, 0, 140);        // Желаемый IP адрес
IPAddress gateway(192, 168, 0, 1);           // Шлюз (обычно IP роутера)
IPAddress subnet(255, 255, 255, 0);          // Маска подсети
IPAddress primaryDNS(8, 8, 8, 8);            // DNS Google
IPAddress secondaryDNS(8, 8, 4, 4);          // Альтернативный DNS

// ================== ВЕБ-СЕРВЕР ==================
WebServer server(80);
const char* www_username = "admin";           // Логин для доступа
const char* www_password = "esp32";           // Пароль для доступа

// ================== ДАТЧИКИ DS18B20 ==================
const int oneWireBus = 23;                   // Пин для данных (GPIO23)
OneWire oneWire(oneWireBus);

// Массив для хранения уникальных адресов датчиков
byte sensorAddrs[4][8];                      // Максимум 4 датчика
int numSensors = 0;                          // Реальное количество

// Прототипы функций
void findSensors();
float readTemperature(int idx);
void handleRoot();
void handleData();

/*
  ============================================================================
  ПОИСК ВСЕХ ДАТЧИКОВ НА ШИНЕ 1-Wire
  ============================================================================
  Каждый DS18B20 имеет уникальный 64-битный адрес.
  Этот код сканирует шину, проверяет CRC и сохраняет найденные адреса.
*/
void findSensors() {
  Serial.println("🔍 Поиск датчиков DS18B20...");
  numSensors = 0;
  
  while (oneWire.search(sensorAddrs[numSensors]) && numSensors < 4) {
    // Проверка контрольной суммы (CRC) для валидации адреса
    if (OneWire::crc8(sensorAddrs[numSensors], 7) == sensorAddrs[numSensors][7]) {
      Serial.print("✅ Найден датчик #");
      Serial.print(numSensors + 1);
      Serial.print(" с адресом: ");
      for (byte i = 0; i < 8; i++) {
        if (sensorAddrs[numSensors][i] < 0x10) Serial.print("0");
        Serial.print(sensorAddrs[numSensors][i], HEX);
        if (i < 7) Serial.print(" ");
      }
      Serial.println();
      numSensors++;
    } else {
      Serial.println("⚠️ Найден датчик, но CRC ошибка - пропускаем");
    }
  }
  
  oneWire.reset_search();
  Serial.print("📊 Всего найдено датчиков: ");
  Serial.println(numSensors);
  
  if (numSensors == 0) {
    Serial.println("❌ ВНИМАНИЕ: Датчики не обнаружены!");
    Serial.println("   Проверьте: питание, подключение, резистор 4.7кОм");
  }
}

/*
  ============================================================================
  ЧТЕНИЕ ТЕМПЕРАТУРЫ С УКАЗАННОГО ДАТЧИКА
  ============================================================================
  Протокол 1-Wire:
    1. Reset – сигнал сброса шины
    2. Select – выбор датчика по уникальному адресу
    3. 0x44 – команда "начать преобразование"
    4. Ожидание 750 мс (для 12-битного разрешения)
    5. 0xBE – команда "чтение памяти" (9 байт)
    6. Конвертация сырых данных: raw / 16.0 = температура в °C
*/
float readTemperature(int idx) {
  if (idx < 0 || idx >= numSensors) return NAN;
  
  oneWire.reset();
  oneWire.select(sensorAddrs[idx]);
  oneWire.write(0x44);          // Команда "начать преобразование"
  delay(750);                   // Ожидание (12 бит)
  
  oneWire.reset();
  oneWire.select(sensorAddrs[idx]);
  oneWire.write(0xBE);          // Команда "чтение памяти"
  
  byte data[9];
  for (byte i = 0; i < 9; i++) {
    data[i] = oneWire.read();
  }
  
  int16_t raw = (data[1] << 8) | data[0];
  return raw / 16.0;            // Преобразование в градусы Цельсия
}

/*
  ============================================================================
  HTML-СТРАНИЦА С МОНИТОРИНГОМ ТЕМПЕРАТУРЫ
  ============================================================================
  Доступна по адресу: http://<IP_ESP32>/
  Авторизация: admin / esp32
  Автообновление каждые 10 секунд (meta refresh)
*/
void handleRoot() {
  if (!server.authenticate(www_username, www_password)) {
    return server.requestAuthentication();
  }
  
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta http-equiv='refresh' content='10'>";
  html += "<title>ESP32 Temperature Monitor</title>";
  html += "<style>body{font-family:Arial;margin:40px;background:#f0f0f0;}";
  html += ".card{background:white;padding:20px;border-radius:10px;margin-bottom:20px;box-shadow:0 2px 5px gray;}";
  html += "h1{color:#333;} .temp{font-size:3em;font-weight:bold;}</style>";
  html += "</head><body>";
  html += "<div class='card'><h1>🌡️ Мониторинг температуры</h1>";
  
  for (int i = 0; i < numSensors; i++) {
    float t = readTemperature(i);
    html += "<div class='card'>";
    html += "<h2>Датчик " + String(i+1) + "</h2>";
    if (isnan(t)) {
      html += "<p class='temp'>❌ Ошибка чтения</p>";
    } else {
      html += "<p class='temp'>" + String(t, 1) + " °C</p>";
    }
    html += "</div>";
  }
  
  html += "<p><small>Автообновление каждые 10 секунд | <a href='/'>Обновить сейчас</a></small></p>";
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

/*
  ============================================================================
  JSON API ДЛЯ ИНТЕГРАЦИИ С INFLUXDB
  ============================================================================
  Доступен по адресу: http://<IP_ESP32>/data
  Авторизация: admin / esp32
  Формат ответа: {"sensor1":23.5,"sensor2":24.1}
  Используется Python-мостом для автоматического сбора данных.
*/
void handleData() {
  if (!server.authenticate(www_username, www_password)) {
    return server.requestAuthentication();
  }
  
  String json = "{";
  for (int i = 0; i < numSensors; i++) {
    float t = readTemperature(i);
    if (i > 0) json += ",";
    json += "\"sensor" + String(i+1) + "\":" + String(t, 1);
  }
  json += "}";
  
  server.send(200, "application/json", json);
}

/*
  ============================================================================
  НАСТРОЙКА И ЗАПУСК
  ============================================================================
*/
void setup() {
  Serial.begin(9600);
  findSensors();
  
  // Подключение к Wi-Fi
  WiFi.mode(WIFI_STA);
  
  if (useStaticIP) {
    if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
      Serial.println("⚠️ Ошибка статического IP! Использую DHCP.");
    } else {
      Serial.println("✅ Статический IP настроен.");
    }
  }
  
  WiFi.setHostname("esp32-temp");
  WiFi.begin(ssid, password);
  Serial.print("📡 Подключение к WiFi");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("✅ WiFi подключён!");
    Serial.print("🌐 IP адрес: ");
    Serial.println(WiFi.localIP());
    Serial.print("📛 Hostname: ");
    Serial.println(WiFi.getHostname());
  } else {
    Serial.println("❌ Не удалось подключиться к WiFi! Перезагрузка...");
    delay(5000);
    ESP.restart();
  }
  
  // Запуск mDNS (доступ по имени esp32-temp.local)
  if (!MDNS.begin("esp32-temp")) {
    Serial.println("⚠️ Ошибка запуска mDNS");
  } else {
    MDNS.addService("http", "tcp", 80);
    Serial.println("✅ mDNS запущен: http://esp32-temp.local");
  }
  
  // Настройка веб-сервера
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
  
  Serial.println("🚀 HTTP сервер запущен");
  Serial.print("🔗 Откройте в браузере: http://");
  Serial.println(WiFi.localIP());
  Serial.println("🔐 Логин: admin  Пароль: esp32");
  Serial.println("📊 JSON API: /data");
}

void loop() {
  server.handleClient();
  delay(2);
}