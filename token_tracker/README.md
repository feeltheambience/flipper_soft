# Token Tracker

Live-дашборд расходов LLM API на экране Flipper'а. Показывает день / месяц / счётчик запросов / процент от дневного бюджета.

## Что считает

- **Daily USD** — сколько потратил за сегодня
- **Monthly USD** — суммарно за календарный месяц
- **Budget USD** — твой дневной лимит (по умолчанию $20, настраивается)
- **Reqs today** — количество запросов

Бар вверху заполняется по мере приближения к дневному бюджету. При превышении горит «OVER!».

## Архитектура

```
┌─────────────────┐                 ┌──────────────┐
│  PC daemon      │ ─ TOKENS,X,Y... → Flipper FAP  │
│  tracker.py     │   via CDC IF1    │  → bar+nums  │
└─────────────────┘                 └──────────────┘
     │
     ├─ читает ~/.flipper_tokens.jsonl (manual entries)
     ├─ опционально: OpenAI /v1/usage API
     └─ опционально: парсит cost из Claude Code сессий
```

## Установка

### На Flipper
```bash
cd token_tracker
ufbt launch
```

### На ПК
```bash
cd token_tracker/pc_daemon
pip install -r requirements.txt

# Минимальный запуск (manual file mode)
python tracker.py

# С OpenAI usage (требует Admin API key)
OPENAI_ADMIN_KEY=sk-admin-... python tracker.py

# С кастомным бюджетом и файлом
FLIPPER_BUDGET_USD=50 FLIPPER_TOKENS_FILE=~/llm.jsonl python tracker.py
```

## Как записывать расход

### Способ 1: Ручная запись (универсальный)

Файл `~/.flipper_tokens.jsonl` — одна JSON-строка на запрос:
```jsonl
{"ts":"2026-05-31T14:23:01","model":"claude-sonnet-4","input":1240,"output":850,"cost":0.0182}
{"ts":"2026-05-31T14:24:15","model":"gpt-4o","input":500,"output":300,"cost":0.0035}
```

Демон следит за файлом, суммирует.

### Способ 2: Обёртка для Anthropic SDK

```python
# anthropic_wrapper.py — оборачивает твой вызов API
from anthropic import Anthropic
import json
from datetime import datetime
from pathlib import Path

PRICES = {
    "claude-sonnet-4": {"input": 3.0, "output": 15.0},  # per 1M tokens
    # ...
}

client = Anthropic()
def call(model, messages, **kw):
    resp = client.messages.create(model=model, messages=messages, **kw)
    p = PRICES.get(model, {"input": 0, "output": 0})
    cost = (resp.usage.input_tokens / 1e6 * p["input"]
          + resp.usage.output_tokens / 1e6 * p["output"])
    entry = {
        "ts": datetime.now().isoformat(),
        "model": model,
        "input": resp.usage.input_tokens,
        "output": resp.usage.output_tokens,
        "cost": cost,
    }
    with open(Path.home() / ".flipper_tokens.jsonl", "a") as f:
        f.write(json.dumps(entry) + "\n")
    return resp
```

### Способ 3: OpenAI Admin API

Если есть admin-ключ (`sk-admin-...`), демон тянет `/organization/usage` сам.

### Способ 4: Claude Code sessions

В будущем — парсить логи Claude Code (если найду стабильный формат).

## Управление

| Кнопка | Действие |
|--------|----------|
| Back | Выход |

Пассивный дисплей.

## Протокол

Демон шлёт каждые 10 секунд:
```
TOKENS,<daily_usd>,<monthly_usd>,<budget_usd>,<requests_today>\n
```
Пример: `TOKENS,3.42,87.21,20.0,124\n`

## Сборка

```bash
cd token_tracker
ufbt
```
