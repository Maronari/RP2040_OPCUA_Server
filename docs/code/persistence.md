# Энергонезависимое хранение

Существует шесть независимых механизмов энергонезависимого хранения на
базе Flash, все они направляют запись через единый
`flash_operation_coordinator`
([hardware.md](hardware.md#flash-driver-layer)). Подробные форматы записей
для обслуживания и истории уже описаны в отдельных документах — эта
страница представляет собой межподсистемный указатель плюс карту областей
Flash.

<a id="flash-region-map"></a>
## Карта областей Flash

Генерируется файлом [ota/cmake/ota_layout.cmake](../../ota/cmake/ota_layout.cmake)
на этапе конфигурации; полный вывод и диаграмму см. в
[ota-bootloader.md](ota-bootloader.md#flash-layout). Для эталонной цели в
16 МиБ:

| Region | Offset | Size | Owner |
| --- | --- | --- | --- |
| Загрузчик | `0x000000` | 64 КиБ | `bootloader/` |
| Банк A (активная прошивка) | `0x010000` | `0x7A4000` | `ota/` |
| Банк B (промежуточное хранение OTA) | `0x7B4000` | `0x7A4000` | `ota/` |
| Журнал обслуживания | `0xF58000` | 32 КиБ | `drivers/maintenance_journal_backend.c` через `services/maintenance_service.c` |
| Кольцевой буфер истории по каналу | `0xF60000` | 128 КиБ | `drivers/history_flash_backend.c` через `services/history_service.c` |
| Область транзакций конфигурации (STAGED/RUNNING/LAST_KNOWN_GOOD, v0.5.0) | `0xF80000` | 128 КиБ (3×40 КиБ роли + 8 КиБ резерв) | `drivers/config_transaction_backend.c` через `services/config_store_service.c` |
| Слот идентификации безопасности ×2 | после конфигурации | по 64 КиБ каждый | `services/security_service.c` |
| Слот доверенных сертификатов ×2 | после идентификации | по 64 КиБ каждый | `services/security_service.c` |
| Состояние жизненного цикла безопасности | после доверенных сертификатов | 64 КиБ | `services/security_state_service.c` |
| Запись состояния OTA | конец Flash | 64 КиБ | `ota/src/ota_state.c` |
| Устаревший сектор конфигурации только для чтения | `0xfff000-0xffffff` (вытеснен) | — | `drivers/flash_storage_driver.c`, импортёр только для чтения, используется лишь когда оба слота конфигурации недействительны, никогда не стирается и не перезаписывается |

## Сравнение механизмов персистентности

| Mechanism | Transactional? | Recovery model | Detail doc |
| --- | --- | --- | --- |
| **Журнал обслуживания** | Да — конечный автомат commit-page/CRC/восстановления (`COMMITTED`/`NOT_COMMITTED`/`INDETERMINATE`) | Полная классификация восстановления (`EMPTY`/`OK`/`DEGRADED`/`INDETERMINATE`/`FATAL`/`UNSUPPORTED`) сканирует все 8 секторов до раскрытия модели обслуживания | [docs/maintenance_journal.md](../maintenance_journal.md) |
| **Кольцевой буфер истории по каналу (Flash)** | Нет — циклический буфер, только CRC на каждый образец + маркер валидности | «Рваный»/нечитаемый образец пропускается, никогда не раскрывается; потеря самых последних незаписанных образцов при отключении питания — принятое, задокументированное поведение | [docs/channel_model.md](../channel_model.md#history-v043), [docs/known_limits.md](../known_limits.md) |
| **Хранилище конфигурации** | Да, начиная с v0.5.0 — та же схема classification commit-page/CRC, что и журнал обслуживания (одна физическая копия на роль, не A/B-пара) | Три независимо классифицируемые роли (STAGED/RUNNING/LAST_KNOWN_GOOD); RUNNING не годится → прозрачный откат на LAST_KNOWN_GOOD; устаревший односекторный импортёр (для компоновок до config-store, v0.3.x) как резервный вариант последней инстанции | эта страница, раздел «Персистентность конфигурации» |
| **Хранилище/состояние безопасности** | Слотовое, запись через посредничество `flash_operation_coordinator` | Не разбиралось глубоко в рамках этого прохода | — |
| **Запись состояния OTA** | Одна запись, собственный `record_crc32` | Обнаруживает только «рваную» запись состояния; не аутентифицирует содержимое образа | [ota-bootloader.md](ota-bootloader.md#integrity-checking) |

<a id="config-persistence"></a>
## Персистентность конфигурации (`drivers/config_transaction_backend.c` + `services/config_store_service.c`, v0.5.0)

С v0.5.0 физическая запись Flash полностью переехала в
`drivers/src/config_transaction_backend.c` — `services/config_store_service.c`
больше не обращается к Flash напрямую нигде, только через
`config_transaction_backend_*()` (см. заголовочный комментарий
`services/src/config_store_service.c:1-40`). Тот же 128 КиБ бюджет
(`OTA_LAYOUT_CONFIG_SLOT_0/1`) разбит на три равные роли по 40 КиБ —
STAGED/RUNNING/LAST_KNOWN_GOOD — каждая со своей независимой
commit-page/CRC-классификацией, той же техникой, что и
`maintenance_journal_backend` (физическое чтение после записи, а не
доверие возвращаемому значению вызова программирования). Одна физическая
копия на роль, не A/B-пара: сама classification commit-page уже
доказывает атомарность без второй копии. Полную геометрию и формат записи
см. в [docs/flash-storage.md](../flash-storage.md#config-txn-backend).

**Наблюдение, перенесённое из pre-v0.5.0 наблюдения**: *физическое* чтение
теперь выполняется через прямое разыменование XIP-указателя в
`drivers/src/config_transaction_backend.c`'s `flash_read()` (только цель
`PICO_ON_DEVICE`) — но, в отличие от pre-v0.5.0 версии этого наблюдения
(где такое чтение происходило прямо в `services/config_store_service.c`),
это теперь корректное место: `drivers/` — это тот слой, который и должен
обращаться к оборудованию напрямую. Слой `services/` (config_store_service.c)
сам никогда не обращается к регистрам/памяти Flash — он вызывает только
`config_transaction_backend_*()`. Считать наблюдение
[architecture.md](architecture.md#confirmed-layering-observations) №2 и
[modules/services.md](modules/services.md#confirmed-architecture-boundary-observations)
№2 разрешённым (перемещено в правильный слой), а не сохраняющимся.

`config_service.c` (`config_service_init()`, `:699-853`) на каждом
загрузочном пути сперва учитывает
`config_store_get_recovery_state()` (см. frozen recovery-state-to-boot-decision
mapping в комментарии этой функции), затем вызывает
`config_store_load_active()`/`config_store_save_payload()`, с однократным
резервным переключением на устаревший импортёр только для чтения
`flash_storage_driver_load_partial()` для компоновок ещё до config-store
(`:804-830`) — этот резервный путь не связан с pre-v0.5.0 двухслотовой
схемой и не мигрирует её данные; см.
[docs/known_limits.md](../known_limits.md#v050-accepted-residual-risks).

<a id="history-persistence-psram--flash"></a>
## Персистентность истории (PSRAM + Flash)

Согласно [docs/channel_model.md](../channel_model.md#history-v043) и
[docs/known_limits.md](../known_limits.md): каждый канал с включённой
историзацией хранит 256 образцов в PSRAM плюс 256 образцов в кольцевом
буфере на базе Flash. `history_flash_backend` привязан один-к-одному к
слотам кольцевого буфера PSRAM по индексу (`history_service.c:261-269`) и
восстанавливается в PSRAM при инициализации
(`history_flash_backend_restore` → `history_service_restore_sample`,
`:151-172,280-289`). Каждый записываемый образец добавляется во Flash
*после* записи в PSRAM, вне блокировки PSRAM (`:399-424`); неудачное
добавление во Flash устанавливает флаг статуса восстановления для данного
канала, а не молча продолжает работу, так что адаптеры `HistoryRead` могут
завершаться отказом безопасным образом (fail closed).

## Персистентность журнала обслуживания

Полную схему, классификацию commit-page и результаты восстановления см. в
[docs/maintenance_journal.md](../maintenance_journal.md). Со стороны
сервисов: `maintenance_service.c` вызывает
`maintenance_journal_backend_init/_recover/_append/_reset/_export_*`
(`maintenance_service.c:9,392-427,704-706,927-928,1042-1043,1094-1096`) и
никогда не обращается к регистрам Flash напрямую. Добавление записи
происходит **без** удержания собственной критической секции сервиса
обслуживания
([state-ownership.md](state-ownership.md#maintenance-servicesrcmaintenance_servicec)).

## Персистентность безопасности

`security_service.c` и `security_state_service.c` сохраняют сертификаты
идентификации, приватные ключи, списки доверенных сертификатов и состояние
жизненного цикла через тот же `flash_operation_coordinator`
(`security_service.c:569,576`, `security_state_service.c:159,166`). Не
разбиралось глубоко за пределами мест вызова в рамках данной документации
— см. [modules/services.md](modules/services.md) о поверхности API уровня
сервисов.

## Персистентность метаданных OTA

Полностью описана в [ota-bootloader.md](ota-bootloader.md#flash-layout) —
`ota_state_record_t` является единственными метаданными OTA на устройстве;
JSON-манифест с SHA-256 — это артефакт хоста/сборки, никогда не
записываемый во Flash устройства.

## Смежная документация

- [hardware.md](hardware.md) — `flash_operation_coordinator`, через
  который проходит всё вышеописанное.
- [state-ownership.md](state-ownership.md) — состояние в оперативной
  памяти, которое зеркалирует или восстанавливает каждый из этих
  механизмов.
- [ota-bootloader.md](ota-bootloader.md) — генерация раскладки областей
  Flash.
