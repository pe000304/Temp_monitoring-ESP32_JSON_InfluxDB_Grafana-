"""
============================================================================
ESP32 → InfluxDB Мост
============================================================================
Назначение:
    - Опрашивает ESP32 по HTTP/JSON API (/data)
    - Полученные температуры записывает в InfluxDB 2.x
    - Работает в бесконечном цикле с заданным интервалом

Зависимости:
    pip install requests influxdb_client

Даташиты и документация:
    - InfluxDB Python Client: https://github.com/influxdata/influxdb-client-python
    - InfluxDB 2.x API: https://docs.influxdata.com/influxdb/v2.7/api/
============================================================================
"""

import requests
import time
from datetime import datetime, timezone
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS
import json

# ================== НАСТРОЙКИ ПОДКЛЮЧЕНИЯ К ESP32 ==================
ESP32_IP = "192.168.0.119"          # IP адрес вашей ESP32
ESP32_PORT = 80                     # Стандартный HTTP порт
ESP32_USER = "admin"                # Логин (совпадает с кодом ESP32)
ESP32_PASS = "esp32"                # Пароль (совпадает с кодом ESP32)
ESP32_URL = f"http://{ESP32_IP}:{ESP32_PORT}/data"

# ================== НАСТРОЙКИ INFLUXDB ==================
INFLUXDB_URL = "http://localhost:8086"
INFLUXDB_TOKEN = "YOUR_TORKEN"
INFLUXDB_ORG = "YOUR_ORG"
INFLUXDB_BUCKET = "YOUR_bucket"

# ================== НАСТРОЙКИ ЦИКЛА ОПРОСА ==================
INTERVAL = 10                       # Секунд между опросами (рекомендуется 10-60)

# Инициализация клиента InfluxDB
client = InfluxDBClient(
    url=INFLUXDB_URL,
    token=INFLUXDB_TOKEN,
    org=INFLUXDB_ORG
)
write_api = client.write_api(write_options=SYNCHRONOUS)


def get_temperatures_from_esp32():
    """
    Получает JSON с температурами с ESP32.
    
    Формат ответа ESP32:
        {"sensor1": 23.5, "sensor2": 24.1}
    
    Returns:
        dict: Словарь {sensor_id: температура} или None при ошибке
    """
    try:
        response = requests.get(
            ESP32_URL,
            auth=(ESP32_USER, ESP32_PASS),
            timeout=10
        )
        
        if response.status_code == 200:
            data = response.json()
            print(f"✅ Данные получены: {data}")
            return data
        else:
            print(f"❌ Ошибка HTTP: {response.status_code}")
            return None
            
    except requests.exceptions.ConnectionError:
        print(f"❌ Не удаётся подключиться к ESP32 по адресу {ESP32_URL}")
        print("   Проверьте: включена ли ESP32, правильный ли IP?")
        return None
    except requests.exceptions.Timeout:
        print("❌ Таймаут ожидания ответа от ESP32")
        return None
    except json.JSONDecodeError:
        print("❌ Ошибка разбора JSON от ESP32")
        return None


def write_to_influxdb(sensor_id, temperature):
    """
    Записывает одно показание температуры в InfluxDB.
    
    Структура данных InfluxDB:
        Measurement: temperature
        Tag: sensor (sensor1, sensor2, ...)
        Field: value (температура в °C)
        Timestamp: текущее время UTC
    """
    point = Point("temperature") \
        .tag("sensor", sensor_id) \
        .field("value", temperature) \
        .time(datetime.now(timezone.utc))
    
    write_api.write(
        bucket=INFLUXDB_BUCKET,
        org=INFLUXDB_ORG,
        record=point
    )
    print(f"   📝 Записано: {sensor_id} = {temperature:.1f}°C")


def main():
    """Основной цикл опроса ESP32 и записи в InfluxDB"""
    print("=" * 50)
    print("🌡️ Мост ESP32 → InfluxDB")
    print(f"   ESP32: {ESP32_URL}")
    print(f"   InfluxDB: {INFLUXDB_URL}")
    print(f"   Интервал опроса: {INTERVAL} секунд")
    print("=" * 50)
    
    error_counter = 0
    
    while True:
        try:
            data = get_temperatures_from_esp32()
            
            if data:
                error_counter = 0  # Сброс счётчика ошибок
                
                for sensor_id, temp in data.items():
                    if isinstance(temp, (int, float)):
                        write_to_influxdb(sensor_id, temp)
                
                print(f"   ✅ Цикл завершён, ожидание {INTERVAL} сек...\n")
            else:
                error_counter += 1
                print(f"⚠️ Не удалось получить данные (ошибка #{error_counter})")
                
                # Если много ошибок подряд — делаем паузу подольше
                if error_counter > 5:
                    print("🔌 Слишком много ошибок, пауза 60 секунд...")
                    time.sleep(60)
                    error_counter = 0
            
        except Exception as e:
            print(f"❌ Непредвиденная ошибка: {e}")
        
        time.sleep(INTERVAL)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n🛑 Программа остановлена пользователем")
        client.close()