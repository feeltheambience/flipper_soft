# System Dashboard

Показывает на экране Flipper'а в реальном времени **CPU / RAM / GPU / Network** твоего ПК. Полезно вынести системные счётчики из угла монитора на отдельное устройство.

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

### 2. На ПК — два варианта

**Вариант A: разовый запуск (для теста)**
```bash
cd system_dashboard/pc_daemon
pip install -r requirements.txt
python dashboard.py
```

Демон автоматически находит **второй** Flipper-COM-порт (первый занят qFlipper RPC), стримит данные. При выходе из FAP на Flipper'е — демон ждёт и автопереподключается. Ctrl+C для остановки.

**Вариант B: авто-старт при логине в Windows** ⭐ рекомендую
```powershell
cd system_dashboard\pc_daemon
.\install_autostart.ps1
```

Что делает:
- Ставит зависимости (`pyserial`, `psutil`)
- Регистрирует **Scheduled Task** «Flipper System Dashboard» с триггером «At Logon»
- Запускает через **`pythonw.exe`** — нет консольного окна, демон невидимый
- Стартует прямо сейчас, потом сам поднимается при каждом входе в Windows

После установки **тебе ничего не нужно делать руками**:
1. Включил ноут → демон уже работает в фоне
2. Подключил Flipper, открыл `System Dashboard` на нём → столбики появляются за 1-2 секунды
3. Закрыл FAP → демон молча ждёт, при следующем открытии снова подключится

Логи демона: `%USERPROFILE%\.flipper_dashboard.log` (пишет ошибки и состояния).

**Удалить авто-старт:**
```powershell
.\uninstall_autostart.ps1
```

Если что-то идёт не так — проверь `Get-ScheduledTask -TaskName "Flipper System Dashboard"` и лог-файл.

**Явный порт** если нужно (для любого варианта):
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
STATS,<cpu_pct>,<ram_pct>,<gpu_pct>,<net_kbps>\n
```

Пример: `STATS,42,67,73,1240\n` — CPU 42%, RAM 67%, GPU 73%, сеть 1240 KB/s.

GPU читается через `nvidia-smi`. Если у тебя AMD/Intel — нужна другая утилита (можно дописать в `get_gpu_pct()`).

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
