# Модуль: `services/`

## Назначение

Владеет всем бизнес- и рабочим состоянием: реестром каналов, транзакцией
staged/running конфигурации, историей, тревогами, рабочим процессом
обслуживания, безопасностью, состоянием сети, временем, диагностикой и
отслеживанием сессии OTA. Обращается к `drivers/` за аппаратурой и к `ota/`
за конечным автоматом обновления; вызывается адаптерами `opcua_model/`.
Согласно [AGENTS.md](../../../AGENTS.md), сервисы не должны напрямую
обращаться к аппаратуре и по возможности должны оставаться независимыми
от типов open62541.

## Опись файлов

31 пара заголовок/исходник в `services/include/services/` и
`services/src/`. Семь заголовков — чистые типы/константы без `.c`
(`telemetry_channel.h`, `channel_config.h`, `device_channels.h`,
`platform_limits.h`, `service_types.h`, `telemetry_driver.h`, `config_wire.h`).

| Сервис | Роль |
| --- | --- |
| `channel_manager` | Ограниченный по размеру реестр каналов в PSRAM — см. §Работа с каналами |
| `sensor_manager` | Рабочая сторона транзакции применения конфигурации; владеет активацией драйверов и циклом задачи опроса/записи истории |
| `driver_catalog` | Статическая таблица времени компиляции, связывающая `driver_id` → `telemetry_driver_vtbl_t`; генератор заявок на ресурсы для обнаружения конфликтов |
| `alarm_service` | Экземпляры условий тревог по каждому каналу — см. §Тревоги |
| `history_service` | Кольца истории в PSRAM + Flash по каждому каналу — см. §История |
| `config_service` | Транзакция staged/running конфигурации устройства — см. §Конфигурация |
| `config_store_service` | Двухслотовая запись во Flash с счётчиком генерации для сохранённого профиля |
| `config_validate_service`, `config_compat`, `config_export_service`, `config_reset_service` | Бессостояниевые валидация, декодирование binary/JSON, экспорт и оркестрация сброса |
| `config_import_service` | Сессия чанковой загрузки импорта конфигурации (одна статическая структура сессии) |
| `maintenance_service` | Конечный автомат обслуживания AMB + связь с журналом — см. §Обслуживание |
| `maintenance_journal_codec` | Чистый побайтовый кодек для одного слота записи журнала (без состояния модуля) |
| `network_manager` | Обёртка статуса Ethernet/DHCP над драйвером W5500 |
| `time_service` | Обёртка времени RTC/SNTP |
| `diag_service` | Реестр задач, статистика кучи, причина сброса, перезагрузка — обращается напрямую к `hardware/watchdog.h` (см. [architecture.md](../architecture.md#confirmed-layering-observations)) |
| `device_health_service` | Бессостояниевый агрегатор → DI `DeviceHealthEnumeration` |
| `firmware_update_service` | Сессия чанковой загрузки образа OTA, оборачивает `ota/ota_state.h` |
| `security_service` | Пользователи, роли, сертификат/ключ идентичности, список доверия, блокировка, коллбэк аудита |
| `security_state_service` | Снимок режима ввода в эксплуатацию / счётчика генерации |
| `security_trust_expiry_service` | Бессостояниевое сканирование окна действительности X.509 |
| `security_certificate_group_service` | Связующий код коллбэков open62541 `UA_CertificateGroup` — раскрывает типы `UA_*` в своём публичном API (наблюдение из [architecture.md](../architecture.md#confirmed-layering-observations)) |
| `security_identity_import_service`, `security_trust_import_service` | Сессии чанкового импорта сертификата/ключа или списка доверия |

## Работа с каналами (`channel_manager` + `sensor_manager`)

`channel_manager` владеет фиксированным массивом на `PLATFORM_CHANNEL_LIMIT`
слотов (по умолчанию 16, `services/include/services/platform_limits.h:10-15`) —
тем же ограничением, что используют `history_service` и `alarm_service`.
Каждая мутация/поиск выполняется в глобальной критической секции FreeRTOS;
драйверы вызываются вне блокировки через аренду
захват/публикация/освобождение, привязанную к отметке генерация+токен, так
что конкурентная реконфигурация приводит к отклонению устаревшей публикации,
а не к повреждению состояния (`channel_manager.c:463-501`). API снимков
(`copy_snapshot_by_id`/`_by_index`) всегда возвращают собственную копию —
вызывающий код никогда не удерживает живой указатель в хранилище реестра.

`sensor_manager` — рабочая сторона транзакции применения конфигурации: он
строит кандидатный набор каналов/драйверов в неактивном банке с двойной
буферизацией, запускает и самотестирует кандидатные драйверы, затем
публикует их через `channel_manager_reset()` +
`channel_manager_register_from_config()`, с остановкой/восстановлением
предыдущего банка в обратном порядке при любом сбое
(`sensor_manager.c:258-311`). Он также управляет циклом
опроса/записи истории задачи `sensor_manager_task` (см.
[freertos.md](../freertos.md)).

## История (`history_service`)

Одно кольцо на канал (`HISTORY_SERVICE_MAX_CHANNELS = PLATFORM_CHANNEL_LIMIT`),
каждое хранит `PLATFORM_HISTORY_SAMPLES_PER_CHANNEL` (256) типизированных
сэмплов. Защищено одним статическим мьютексом FreeRTOS с таймаутом 10 мс.
Чтения используют ограниченные по размеру критические секции на чанк
(32 сэмпла на один захват блокировки, `history_service.c:554-592`) — никогда
одна блокировка на весь скан. Зеркало во Flash привязано один-к-одному к
слотам PSRAM и восстанавливается при инициализации; неудачное добавление во
Flash фиксируется по каждому каналу, а не молча отбрасывается. См.
[persistence.md](../persistence.md#history-persistence-psram--flash) и
[docs/channel_model.md](../../channel_model.md#history-v043).

## Тревоги (`alarm_service`)

Один `alarm_condition_status_t` на каждый канал, допускающий тревоги
(числовой или Boolean; никогда не String), с ключом по стабильному ID
канала. Поддерживает high, high-high, low, low-low с настраиваемым
гистерезисом (только при возврате к норме) и подавлением дребезга
(debounce), дискретное состояние для Boolean-каналов и независимое
состояние `quality_fault`, которое никогда не перезаписывает последнее
состояние процесса/дискретное состояние с хорошим качеством. Расчёт
оценки выполняется вне критической секции; блокируется только сравнение
и фиксация. Подтверждение использует оптимистичный счётчик
`acknowledgement_revision`, так что конкурентная фиксация вычислителя и
подтверждение никогда не затирают друг друга. См.
[docs/channel_model.md](../../channel_model.md#alarms-v043).

## Конфигурация (`config_service` + `config_store_service` и сопутствующие)

`config_service` хранит три полные копии `device_profile_config_t`
(running, staged, базовая по умолчанию) в одном блоке PSRAM, каждая со
своим независимым монотонным счётчиком ревизии. `config_service_get()`/
`get_staged()` — единственные API, возвращающие живой указатель; каждый
мутатор работает с локальной копией и фиксирует изменения в критической
секции. Применение — пятишаговая транзакция: начало → нормализация и
валидация (без блокировки) → синхронный вызов зарегистрированного
коллбэка применения на уровне выполнения (`sensor_manager_activate_config`)
без удержания блокировки конфигурации → фиксация в критической секции →
завершение. `config_store_service` сохраняет результат как двухслотовую
запись во Flash со счётчиком генерации. См.
[persistence.md](../persistence.md#config-persistence-servicesconfig_store_servicec).

## Обслуживание (`maintenance_service`)

Снимок с двойной буферизацией (`snapshots[2]` + `active_index`) под
`portENTER_CRITICAL`/`portEXIT_CRITICAL` (явно задокументировано как
безопасное между ядрами без мьютекса FreeRTOS). Двухфазное
применение/фиксация: `prepare_*` формирует кандидата под блокировкой;
`apply` копирует его, снимает блокировку, добавляет запись в журнал Flash
*без* удержания блокировки, затем повторно захватывает блокировку только
для записи неактивного слота снимка; `commit` — единственная функция,
переключающая `active_index`. Токен транзакции вместе с номерами ревизий
защищает каждый шаг от устаревшего вызывающего кода. См.
[docs/maintenance_workflow.md](../../maintenance_workflow.md) и
[docs/maintenance_journal.md](../../maintenance_journal.md).

## Зависимости / Зависимые модули

- **Зависимости**: `drivers/` (через `telemetry_driver_vtbl_t`,
  `flash_operation_coordinator`, `rtc_driver`, `w5x00_ethernet_driver`,
  `maintenance_journal_backend`, `history_flash_backend`), `ota/`
  (`firmware_update_service` оборачивает `ota_state.h`), типы open62541
  только в `security_certificate_group_service`.
- **Зависимые модули**: `opcua_model/` (каждый коллбэк делегирует вызов
  функции `*_service_*` — см. [opcua-flow.md](../opcua-flow.md)),
  `app/basic_profile/main.c` (последовательность инициализации и создание
  задач).

## Контекст выполнения

Шесть из семи задач FreeRTOS выполняют тела циклов задач сервисов —
`network_manager_task`, `sensor_manager_task`, `time_service_task`,
`alarm_service_task`, `watchdog_task` — см. [freertos.md](../freertos.md).
Мутации конфигурации и обслуживания вызываются синхронно из коллбэков
методов `OpcUaTask`, а не из выделенной задачи.

## Поведение при сбоях

Сбои на этапе инициализации делятся на фатальные (`app_fault_loop`:
`flash_operation_coordinator_init`, `config_service_init`,
`channel_manager_init`, `history_service_init`) и только логируемые
с продолжением работы (`network_manager_init`, `sensor_manager_init`,
`security_service_init`, `time_service_init`, `alarm_service_init`) — см.
[startup-flow.md](../startup-flow.md). Сбои во время работы, как правило,
возвращают типизированное перечисление результата; неограниченных
повторов/выделений памяти в модулях, изученных в этом проходе, не
обнаружено.

<a id="confirmed-architecture-boundary-observations"></a>
## Подтверждённые наблюдения о границах архитектуры

1. `diag_service.c` напрямую вызывает `hardware/watchdog.h`
   (`watchdog_caused_reboot`, `watchdog_reboot`) — каждый другой связанный
   с аппаратурой вызов сервиса на этом уровне вместо этого проходит через
   заголовок `drivers/*`.
2. `config_store_service.c` читает слоты конфигурации во Flash через
   разыменование сырого указателя XIP, а не через вызов чтения драйвера
   (записи корректно проходят через `flash_operation_coordinator`).
3. `security_certificate_group_service.h`/`.c` напрямую раскрывают в своём
   публичном API `UA_CertificateGroup *`, `UA_StatusCode`, `UA_NodeId`,
   `UA_Logger`, `UA_TrustListDataType`, что противоречит цели
   «независимость сервисов от типов open62541» из
   [AGENTS.md](../../../AGENTS.md).

Обращений к ADC/GPIO/SPI/I2C нигде в `services/src/*.c` не найдено — цель
дорожной карты v0.4.1 по выносу обращений к аппаратуре из
`driver_catalog.c` подтверждённо выполнена.

## Связанные тесты

`static/tests/{alarm_service,channel_manager,channel_runtime,config_*,
history_service,maintenance_service,sensor_manager}/` — см.
[build-system.md](../build-system.md#testing).
