# Карта владения состоянием

Каждый значимый глобальный/статический объект состояния, найденный прямым
осмотром исходного кода (поиск по `^static` объявлениям уровня модуля,
перепроверенный по каждому файлу), сгруппированный по подсистемам.
«Читатели»/«писатели» перечисляют места вызова, найденные в ходе данного
прохода — пустая ячейка означает, что паттерн доступа подтверждён, но
отдельные места вызова не перечислялись (см. примечания по сопоставлению
для каждой подсистемы). Это не исчерпывающий анализ def/use по всей
программе; пробелы следует трактовать как «не прочитано в рамках данного
прохода», а не как «подтверждённо отсутствует».

## Каналы (`services/src/channel_manager.c`)

| Symbol | File:line | Owner | Synchronization |
| --- | --- | --- | --- |
| `g_slots` / `g_host_slots` | `channel_manager.c:26,29` | `channel_manager` | `taskENTER_CRITICAL`/`taskEXIT_CRITICAL` (глобальная критическая секция, не мьютекс) вокруг каждого доступа |
| `g_count` | `channel_manager.c:31` | `channel_manager` | то же самое |

Хранилище: PSRAM (`psram_calloc`) на устройстве, статический массив в
остальных случаях. Каждое удержание блокировки — операция O(1) на слот, за
исключением ограниченного сканирования на `PLATFORM_CHANNEL_LIMIT`
итераций в `channel_manager_reset`. Драйверы **никогда** не вызываются при
удержании критической секции — паттерн «аренды» acquire/publish/release
фиксирует снимок генерации+токена под блокировкой, вызывает драйвер вне
какой-либо блокировки, а затем публикует результат только если генерация
всё ещё совпадает (`channel_manager.c:463-501`). Одновременная
реконфигурация (увеличенная генерация) приводит к тихому отклонению
публикации — вызывающая сторона получает
`CHANNEL_MANAGER_WRITE_RESULT_STALE`, а не «рваную» запись.

## Активация конфигурации датчиков (`services/src/sensor_manager.c`)

| Symbol | File:line | Owner |
| --- | --- | --- |
| `g_initialized`, `g_applied_config_revision`, `g_active_count`, `g_active_bank` | `sensor_manager.c:32-41` | `sensor_manager` |
| `g_runtime_storage` (двойная буферизация `configs[2][PLATFORM_CHANNEL_LIMIT]`) | `sensor_manager.c:38` | `sensor_manager` |

`sensor_manager_activate_config` формирует кандидатный банк, запускает и
самотестирует его драйверы, затем публикует его через
`channel_manager_reset()` + `channel_manager_register_from_config()`, с
остановкой/восстановлением предыдущего банка в обратном порядке при любом
сбое (`sensor_manager.c:258-311`).

## История (`services/src/history_service.c`)

| Symbol | File:line | Owner |
| --- | --- | --- |
| `g_store` / `g_host_store` | `history_service.c:33,35` | `history_service` (`HISTORY_SERVICE_MAX_CHANNELS` независимых кольцевых буферов) |
| `g_initialized` | `history_service.c:37` | `history_service` |
| `g_history_mutex` / `g_history_mutex_storage` | `history_service.c:42-43` | `history_service` — статический мьютекс FreeRTOS, тайм-аут 10 мс |
| `g_flash_backend`, `g_flash_ready` | `history_service.c:47-48` (только на устройстве) | `history_service` → `drivers/history_flash_backend.h` |

Один `history_service_channel_ring_t` на канал, ключом служит стабильная
строка идентификатора канала. Чтения используют ограниченные критические
секции на порцию данных (`HISTORY_SERVICE_READ_CHUNK_SAMPLES` = 32 записи
на одно взятие блокировки, `history_service.c:554-592`) — блокировка
освобождается между порциями и никогда не удерживается на протяжении
полного сканирования по одному каналу или по нескольким каналам, что
удовлетворяет требованию `ROADMAP.md`/`AGENTS.md` для v0.4.3. См.
[persistence.md](persistence.md) о зеркале во Flash.

## Аварийные сигналы (`services/src/alarm_service.c`)

| Symbol | File:line | Owner |
| --- | --- | --- |
| `g_conditions` / `g_host_conditions` | `alarm_service.c:21,25` | `alarm_service` (`ALARM_SERVICE_MAX_CONDITIONS` слотов) |
| `g_condition_count` | `alarm_service.c:27` | `alarm_service` |
| `g_legacy_channel_id` / `g_legacy_channel_name` | `alarm_service.c:30-31` | заглушка обратной совместимости `alarm_service` |

`taskENTER_CRITICAL`/`taskEXIT_CRITICAL` вокруг каждой операции
привязки/чтения/фиксации/подтверждения. Вычисления оценки выполняются
**вне** блокировки; заблокирована только финальная операция
сравнения-и-фиксации (`alarm_service.c:301-385`), поэтому оценка порога
никогда не выполняется при глобально отключённых прерываниях/планировании.
Одновременное подтверждение объединяется, а не затирается, с помощью
счётчика `acknowledgement_revision` для каждого условия
(`alarm_service.c:369-383`).

## Конфигурация (`services/src/config_service.c`)

| Symbol | File:line | Owner |
| --- | --- | --- |
| `g_config_state` / `g_config_state_fallback` | `config_service.c:37,39` | `config_service` — один блок PSRAM, содержащий `g_running_config`, `g_staged_config`, `g_default_config` |
| `g_running_revision`, `g_staged_revision` | `config_service.c:47-48` | `config_service` |
| `g_runtime_apply_callback` | `config_service.c:49` | устанавливается один раз из `sensor_manager_init` — хук применения на этапе выполнения |
| `g_transaction_active` | `config_service.c:50` | `config_service`, защита «по одному применению за раз» |
| `g_default_config_valid` | `config_service.c:46` | `config_service` |

`config_service_get()`/`get_staged()` — **единственные** два API в этом
файле, возвращающие «живой» указатель на состояние сервиса — все прочие
методы доступа возвращают копию. Применение — пятишаговая транзакция:
начало (критическая секция) → нормализация+валидация (без блокировки) →
синхронный вызов `g_runtime_apply_callback` без удержания блокировки
конфигурации → фиксация `g_running_config` (критическая секция) →
завершение транзакции. См.
[modules/services.md](modules/services.md#config_service).

## Обслуживание (`services/src/maintenance_service.c`)

| Symbol | File:line | Owner |
| --- | --- | --- |
| `g_maintenance_service_runtime` (`snapshots[2]` + `active_index`) | `maintenance_service.c:37` | `maintenance_service` |

Используется `portENTER_CRITICAL`/`portEXIT_CRITICAL` (явно
задокументировано как работающее на обоих ядрах RP2350 без мьютекса
FreeRTOS — `maintenance_service.c:168-184`), а не `taskENTER_CRITICAL`.
Двухфазное применение/фиксация: `prepare_*` подготавливает кандидата под
блокировкой; `apply` копирует его, освобождает блокировку, добавляет запись
в журнал Flash **без** удержания блокировки, затем повторно блокирует
только для записи результата в *неактивный* слот снимка; `commit` —
единственная функция, переключающая `active_index`, атомарно публикуя
результат. Токен транзакции вместе с номерами базовой/кандидатной ревизии
защищают каждый шаг от устаревшего вызывающего кода.

## Безопасность (`services/src/security_service.c` и связанные модули)

| Symbol | File:line | Owner |
| --- | --- | --- |
| `g_user_auth_runtime[SECURITY_SERVICE_MAX_USERS]` | `security_service.c:65` | `security_service` |
| `g_unknown_auth_runtime` | `security_service.c:66` | защита от тайминг-атак при блокировке неизвестных имён пользователей |
| `g_failed_login_count`, `g_last_auth_failure_epoch_s` | `security_service.c:67-68` | `security_service` |
| `g_audit_callback`, `g_audit_callback_context` | `security_service.c:69-70` | `security_service` |
| `g_security_store_ready` | `security_service.c:64` | блокирует выполнение большинства вызовов `security_service_*` |
| `g_security_state_ready` | `security_state_service.c:33` | флаг готовности `security_state_service` |
| `g_security_identity_import_runtime`, `g_security_trust_import_runtime` | соответствующие `security_*_import_service.c` | по одной статической структуре сессии на каждый, повторяющие форму блочной загрузки `config_import_service` |

## OTA / обновление прошивки (`services/src/firmware_update_service.c`, `ota/src/*`)

| Symbol | File:line | Owner |
| --- | --- | --- |
| `g_firmware_update_runtime` | `firmware_update_service.c:16` | `firmware_update_service` — одна сессия (ожидаемый размер/CRC/версия, записанные байты), обеспечивает выполнение «по одной за раз» |
| запись OTA-состояния на устройстве | `ota/include/ota/ota_state.h:28-41` | `ota/src/ota_state.c` + `ota/src/ota_flash.c`; единственные персистентные метаданные OTA (Flash) | см. [ota-bootloader.md](ota-bootloader.md) |

## Сеть / время / диагностика

| Symbol | File:line | Owner |
| --- | --- | --- |
| `g_driver` (`w5x00_ethernet_driver_t`), `g_status`, `g_initialized`, счётчики связи/переподключения | `network_manager.c:16-22` | `network_manager` |
| `g_status` (время), `g_initialized` | `time_service.c:13-14` | `time_service`, защищено `taskENTER_CRITICAL` в `time_service_set_status` |
| `g_registered_task_count`, `g_reset_cause`, `g_last_fault_code` | `diag_service.c:12-14` | `diag_service` — **обращается напрямую к `hardware/watchdog.h`** (`watchdog_caused_reboot`, `watchdog_reboot`), исключение из слоистой архитектуры, см. [architecture.md](architecture.md#confirmed-layering-observations) |

## Координация Flash (`drivers/src/flash_operation_coordinator.c`)

| Symbol | File:line | Owner |
| --- | --- | --- |
| `g_flash_operation_mutex` | `flash_operation_coordinator.c:22-24` | `flash_operation_coordinator` — **единственный** модуль, вызывающий `flash_range_program`/`flash_range_erase`; все прочие подсистемы, пишущие во Flash, проходят через него |

## Контекст модели OPC UA (`opcua_model/src/opcua_model_context.c`)

| Symbol | Owner |
| --- | --- |
| `g_model_context` (`opcua_model_context_t`) | единственный статический экземпляр, содержащий дескрипторы сервера, индексы пространств имён, контексты узлов по каждому каналу (`channel_node_contexts[PLATFORM_CHANNEL_LIMIT]`), таблицу точек продолжения истории (history continuation-point), состояние времени выполнения PubSub и ограниченные буферы для временных данных. Полностью принадлежит `opcua_model`; никогда не дублирует бизнес-состояние, принадлежащее сервисам — контексты узлов хранят скопированную строку идентификатора канала, а не заимствованный указатель в реестр каналов. |

## Модули без состояния (подтверждено отсутствие изменяемых глобальных переменных уровня модуля)

`driver_catalog.c`, `device_health_service.c`,
`security_certificate_group_service.c`, `security_trust_expiry_service.c`,
`config_reset_service.c`, `config_export_service.c`,
`config_validate_service.c`, `config_compat.c`,
`maintenance_journal_codec.c` — подтверждено поиском по `^static`
объявлениям уровня модуля плюс ручной проверкой.

## Смежная документация

- [persistence.md](persistence.md) — подмножество этого состояния,
  хранимое во Flash.
- [architecture.md](architecture.md) — сводка нарушений слоистой
  архитектуры.
- [freertos.md](freertos.md) — какая задача затрагивает какое состояние.
