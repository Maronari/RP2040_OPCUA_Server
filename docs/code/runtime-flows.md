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

## Конфигурация

```mermaid
flowchart TD
    Input["Метод OPC UA (ApplyConfig / SetChannelConfig / import chunk)<br/>opcua_model/src/model_configuration.c или model_ota.c"] --> Validate["config_validate_service_validate_candidate()<br/>services/src/config_validate_service.c"]
    Validate -->|недействительно| Reject["типизированный код состояния, промежуточная конфигурация не изменена"]
    Validate -->|действительно| Stage["config_service stage_set_* / normalize()<br/>локальная копия, фиксируется в g_staged_config под критической секцией"]
    Stage --> Apply["config_service_apply()<br/>транзакция из 5 шагов, см. modules/services.md"]
    Apply --> Begin["1. begin_transaction() — защита «по одной за раз»"]
    Begin --> NormValidate["2. нормализация + валидация промежуточной копии (без блокировки)"]
    NormValidate --> RuntimeApply["3. g_runtime_apply_callback()<br/>= sensor_manager_activate_config()<br/>вызывается БЕЗ удержания блокировки конфигурации"]
    RuntimeApply --> Candidate["sensor_manager формирует кандидатный банк,<br/>запускает/самотестирует кандидатные драйверы,<br/>при неудаче в обратном порядке останавливает/восстанавливает предыдущий банк"]
    Candidate -->|успех| Publish["channel_manager_reset() + register_from_config()"]
    Candidate -->|неудача| RollbackRuntime["предыдущий рабочий банк остаётся активным;<br/>config_service_apply() сообщает об ошибке"]
    Publish --> Commit["4. фиксация g_running_config под критической секцией"]
    Commit --> End["5. end_transaction()"]
    Commit --> Persist["config_store_save_payload()<br/>запись во Flash с двумя слотами и счётчиком генерации<br/>через flash_operation_coordinator"]
```

Коллбэк активации во время выполнения (`sensor_manager_activate_config`)
вызывается **синхронно, без удержания собственной критической секции
сервиса конфигурации** — по контракту он должен либо полностью активировать
кандидата, либо оставить предыдущее рабочее состояние полностью нетронутым.
Сохранение во Flash происходит после фиксации в памяти, через
`config_store_service`, никогда прямым доступом к регистрам из
`config_service.c`. См.
[modules/services.md](modules/services.md#configuration-config_service--config_store_service--friends)
и [persistence.md](persistence.md#config-persistence-servicesconfig_store_servicec).

**v0.5.0 T6**: после шага `Commit` на диаграмме выше
`config_service_run_commit_extensions()` безусловно вызывает
зарегистрированный `opcua_model`-ом коллбэк перезапуска
(`config_service_set_restart_callback()`); если изменённая конфигурация
требует перестроения адресного пространства
(`model_config_requires_server_reboot()`), коллбэк планирует управляемый
перезапуск сервера OPC UA — см.
[modules/opcua_model.md](modules/opcua_model.md#server-lifecycle) для
полной последовательности и обоснования, почему перестроение отложено до
границы цикла `model_core_task`, а не выполнено прямо внутри коллбэка.

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
