# Flutter X11 Embedder - Информация о проекте

## Конфигурация окружения

### Raspberry Pi
- **IP адрес**: 192.168.1.199
- **Пользователь**: dmitrii
- **SSH доступ**: `ssh dmitrii@192.168.1.199`
- **Рабочая директория**: `/home/dmitrii/flutter_pi_demo/x11_embedder`

### Датчик DHT22/AM2302
- **GPIO Pin**: 4 (физический пин 7)
- **Питание**: 3.3V
- **Подтяжка**: 4.7-10 кОм к VCC
- **Протокол**: One-wire с проприетарным таймингом

## Текущее состояние

### Реализованные функции:
1. **X11 Embedder** - работает стабильно, отображает Flutter UI
2. **DHT22 чтение** - реализовано через pigpiod с callbacks (без polling)
3. **Platform Channels** - температура и влажность передаются во Flutter

### Файлы проекта:
- `x11_embedder.cc` - основной embedder
- `dht22_pigpio.cc` - callback-based чтение DHT22
- `dht22_callbacks.cc` - тестовая программа
- `CMakeLists.txt` - конфигурация сборки

### Последние изменения:
- Переход с polling на callback-based чтение DHT22
- Добавлен watchdog таймер (5 мс) для предотвращения зависаний
- Добавлен glitch filter (10 мкс) для фильтрации шума
- Использование condition_variable с таймаутом для гарантированного выхода

## Команды для работы

### Сборка на Raspberry Pi:
```bash
ssh dmitrii@192.168.1.199
cd /home/dmitrii/flutter_pi_demo/x11_embedder
cmake .
make
```

### Запуск:
```bash
# Основное приложение
./flutter_x11_embedder --assets flutter_assets

# Тест датчика
./test_dht22_cb
```

### Копирование файлов с хоста:
```bash
scp файл.cc dmitrii@192.168.1.199:/home/dmitrii/flutter_pi_demo/x11_embedder/
```

## Проблемы и решения

### Решено:
1. **Зависание при чтении DHT22** - переход на callbacks
2. **Нестабильное чтение** - добавлен glitch filter
3. **Timeout проблемы** - watchdog + condition_variable timeout

### В работе:
- Оптимизация частоты опроса датчика
- Добавление поддержки других датчиков

## Зависимости

На Raspberry Pi должны быть установлены:
- pigpio (`sudo apt-get install pigpio`)
- pigpiod daemon (`sudo pigpiod`)
- X11 dev libs
- EGL/GLES libs
- Flutter engine для ARM

## Контакты и ссылки

- Проект находится в `/Users/dmitrii/hackspace/flutter_pi_demo/x11_embedder` (локально)
- На Pi: `/home/dmitrii/flutter_pi_demo/x11_embedder`
- Branch: `01-am2302`