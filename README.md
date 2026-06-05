# Flipperz

Набор небольших приложений (FAP) для Flipper Zero. Собраны под прошивку **Unleashed** (`unlshd-089`), но без больших правок должны собраться и под Momentum / Xtreme / RogueMaster.

## Список приложений

| Папка | Категория | Что делает |
|-------|-----------|------------|
| [`alarm_clock/`](./alarm_clock) | Tools | Будильник с сиреной и вибрацией. Любая кнопка — стоп. |
| [`remote_hub/`](./remote_hub) | Sub-GHz | Быстрый запуск сохранённых `.sub` сигналов из одной плоской папки. |
| [`spectrum_sniffer/`](./spectrum_sniffer) | Sub-GHz | RSSI-мониторинг 4-х ISM-частот (315 / 434 / 868 / 915 МГц) с пиковыми значениями. |
| [`ir_killtv/`](./ir_killtv) | Infrared | Универсальный «выключатель телевизоров» — рассылает power-команды 14 брендов. |
| [`rdp_manager/`](./rdp_manager) | USB | BadUSB-меню: включить/выключить Windows RDP, добавить юзера, показать статус. |
| [`mouse_jiggler/`](./mouse_jiggler) | USB | HID-мышь: микро-движение раз в N сек, чтобы PC не уходил в сон. Win/Mac/Linux. |
| [`button_jiggler/`](./button_jiggler) | USB | HID-клавиатура: нажатие Space/Shift/F15 раз в N минут. Win/Mac/Linux. |
| [`system_dashboard/`](./system_dashboard) | USB | Live CPU/RAM/Disk/Net на экране Flipper'а от PC-демона. Win/Mac/Linux. |
| [`pomodoro_sync/`](./pomodoro_sync) | USB | Pomodoro-таймер + desktop-уведомления через PC-демон. Win/Mac/Linux. |
| [`vibe_idle_keeper/`](./vibe_idle_keeper) | USB | Умный anti-idle для долгих AI-сессий (Claude/Cursor): рандом + пресеты. |
| [`token_tracker/`](./token_tracker) | USB | Live дашборд расходов LLM API. Бар «$X / $Y today». PC-демон. |
| [`vpn_keeper/`](./vpn_keeper) | USB | Windows VPN watchdog — поднимает упавший VPN. Статус на Flipper'е. |
| [`ble_vpn_button/`](./ble_vpn_button) | Bluetooth | Беспроводной toggle Windows VPN через BLE GATT + Python-демон. |
| [`white_noise/`](./white_noise) | Media | Генератор фонового шума с таймером сна. 6 пресетов. |
| [`life_scenes/`](./life_scenes) | Tools | Движок сценариев: одна кнопка → цепочка IR + пауз + вибро. |

## Скачать готовые сборки

Берёшь последний релиз: **[Releases](https://github.com/feeltheambience/flipper_soft/releases/latest)**.

Там лежат уже скомпилированные `.fap` файлы. Установка:

1. Скачай нужный `.fap`
2. Подключи Flipper к компьютеру, открой **qFlipper**
3. Перетащи `.fap` в соответствующую папку на SD-карте:
   - `alarm_clock.fap` → `apps/Tools/`
   - `remote_hub.fap` → `apps/Sub-GHz/`
   - `spectrum_sniffer.fap` → `apps/Sub-GHz/`
   - `ir_killtv.fap` → `apps/Infrared/`
4. На Flipper'е: `Apps → <категория> → <имя>`

Сборки делаются автоматически через GitHub Actions под Unleashed `unlshd-089`.

## Идеи на будущее

Полный список задуманных приложений (собрано + в планах) с категориями, сложностью и оценкой шанса попасть в каталог — в [`IDEAS.md`](./IDEAS.md).

## Сборка из исходников

Нужен Python 3.10+ и USB-кабель.

```bash
# Один раз — поставить ufbt и SDK
pip install --upgrade ufbt
ufbt update --index-url=https://up.unleashedflip.com/directory.json

# Собрать конкретное приложение
cd alarm_clock
ufbt              # → dist/alarm_clock.fap

# Залить на подключённый Flipper и запустить
ufbt launch
```

Готовый `.fap` появится в `<app>/dist/`. Можно скопировать вручную через qFlipper в соответствующую папку `apps/<категория>/`.

> **Важно:** перед `ufbt launch` закрой qFlipper (он держит COM-порт) и убедись что на Flipper'е нет запущенного FAP — иначе ufbt не сможет автоматически закрыть предыдущее приложение.

## Структура проекта

Каждое приложение — отдельная папка с минимальным набором:

```
<app>/
├── application.fam   # манифест: имя, точка входа, размер стека, категория
└── <app>.c           # весь код в одном файле
```

Сборочные артефакты (`dist/`, `.vscode/`) в `.gitignore`.

## Лицензия

MIT — делайте что хотите, оглядки никакой.
