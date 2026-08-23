# Карта модулей

Однострочное описание ответственности для каждого каталога верхнего уровня
первой стороны и фактическая (а не предполагаемая) цель CMake, которую он
производит. См. [build-system.md](build-system.md) для полного графа
зависимостей целей и
[docs/code/topology/TOPOLOGY.html](topology/TOPOLOGY.html) для
интерактивного графа зависимостей на уровне файлов, полученного на основе
регулярных выражений (открывается в браузере; см. массив `observations`
перед тем как доверять какому-либо конкретному ребру графа).

| Каталог | Цель(и) CMake | Ответственность | Подробный документ |
| --- | --- | --- | --- |
| `app/` (`app/basic_profile/`) | `device_profile_basic[_bank_a\|_w5x00_diag]` | `main()`, создание задач FreeRTOS, последовательность инициализации | [modules/app.md](modules/app.md) |
| `boards/` | (только заголовки, без цели) | Определения выводов платы/PSRAM/размера Flash, выбираются через `PICO_BOARD_HEADER_DIRS` | [startup-flow.md](startup-flow.md#boards-and-board-selection) |
| `drivers/` | `FIRMWARE_DRIVERS`, `FLASH_OPERATION_COORDINATOR` | Владеет каждой единицей периферии RP2350 | [modules/drivers.md](modules/drivers.md) |
| `services/` | `FIRMWARE_SERVICES` | Владеет бизнес-состоянием и состоянием времени выполнения: каналы, конфигурация, история, аварийные сигналы, обслуживание, безопасность, сеть, время, диагностика | [modules/services.md](modules/services.md) |
| `opcua_model/` | `OPCUA_DEVICE_MODEL` | Адаптер информационной модели OPC UA над open62541 | [modules/opcua_model.md](modules/opcua_model.md) |
| `ota/` | `OTA_SHARED`, `OTA_FLASH_BACKEND_APPLICATION` | Общие примитивы конечного автомата OTA | [modules/ota.md](modules/ota.md) |
| `bootloader/` | `ota_bootloader` (только если `OTA_BUILD_BOOTLOADER=ON`) | Решение загрузки второго этапа + копирование/проверка банк B→A | [modules/bootloader.md](modules/bootloader.md) |
| `cmake/` | (без цели) | `firmware_version.cmake` — единственный источник истины для версии | [build-system.md](build-system.md) |
| `static/tools/` | (хостовый Python, без цели) | Инструменты для промежуточной подготовки/проверки/CI | [build-system.md](build-system.md#statictools-operational-and-staging-tools) |
| `static/tests/` | (собирается ad hoc через `test_*.py`, без CTest) | Хостовые обвязки модульных тестов | [build-system.md](build-system.md#testing) |
| `examples/` | `w5x00_opc_dhcp` (только если `BUILD_LEGACY_EXAMPLES=ON`) | Устаревший монолитный эталонный пример, заменён `app/basic_profile` | [build-system.md](build-system.md) |
| `tools/flash_nuke/` | `flash_nuke` | Отдельная утилита восстановления (полное стирание чипа), собирается всегда | — |
| `port/` (связующий слой сторонних компонентов, упоминается только на границах) | `IOLIBRARY_FILES`, `PSRAM_ALLOCATOR`, `UA_PORT_ALLOC`, `LWIP_FILES`, `TIMER_FILES`, `OPEN62541_FILES` | Связующий слой интеграции RP2350/FreeRTOS/lwIP/open62541/PSRAM | [hardware.md](hardware.md), [build-system.md](build-system.md) |
| `libraries/` (сторонние компоненты, документируются только на уровне точек интеграции) | `ETHERNET_FILES`, `FREERTOS_FILES` и т.д. | Pico SDK, FreeRTOS-Kernel, ioLibrary_Driver, pico-extras | — |
| `open62541_v1.5.3/` (сторонний компонент, включён как подмодуль) | (часть `OPEN62541_FILES`) | SDK OPC UA | — |

## Выбор драйверов/пакетов на этапе компиляции

`drivers/CMakeLists.txt` предоставляет по одному `option()` на каждый
нативный драйвер канала (`FIRMWARE_ENABLE_NATIVE_INTERNAL_TEMP_DRIVER`,
`_ADC_VOLTAGE_DRIVER`, `_GPIO_INPUT_DRIVER`, по умолчанию все `ON`), каждый
полностью удаляет исходный код отключённого драйвера из ELF-файла —
целевая политика построения пакетов драйверов на этапе компиляции из
[ROADMAP.md](../../ROADMAP.md) §5.1 для версии `v0.5.1` сегодня частично
реализована на уровне отдельных драйверов, но ещё не в виде именованных
пакетов (`base`/`environmental`/`motion`/`industrial`/`development`).

## Переключатели профилей сборки

`W5X00_DIAGNOSTIC_NO_OPCUA` и `OTA_BUILD_BOOTLOADER` взаимно исключают друг
друга и изменяют набор существующих библиотек/целей — полную таблицу см. в
[build-system.md](build-system.md#build-time-feature-options).

## Связанная документация

- [architecture.md](architecture.md) — правила разделения на слои и
  подтверждённые нарушения.
- [build-system.md](build-system.md) — полный граф смежности целей CMake.
- [README.md](README.md) — индекс документации.
