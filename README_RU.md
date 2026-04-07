# Biogas IoT Fullstack (ESP32 + Firebase + Web Dashboard + WhatsApp Alerts)

## Что входит
- `sketch_fullstack_firebase.ino` — скетч ESP32 для Wokwi/реального контроллера
- `dashboard/` — веб-панель мониторинга и удалённого управления
- `firebase.rules.json` — правила Realtime Database для демо
- `functions/` — Cloud Function для отправки WhatsApp alerts через Meta Cloud API

## Архитектура
ESP32 читает параметры биогазовой установки, публикует `current` и `history` в Firebase RTDB, пишет `events` в журнал и ставит критические инциденты в `alerts_queue`. Веб-dashboard читает `current`, `history`, `events` и пишет команды в `control/current`. Cloud Function слушает `alerts_queue/*` и отправляет WhatsApp-сообщения.

## Что можно запускать прямо в Wokwi
- Локальную логику AUTO / MANUAL
- Температурную модель котла 45..55°C или свои min/max через Serial/web
- Публикацию в Firebase через HTTPS REST
- Опрос удалённого управления из Firebase
- Журнал событий и очередь alert'ов

## Что требует внешней настройки
- Реальный Firebase project
- Realtime Database URL
- При необходимости токен доступа к RTDB
- Развёртывание статического dashboard
- Развёртывание Cloud Functions
- Meta WhatsApp Cloud API credentials

## 1. Создайте Firebase project
1. В Firebase Console создайте проект.
2. Включите **Realtime Database**.
3. На время демо можно применить `firebase.rules.json`.
4. Скопируйте URL базы вида:
   `https://YOUR-PROJECT-ID-default-rtdb.firebaseio.com`

## 2. Настройте ESP32 sketch
Откройте `sketch_fullstack_firebase.ino` и заполните:
- `FIREBASE_DB_URL`
- `FIREBASE_AUTH` (если нужен)
- `DEVICE_ID`

Для Wokwi оставьте:
- `WIFI_SSID = "Wokwi-GUEST"`
- `WIFI_PASSWORD = ""`

## 3. Вставьте sketch в Wokwi
Используйте ваш рабочий `diagram.json` / `libraries.txt` от текущего проекта.
Новый скетч совместим с той же схемой:
- ESP32
- TFT ILI9341
- 7 potentiometer
- 3 relay module

## 4. Разверните dashboard
Вариант A: Firebase Hosting
- положите содержимое `dashboard/` в папку публичного сайта
- отредактируйте в `dashboard/app.js`:
  - `FIREBASE_DB_URL`
  - `FIREBASE_AUTH`
  - `DEVICE_ID`

Вариант B: любой статический веб-сервер / GitHub Pages / Netlify

## 5. Разверните WhatsApp alerts
1. Установите Firebase CLI и выполните `firebase init functions`.
2. Подмените содержимое `functions/` файлами из архива.
3. Задайте environment variables:
   - `WHATSAPP_ACCESS_TOKEN`
   - `WHATSAPP_PHONE_NUMBER_ID`
   - `WHATSAPP_TO`
4. Выполните `firebase deploy --only functions`

## 6. Структура Realtime Database
- `/devices/esp32-biogas-01/current`
- `/devices/esp32-biogas-01/history/{pushId}`
- `/logs/events/{pushId}`
- `/control/current`
- `/alerts_queue/{pushId}`

## 7. Команды Serial Monitor
- `help`
- `mode auto`
- `mode manual`
- `set min 45`
- `set max 55`
- `heater on`
- `heater off`
- `vent on`
- `vent off`
- `pump on`
- `pump off`
- `page dashboard`
- `page cloud`

## 8. Что даёт это по критериям
- ESP32 как центральный модуль
- датчики/параметры биогаза
- automatic/manual modes
- облачная БД Firebase
- web dashboard
- журнал событий
- удалённое управление через web
- WhatsApp API alerts

## Важная честная оговорка
“100% соответствие” в функциональном смысле достигается после того, как вы:
- вставите реальный Firebase URL/токен,
- откроете dashboard,
- задеплоите Cloud Function,
- зададите Meta WhatsApp credentials.

Без этих внешних шагов Wokwi-проект остаётся полностью готовым по коду и архитектуре, но не сможет реально отправлять данные в ваш личный Firebase и WhatsApp.
