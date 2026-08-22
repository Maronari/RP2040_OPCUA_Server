# Модуль: `ota/`

## Назначение

Общие примитивы конечного автомата OTA: запись состояния на устройстве,
писатель промежуточной подготовки в банк B и проверка CRC32, используемые
как приложением (`services/firmware_update_service.c`), так и загрузчиком.
Полный поток, диаграмму состояний и анализ проверки целостности см. в
[ota-bootloader.md](../ota-bootloader.md); эта страница — представление на
уровне справочника по модулю.

## Публичный API

`ota/include/ota/ota_state.h` — перечисление состояний
(`IDLE/STAGED_IN_BANK_B/COPYING_TO_BANK_A/READY_TO_BOOT/FAILED`) и раскладка
`ota_state_record_t` на устройстве. `ota/src/ota_flash.c` — API писателя:
`ota_flash_reset_state()`, `ota_flash_writer_begin/write/finish()`,
`ota_flash_read_state()`, `ota_flash_verify_bank_b()`.
`ota/src/ota_flash_backend.h` — подключаемый интерфейс бэкенда
стирания/программирования (с каждым образом линкуется ровно одна
реализация).

## Внутренняя реализация

`ota_crc32_compute()` (`ota_state.c:6-27`) — единая реализация CRC32,
переиспользуемая повсюду (проверка при загрузке, повторная проверка во
время загрузки, самопроверка записи состояния).
`ota_flash_writer_begin/write/finish` буферизует/постранично записывает
данные в банк B и записывает переход записи состояния только на `finish()`
— состояние «в процессе» во время буферизации не записывается.

## Зависимости

`OTA_FLASH_BACKEND_APPLICATION` (образы приложения, проходит через
`drivers/flash_operation_coordinator.h`) либо собственный бесплатформенный
бэкенд загрузчика (`bootloader/main.c:200-210`) — никогда оба одновременно
в одном образе. `ota/cmake/ota_layout.cmake` (генерация раскладки областей
Flash, подключается из корневого `CMakeLists.txt`) и
`ota/cmake/ota_artifacts.cmake` (постсборочная упаковка
полезной нагрузки/манифеста/заводского образа).

## Зависимые модули

`services/firmware_update_service.c` (обёртка сессии на стороне приложения,
вызывается из методов OPC UA в `opcua_model/src/model_ota.c`),
`bootloader/main.c` (чтение/запись записи состояния во время копирования/
проверки при загрузке).

## Владение состоянием

Запись состояния OTA на устройстве — единственные постоянные метаданные
OTA — отдельной области манифеста на устройстве не существует. См.
[persistence.md](../persistence.md#flash-region-map).

## Важные символы

| Символ | File:line |
| --- | --- |
| Перечисление состояний | `ota/include/ota/ota_state.h:11-17` |
| Раскладка записи | `ota/include/ota/ota_state.h:28-41` |
| `ota_crc32_compute()` | `ota/src/ota_state.c:6-27` |
| `ota_flash_reset_state()` | `ota/src/ota_flash.c:123-129` |
| `ota_flash_writer_begin/write/finish()` | `ota/src/ota_flash.c:194-312` |
| `ota_flash_verify_bank_b()` | `ota/src/ota_flash.c:166-182` |

## Контекст выполнения

Вызовы `ota_flash_writer_*` выполняются в `OpcUaTask` (вызываются
синхронно из коллбэков методов `BeginFirmwareUpdate`/`WriteFirmwareChunk`/
`FinalizeFirmwareUpdate`). `ota_flash_reset_state()` также выполняется один
раз из `InitTask` при загрузке (`app_acknowledge_ota_boot_if_needed`).
Переходы на стороне загрузчика выполняются до появления RTOS, однопоточно.

## Синхронизация

Внутри самого `ota/` отсутствует — `firmware_update_service.c` обеспечивает
работу только одной сессии одновременно на уровне выше этого слоя;
загрузчик однопоточен.

## Аппаратные ресурсы

QSPI Flash: банк B (цель записи во время промежуточной подготовки),
область состояния OTA (чтение/запись при каждом переходе).

## Персистентное состояние

См. [persistence.md](../persistence.md#flash-region-map) и
[ota-bootloader.md](../ota-bootloader.md#flash-layout).

## Поведение при сбоях

Несовпадение CRC32 при `finish()` приводит к отказу загрузки ещё до
какого-либо перехода состояния. Сбой повторной проверки CRC32/таблицы
векторов во время загрузки записывает `FAILED` и оставляет банк A
нетронутым в рамках этой попытки (хотя банк A уже может содержать копию из
предыдущей попытки — см.
[ota-bootloader.md](../ota-bootloader.md#health-confirmation-and-rollback--current-gap)).

## Связанные тесты

`static/tests/ota_state/ota_state_test.c` (запускается через
`test_ota_state.py`), `static/tools/test_stage_ota_resume.py` (проверка
безопасного возобновления загрузки).
