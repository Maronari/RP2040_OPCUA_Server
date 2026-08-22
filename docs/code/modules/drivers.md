# Модуль: `drivers/`

## Назначение

Владеет всей аппаратной периферией RP2350, к которой обращается код первой стороны: ADC,
GPIO, SPI/W5500 Ethernet, Flash, RTC/AON-таймер, watchdog, светодиод. Предоставляет узкий
контракт vtable (`telemetry_driver_vtbl_t`, определён в
`services/include/services/telemetry_driver.h`) для `services/` для сбора данных каналов,
плюс отдельные заголовки для аппаратуры, не относящейся к каналам.

## Публичный API

| Семейство драйверов | Файлы | Публичный API |
| --- | --- | --- |
| Канал внутренней температуры | `internal_temperature_channel_driver.c` + `temp_sensor_driver.[ch]` | `native_internal_temperature_{start,stop,read}`, `temp_sensor_driver_init/read_celsius` |
| Канал напряжения ADC | `adc_voltage_channel_driver.c` | `native_adc_voltage_{start,stop,read}`, `native_adc_voltage_source_is_valid` |
| Канал входа GPIO | `gpio_input_channel_driver.c` | `native_gpio_input_{start,stop,read}`, `native_gpio_input_source_is_valid` |
| W5500 Ethernet | `w5x00_ethernet_driver.c` | `w5x00_ethernet_driver_init/_start_dhcp/_set_static_ipv4/_poll/_is_link_up/_has_ipv4/_get_netif` |
| Координатор Flash-операций | `flash_operation_coordinator.c` | `flash_operation_coordinator_init/erase_sector/program_page` |
| Бэкенд журнала обслуживания | `maintenance_journal_backend.c` | транзакционный журнал во Flash — см. [docs/maintenance_journal.md](../../maintenance_journal.md) |
| Flash-бэкенд истории | `history_flash_backend.c` | нетранзакционное кольцо во Flash — см. [persistence.md](../persistence.md) |
| Устаревший импортёр конфигурации | `flash_storage_driver.c` | `flash_storage_driver_load_partial` — только чтение, никогда не стирает/не программирует |
| Светодиод | `led_driver.c` | `led_driver_init/set/toggle/is_initialized` |
| Платформенные часы | `platform_clock.c` | `platform_clock_init()` — только RP2040; на RP2350 явный no-op |
| RTC | `rtc_driver.c` | `rtc_driver_init/is_valid/set_unix_time/get_unix_time[_us]` |
| Watchdog | `watchdog_driver.c` | `watchdog_driver_init/kick/is_enabled/get_timeout_ms` |

## `telemetry_driver_vtbl_t` и диспетчеризация

```c
typedef struct telemetry_driver_vtbl {
    telemetry_driver_result_t (*start)(telemetry_channel_t *channel);
    telemetry_driver_result_t (*stop)(telemetry_channel_t *channel);
    telemetry_driver_result_t (*read)(telemetry_channel_t *channel, uint32_t now_ms);
    telemetry_driver_result_t (*write)(telemetry_channel_t *channel, const telemetry_channel_value_t *value);
    telemetry_driver_result_t (*configure)(telemetry_channel_t *channel, const void *config);
    telemetry_driver_result_t (*self_test)(telemetry_channel_t *channel);
} telemetry_driver_vtbl_t;
```

Только `start`/`stop`/`read` заполнены тремя поставляемыми драйверами.
`services/src/driver_catalog.c` связывает указатели vtbl с символами нативных драйверов,
но **сам не содержит обращений к аппаратуре** (подтверждено grep-поиском). Диспетчеризация
происходит из `sensor_manager.c` (`start`/`stop`/`configure`/`self_test`) и
`channel_manager.c:545` (`read`), всегда вне блокировки реестра. Подробности:
[hardware.md](../hardware.md#driver-operation-table-telemetry_driver_vtbl_t).

## Внутренняя реализация

Каждый драйвер канала проверяет свои `source_index`/`source_flags` перед обращением
к аппаратуре (например, драйвер входа GPIO отклоняет `source_index >= NUM_BANK0_GPIOS`,
неизвестные битовые флаги и одновременное включение pull-up и pull-down). Ни один драйвер
не реализует `configure`/`write`/`self_test`. Драйвер напряжения ADC разделяет один
файл-статический флаг инициализации `g_adc_ready` между всеми экземплярами каналов
напряжения ADC, отражая то, что сама периферия ADC — единый разделяемый ресурс,
явно отслеживаемый системой заявок на ресурсы (ниже).

## Цепочка W5500 / SPI / сеть

Периферия SPI RP2350 → `port/ioLibrary_Driver/src/w5x00_spi.c` (связующий код
поставщика, `spi_init`) → `w5x00_ethernet_driver.c` (последовательность инициализации
`wizchip_*`) → TX/RX на уровне сокета MACRAW (`send_lwip`/`recv_lwip`) →
`w5x00_ethernet_driver_poll()` (цикл опроса, принадлежащий драйверу, вызывается каждые
10 мс из `NetworkTask`) → передача lwIP `pbuf`/`netif.input()`. `configure_netif()`
регистрирует netif через `netif_add(..., netif_initialize, ...)` — внутренности
`netif_initialize` находятся в связующем коде поставщика/порта (`port/lwip/w5x00_lwip.c`),
дальше не прослеживались. Подробности со ссылками:
[hardware.md](../hardware.md#w5500--spi--networking-chain).

## Уровень драйвера Flash

`flash_operation_coordinator.c` — единственный вызывающий код для
`flash_range_program`/`flash_range_erase`, сериализован мьютексом FreeRTOS с
обнаружением повторного входа из той же задачи и отклоняет любую страницу,
пересекающуюся с исполняемым образом прошивки. Через него проходят шесть
вызывающих сторон: `config_store_service`, `security_service`,
`security_state_service`, `maintenance_journal_backend`, `history_flash_backend`
и (только чтение, без стирания/программирования) устаревший `flash_storage_driver`.
Подробности: [hardware.md](../hardware.md#flash-driver-layer).

## Владение ресурсами / обнаружение конфликтов

Учёт заявок на ресурсы находится в **services** (`driver_catalog`), а не в самом
`drivers/` — см. [modules/services.md](services.md). Типы заявок: разделяемая
`ADC_PERIPHERAL`, эксклюзивные `ADC_INPUT`/`GPIO` по индексу источника. Механизма
заявок нет для шины SPI W5500, RTC, watchdog или светодиода — каждый из них
является синглтоном, не выбираемым на этапе конфигурации.

## Зависимости / Зависимые модули

- **Зависимости**: заголовки аппаратуры Pico SDK (`hardware/adc.h`,
  `hardware/gpio.h`, `hardware/spi.h`, `hardware/flash.h`,
  `hardware/watchdog.h`, `pico/aon_timer.h`), `libraries/ioLibrary_Driver` +
  `port/ioLibrary_Driver` + `port/lwip` (код поставщика/связующий код, только
  для цепочки W5500).
- **Зависимые модули**: `services/` (через vtbl и отдельные заголовки), `ota/`
  (`OTA_FLASH_BACKEND_APPLICATION` проходит через
  `flash_operation_coordinator`).

## Контекст выполнения

Вызовы `read`/`write`/`start`/`stop` драйверов каналов происходят в `SensorTask`
(через `channel_manager_poll_all`/`sensor_manager`). `w5x00_ethernet_driver_poll`
выполняется в `NetworkTask` каждые 10 мс. `watchdog_driver_kick` выполняется в
`WatchdogTask`. Вызовы `rtc_driver` выполняются в `TimeTask`. Операции Flash
выполняются в той задаче, которая вызывает владеющий сервис (применение
конфигурации, фиксация транзакции обслуживания, добавление сэмпла истории) —
сериализуются общим мьютексом координатора независимо от вызывающей стороны.

## Поведение при сбоях

Драйверы каналов возвращают `telemetry_driver_result_t`
(`OK`/`UNSUPPORTED`/`INVALID_ARGUMENT`/`INVALID_STATE`/`TIMEOUT`/
`HARDWARE_FAILURE`/`NO_MEMORY`). `flash_operation_program_page` завершается
отказом («fail closed») при любой странице, пересекающейся с исполняемым
образом; мьютекс координатора завершается по таймауту, а не блокирует
бесконечно при повторном входе из той же задачи.

## Подтверждённый результат разделения по уровням

**Обращений к аппаратуре в `services/` не найдено**, за исключением двух
исключений, задокументированных в
[architecture.md](../architecture.md#confirmed-layering-observations)
(прямые вызовы watchdog в `diag_service.c` и прямое чтение конфигурации из XIP
в `config_store_service.c`) — историческое нарушение в `driver_catalog.c`
(ADC/GPIO), отмеченное в более ранних версиях `ROADMAP.md`, подтверждённо
устранено.

## Связанные тесты

`static/tests/w5x00_transport/`, а также поведение драйверов, проверяемое
косвенно через `static/tests/channel_manager/driver_catalog_test.c` и
`static/tests/sensor_manager/`.
