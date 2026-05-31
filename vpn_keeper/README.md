# VPN Keeper

Демон-сторож для Windows Native VPN. Пока Flipper подключён к ПК — VPN автоматически поднимается при падении. На экране Flipper'а виден статус, аптайм, количество переподключений.

## Архитектура

```
┌───────────────────────┐                ┌──────────────┐
│  PC daemon            │ ─ VPN,OK,... → │  Flipper FAP │
│  keeper.py "MyVPN"    │  via CDC IF1   │  → status UI │
│   │                   │                └──────────────┘
│   ├ Get-VpnConnection │
│   └ Connect-VpnConnection if down
└───────────────────────┘
```

## Установка

### На Flipper
```bash
cd vpn_keeper
ufbt launch
```

### На ПК (Windows)

```powershell
cd vpn_keeper\pc_daemon
pip install -r requirements.txt
python keeper.py "Имя моего VPN-подключения"
```

**Где взять «Имя VPN»:** Параметры → Сеть и Интернет → VPN → справа от твоего подключения «Свойства». Или PowerShell:
```powershell
Get-VpnConnection | Select-Object Name
```

## Логика демона

Каждые 5 секунд:
1. `Get-VpnConnection -Name "Имя"` — текущий статус
2. Если `ConnectionStatus != "Connected"` → `rasdial "Имя"` (требует сохранённые креды) или `Connect-VpnConnection`
3. Отправляет в Flipper строку `VPN,OK,profile,uptime,reconnects\n`

Когда Flipper отключают от USB → демон продолжает работать (по умолчанию) или останавливается (если запустить с `--stop-on-disconnect`).

## Управление

| Кнопка | Действие |
|--------|----------|
| Back | Выход (демон на ПК продолжит работать) |

## Требования по Windows

- **Сохранённые креды для VPN**. Иначе `rasdial` спросит пароль в воздух. Один раз подключись вручную с галочкой «Запомнить логин и пароль»
- **Запускать как Administrator** — `Connect-VpnConnection` для системных профилей требует прав
- Или используй **User VPN connection** (пункт «Все пользователи» сними при создании) — тогда без админа

## Протокол

Демон → Flipper, раз в 5 сек:
```
VPN,<STATUS>,<profile>,<uptime_sec>,<reconnect_count>\n
```
Статусы: `OK` / `DOWN` / `RECON` / `ERR`

Примеры:
```
VPN,OK,Work-VPN,1247,0\n
VPN,DOWN,Work-VPN,0,3\n
VPN,RECON,Work-VPN,0,3\n
```

## Сборка

```bash
cd vpn_keeper
ufbt
```
