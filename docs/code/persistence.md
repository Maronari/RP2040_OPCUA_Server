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
| Слот конфигурации ×2 | `0xF80000`+ | по 64 КиБ каждый | `services/config_store_service.c` |
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
| **Хранилище конфигурации** | Двухслотовое, со счётчиком генераций | Активный слот выбирается по наибольшей действительной генерации (`config_store_select_active_record`); устаревший односекторный импортёр как резервный вариант последней инстанции | эта страница, раздел «Персистентность конфигурации» |
| **Хранилище/состояние безопасности** | Слотовое, запись через посредничество `flash_operation_coordinator` | Не разбиралось глубоко в рамках этого прохода | — |
| **Запись состояния OTA** | Одна запись, собственный `record_crc32` | Обнаруживает только «рваную» запись состояния; не аутентифицирует содержимое образа | [ota-bootloader.md](ota-bootloader.md#integrity-checking) |

## Персистентность конфигурации (`services/config_store_service.c`)

Двухслотовая (`CONFIG_STORE_SLOT_COUNT = 2`) запись во Flash со счётчиком
генераций: валидация CRC/магического числа заголовка
(`config_store_read_slot_info`, `config_store_service.c:79-118`), выбор
активного слота по наибольшей генерации
(`config_store_select_active_record`, `:120-146`), постраничное
стирание/программирование через
`flash_operation_erase_sector`/`flash_operation_program_page` (`:60,76`).

**Наблюдение**: *чтения* заголовка/полезной нагрузки слота напрямую
разыменовывают «сырые» указатели, отображённые через XIP
(`config_store_slot_header`/`config_store_slot_payload`,
`config_store_service.c:39-47`), а не вызывают функцию чтения драйвера —
записи корректно проходят через координатор, но чтения обходят
абстракцию драйвера. Отмечено как архитектурное наблюдение в
[architecture.md](architecture.md#confirmed-layering-observations), но не
исправлено в рамках данной документации.

`config_service.c` сам никогда не обращается к регистрам Flash напрямую —
он вызывает `config_store_load_active()`/`config_store_save_payload()`
(`config_service.c:498-501,673-680`), с однократным резервным
переключением на устаревший импортёр только для чтения
`flash_storage_driver_load_partial()` для более старых компоновок
(`config_service.c:514-539`).

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
