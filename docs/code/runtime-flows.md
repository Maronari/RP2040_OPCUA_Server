# Потоки выполнения во время работы

Прослеживает пути реализации, необходимые для инженерной документации.
Загрузка, OPC UA Read/Write и OTA имеют отдельные страницы с полными
диаграммами — ссылки приведены ниже, без дублирования. Сеть, опрос каналов,
конфигурация и техническое обслуживание прослежены здесь полностью.

## Загрузка

См. [startup-flow.md](startup-flow.md) — сброс → (загрузчик) → `main()` →
инициализация платформы → хранилище → сеть → сервисы → OPC UA →
планировщик FreeRTOS, с точной таблицей фатальных/только-логируемых ошибок
и всеми примечательными наблюдениями по порядку выполнения.

## Сеть

```mermaid
flowchart TD
    A["Периферия SPI RP2350"] --> B["port/ioLibrary_Driver/src/w5x00_spi.c<br/>spi_init() — код связки вендора"]
    B --> C["drivers/src/w5x00_ethernet_driver.c<br/>w5x00_ethernet_driver_init()<br/>последовательность инициализации wizchip_*"]
    C --> D["configure_netif()<br/>netif_add(..., netif_initialize, tcpip_input)"]
    D --> E["services/src/network_manager.c<br/>network_manager_init() владеет g_driver"]
    E --> F["цикл NetworkTask, 10мс<br/>w5x00_ethernet_driver_poll(&g_driver)"]
    F --> G["recv_lwip() читает RX-кольцо W5500<br/>pbuf_alloc/pbuf_take"]
    G --> H["driver->netif.input() -> стек lwIP"]
    H --> I["сервер OPC UA (TCP-биндинг open62541)<br/>через socket API lwIP"]
```

`NetworkTask` вызывает функцию опроса драйвера напрямую — предусмотренное
направление `services -> drivers`, а не нарушение (см.
[hardware.md](hardware.md#w5500--spi--networking-chain)). `OpcUaTask`
блокируется на `network_manager_is_ready()` перед формированием
`UA_ServerConfig` — см.
[modules/opcua_model.md](modules/opcua_model.md#server-lifecycle) — поэтому
сервер OPC UA не начинает прослушивание, пока `network_manager` не сообщит о
готовности линка и последовательности DHCP/статического IP. `TimeTask`
аналогично привязывает запуск SNTP к тому же флагу готовности.

## Опрос каналов

```mermaid
flowchart TD
    HW["Оборудование (ADC / GPIO / встроенный датчик температуры RP2350)"] --> Drv["метод .read() нативного драйвера<br/>drivers/src/*_channel_driver.c"]
    Drv --> CM["channel_manager_poll_all()<br/>services/src/channel_manager.c:545<br/>получить лизинг -> чтение драйвера (без блокировки) -> публикация при совпадении генерации"]
    CM --> Snap["собственный telemetry_channel_snapshot_t"]
    Snap --> Hist["sensor_manager_record_channel_history()<br/>services/src/sensor_manager.c:408<br/>-> history_service_record_channel_sample()"]
    Snap --> Alarm["alarm_service_task, 500мс<br/>alarm_service_evaluate_all() читает снимки каналов напрямую<br/>(не через history_service)"]
    Snap --> Model["opcua_model: model_read_value()<br/>при следующем OPC UA Read узла Value этого канала"]
```

Управляется циклом `SensorTask` (`sensor_manager_task`,
[freertos.md](freertos.md)): `channel_manager_poll_all()`, затем
`sensor_manager_record_channel_history()`, один раз за настроенный период
выборки. `alarm_service` читает снимки каналов независимо, в собственной
задаче с периодом 500 мс, не через `history_service` — история и аварийные
сигналы являются независимыми подсистемами согласно
`docs/releases/v0.4.3.md`. Адаптер OPC UA никогда не опрашивает драйвер
напрямую; он всегда читает последний снимок, опубликованный
`channel_manager`.

<a id="config-transaction-flow"></a>
## Конфигурация (v0.5.0: реальный конвейер транзакции)

С v0.5.0 `config_service_apply()`/`config_service_replace_and_save()`
выполняют единственный зафиксированный порядок — validate -> persist staged
-> stop -> activate -> verify -> commit/rollback — заменивший
pre-v0.5.0 порядок «activate before persist» (T4):

```mermaid
flowchart TD
    Input["Метод OPC UA (ApplyConfig / SetChannelConfig / import chunk / ApplyConfigPreset)<br/>opcua_model/src/model_configuration.c, model_ota.c, model_address_space.c"] --> Validate["1. validate: config_validate_service_validate_candidate()<br/>services/src/config_validate_service.c<br/>(v0.5.0 T5: собирает КАЖДЫЙ отсутствующий driver_id, не только первый)"]
    Validate -->|недействительно| Reject["типизированный код состояния;<br/>staged/running Flash не тронуты"]
    Validate -->|действительно| Persist["2. persist staged: config_store_stage_payload()<br/>-> drivers/config_transaction_backend_stage() (v0.5.0 T3)"]
    Persist -->|сбой записи| RejectIO["ошибка ввода-вывода;<br/>предыдущая RUNNING-конфигурация остаётся активной"]
    Persist -->|успех| Stop["3. stop: sensor_manager останавливает драйверы затронутых каналов"]
    Stop --> Activate["4. activate: запуск новых/изменённых драйверов,<br/>публикация поколения реестра каналов<br/>(БЕЗ перестроения пространства имён OPC UA —<br/>см. modules/opcua_model.md)"]
    Activate -->|сбой| RollbackDriver["перезапуск предыдущих драйверов,<br/>восстановление предыдущего реестра;<br/>staged/running Flash НЕ изменяются"]
    Activate -->|успех| Verify["5. verify: init каждого нового/изменённого драйвера<br/>+ минимум одно валидное чтение на канал,<br/>с явным таймаутом на канал"]
    Verify -->|сбой| RollbackDriver
    Verify -->|успех| Commit["6. commit: config_store_commit_staged()<br/>-> config_transaction_backend_commit()<br/>(сначала RUNNING -> LAST_KNOWN_GOOD, затем STAGED -> RUNNING)"]
    Commit --> ClearState["config_service_clear_stale_channel_state() (v0.5.0 T7)<br/>очищает историю/тревоги удалённых/изменённых каналов"]
    ClearState --> Restart["коллбэк перезапуска (v0.5.0 T6):<br/>если model_config_requires_server_reboot() —<br/>управляемый перезапуск OPC UA сервера,<br/>синхронно, в этом же шаге, до возврата вызывающему"]
```

Каждый шаг выполняется синхронно и последовательно, без блокировки
конфигурации на всём протяжении (блокировка — явный `SemaphoreHandle_t`
мьютекс вокруг `sensor_manager_activate_config()`, v0.5.0 T4). Persist
теперь **предшествует** activate (эта перестановка и есть исправление
pre-v0.5.0 бага «activate before persist»
(`config_service_replace_and_save()`)); откат на шагах Activate/Verify
восстанавливает предыдущие драйверы и реестр, но никогда не трогает
staged/running Flash — Flash остаётся согласованным независимо от того, на
каком шаге произошёл сбой. Полную геометрию Flash-бэкенда, его
commit-page-классификацию и восстановление после перезагрузки на каждой
фазе см. в [persistence.md](persistence.md#config-persistence)
и [flash-storage.md](../flash-storage.md#config-txn-backend);
полное описание шага `Commit`/очистки/перезапуска и почему перестроение
пространства имён отложено до границы цикла `model_core_task` — в
[modules/opcua_model.md](modules/opcua_model.md#server-lifecycle) и
[modules/services.md](modules/services.md#config-service-and-config-store-service).

## Техническое обслуживание

```mermaid
flowchart TD
    Method["Метод OPC UA (PlanMaintenance / StartMaintenance /<br/>RescheduleMaintenance / FinishMaintenance)<br/>opcua_model/src/model_maintenance.c"] --> Commit["model_commit_maintenance_transaction()<br/>единая точка фиксации"]
    Commit --> Prepare["maintenance_service_prepare_*()<br/>валидирует before.state, готовит кандидата<br/>под portENTER_CRITICAL (межъядерная, без мьютекса FreeRTOS)"]
    Prepare --> ApplySvc["maintenance_service_apply()<br/>копирует отложенный снимок, ОСВОБОЖДАЕТ блокировку,<br/>затем добавление в журнал без удержания блокировки"]
    ApplySvc --> Journal["maintenance_journal_backend_append()<br/>Flash-журнал drivers/, транзакционная фиксация страницы/CRC/восстановление"]
    Journal --> ReLock["повторная блокировка: запись результата в НЕАКТИВНЫЙ слот снимка,<br/>отметка фазы APPLIED"]
    ReLock --> CommitSvc["maintenance_service_commit()<br/>ЕДИНСТВЕННАЯ функция, переключающая active_index<br/>= атомарная публикация"]
    CommitSvc --> Sync["model_commit_maintenance_transaction()<br/>синхронизирует CurrentState/CurrentState.Id OPC UA/<br/>запланированные поля/метаданные Condition из той же транзакции"]
    Sync --> Event["живое событие перехода OPC UA генерируется<br/>ПОСЛЕ успешной фиксации"]
```

Неудачный `apply()` возвращает `pending_phase` обратно в `PREPARED` для
повторной попытки; `maintenance_service_abort` полностью отбрасывает
отложенную транзакцию. Токен транзакции вместе с номерами базовой/кандидатной
ревизий защищают каждый шаг от устаревшего вызывающего. Полное описание:
[docs/maintenance_workflow.md](../maintenance_workflow.md) и
[docs/maintenance_journal.md](../maintenance_journal.md).

## OPC UA Read / Write

См. [opcua-flow.md](opcua-flow.md) с полными диаграммами последовательности
клиент → open62541 → коллбэк → сервис → ответ.

## OTA

См. [ota-bootloader.md](ota-bootloader.md) с полным описанием конечного
автомата, цепочки триггеров CMake, проверки целостности и текущего пробела
в подтверждении работоспособности.
