# Pomodoro Sync

Помодоро-таймер на Flipper'е, который **дублирует фазы desktop-уведомлениями** на подключённом ПК. Видишь начало работы / начало перерыва / окончание сразу и на устройстве, и в OS-нотификациях.

## Архитектура

```
┌────────────┐                    ┌──────────────┐
│  Flipper   │ ─── PHASE,WORK,25 ─→│   PC daemon  │
│ ⏱ timer   │      via USB CDC    │  → notify-send│
│  25 / 5    │ ←─── (no input) ────│  → osascript │
└────────────┘                    └──────────────┘
```

Стандартный цикл: **25 минут работы → 5 минут перерыва → повторить**. Считается «Cycle» каждые 25 минут работы.

## Установка

### 1. На Flipper'е
```bash
cd pomodoro_sync
ufbt launch
```

Категория: `apps/USB/`.

### 2. На ПК (любой ОС)
```bash
cd pomodoro_sync/pc_daemon
pip install -r requirements.txt
python pomodoro.py
```

Демон автоматически найдёт **второй** Flipper COM-порт (первый занят qFlipper RPC). Можно указать явно:
```bash
python pomodoro.py COM7                  # Windows
python pomodoro.py /dev/tty.usbmodem...  # macOS/Linux
```

Можно запускать без демона — Flipper работает как обычный Pomodoro-таймер.

## Управление на Flipper'е

| Кнопка | Idle | В фазе |
|--------|------|--------|
| OK | Старт WORK | Пауза / Resume |
| ← | — | Остановить таймер |
| Back | Выход | Остановить + выход |

## Протокол

Flipper шлёт строки в ПК (демон их парсит):

| Сообщение | Когда |
|-----------|-------|
| `PHASE,WORK,25\n` | Началась 25-мин фаза работы |
| `PHASE,BREAK,5\n` | Началась 5-мин фаза перерыва |
| `PHASE,PAUSE\n` | Поставлено на паузу |
| `PHASE,RESUME\n` | Снято с паузы |
| `PHASE,STOP\n` | Таймер остановлен |

Демон на каждое сообщение показывает нативное уведомление: `osascript` на macOS, `notify-send` на Linux, `winotify`/fallback на Windows.

## Совместимость

| ОС | Уведомления |
|----|-------------|
| Windows 10/11 | ✅ через `winotify` (ставится `pip install winotify`) или fallback на консоль |
| macOS | ✅ через AppleScript (`osascript`) — без доп. зависимостей |
| Linux | ✅ через `notify-send` (есть в любом DE из коробки) |

## Сборка

```bash
cd pomodoro_sync
ufbt
```

## Идеи на будущее

- Кастомные интервалы (45/15, 50/10, ...)
- Звуковые уведомления через системный TTS
- Длинный перерыв каждые 4 цикла (25/5 × 4 → 25/15)
- Лог сессий в CSV на SD
- Интеграция с Toggl / Clockify через API из демона
