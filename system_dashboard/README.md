# System Dashboard

Показывает на экране Flipper'а в реальном времени **CPU / RAM / Disk / Network** твоего ПК. Полезно вынести системные счётчики из угла монитора на отдельное устройство.

## Архитектура

```
┌─────────┐   USB CDC      ┌─────────────┐
│   PC    │ ─────────────→ │   Flipper   │
│ daemon  │   "STATS,…\n"  │   FAP UI    │
└─────────┘                └─────────────┘
   psutil                    bars + labels
```

При запуске FAP переключает USB в режим **dual CDC** — первый интерфейс остаётся для qFlipper RPC (продолжает работать), второй — наш custom channel. На ПК появляется **второй COM-порт**, к нему подключается Python-демон.

## Установка

### 1. На Flipper'е
```bash
cd system_dashboard
ufbt launch
```

Категория: `apps/USB/`.

### 2. На ПК
```bash
cd system_dashboard/pc_daemon
pip install -r requirements.txt
python dashboard.py
```

При запуске демон автоматически находит **второй** Flipper-COM-порт (первый занят qFlipper'ом). Если нашлось несколько устройств — передай порт явно:

```bash
# Windows
python dashboard.py COM7

# macOS / Linux
python dashboard.py /dev/tty.usbmodem1421003
```

## Совместимость

| ОС | Демон работает? |
|----|-----------------|
| Windows 10/11 | ✅ `psutil` + `pyserial` |
| macOS | ✅ всё то же самое, порт `/dev/tty.usbmodemXXXX2` |
| Linux | ✅ всё то же самое, порт `/dev/ttyACM1` |

## Протокол

Демон шлёт текстовые строки в формате:

```
STATS,<cpu_pct>,<ram_pct>,<disk_pct>,<net_kbps>\n
```

Пример: `STATS,42,67,73,1240\n` — CPU 42%, RAM 67%, диск 73%, сеть 1240 KB/s.

Если хочешь добавить больше метрик (GPU/температура/процессы) — допиши в `dashboard.py` и в `parse_line()` в `system_dashboard.c`. Протокол текстовый, расширяется на ура.

## Управление

| Кнопка | Действие |
|--------|----------|
| Back | Выход (восстановит USB-режим) |

Никаких других кнопок — это пассивный дисплей.

## Сборка

```bash
cd system_dashboard
ufbt
```

## Идеи на будущее

- Добавить GPU-нагрузку (на ПК — через `nvidia-smi` или `pyamdgpuinfo`)
- Температуру CPU
- Battery % для ноутбука
- Кнопка-фильтр (показать только конкретный график на весь экран)
