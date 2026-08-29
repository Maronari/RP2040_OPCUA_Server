# Графы вызовов

Сфокусированные, подтверждённые доказательствами диаграммы вместо одного
огромного графа. Для исследования зависимостей на уровне файлов
используйте интерактивный граф, полученный на основе регулярных выражений,
`docs/code/topology/TOPOLOGY.html` (откройте в браузере) — он охватывает
168 файлов `.c`/`.h` первой стороны и 147 целей CMake, но является
**извлечением на основе эвристики, а не подтверждённым графом вызовов**;
см. встроенное поле `observations` для оговорок об извлечении (он не видит
диспетчеризацию через указатели на функции/vtable и может неверно
атрибутировать одноимённые функции из разных файлов). Диаграммы ниже —
проверенное подмножество, вручную подтверждённое по исходному коду в ходе
прохода картирования шестью агентами.

## Диспетчеризация драйверов (`telemetry_driver_vtbl_t`)

```mermaid
graph TD
    Catalog["driver_catalog.c<br/>только указатели vtbl, без обращений к оборудованию"]
    SM["sensor_manager.c<br/>start/stop/configure/self_test"]
    CM["channel_manager.c:545<br/>read (poll_all)"]
    CMW["channel_manager.c<br/>write (write_value, аренда захват/публикация)"]
    IT["native_internal_temperature_{start,stop,read}"]
    ADC["native_adc_voltage_{start,stop,read}"]
    GPIO["native_gpio_input_{start,stop,read}"]

    Catalog -->|указатель на const vtbl| SM
    Catalog -->|указатель на const vtbl| CM
    SM --> IT
    SM --> ADC
    SM --> GPIO
    CM --> IT
    CM --> ADC
    CM --> GPIO
    CMW -.->|слот write никогда не заполняется поставляемыми драйверами| Unreached["нет действующей цели диспетчеризации"]
```

Слоты vtbl `configure`/`self_test`/`write` диспетчеризуются из
`sensor_manager.c`, но всегда равны NULL для трёх поставляемых драйверов —
это действующие точки диспетчеризации без текущей конкретной реализации,
до которой можно было бы дойти.

## Обобщённая диспетчеризация OPC UA

```mermaid
graph TD
    AddVar["model_add_*_node()<br/>model_address_space.c"] -->|"data_source.read ="| Read["model_read_value()<br/>model_history.c:721"]
    AddVar -->|"data_source.write ="| Write["model_write_value()<br/>model_configuration.c:76"]
    Read -->|"диспетчеризация по model_node_context_t.kind"| Services1["channel_manager / config_service /<br/>alarm_service / diag_service / ..."]
    Write -->|"диспетчеризация по model_node_context_t.kind"| Services2["channel_manager_write_value /<br/>config_service_set_* /<br/>channel_manager_update_settings"]
```

Один обобщённый коллбэк Read и один обобщённый коллбэк Write обслуживают
почти каждый узел в адресном пространстве — полную последовательность
запроса см. в [opcua-flow.md](opcua-flow.md).

## Делегирование коллбэков методов сервисам (репрезентативная выборка)

```mermaid
graph LR
    AckAll["AcknowledgeAllAlarms"] --> AlarmAckAll["alarm_service_acknowledge_all()"]
    AckOne["Acknowledge (условие по каналу,<br/>диспетчер A&C open62541)"] --> AlarmAckOne["alarm_service_acknowledge_channel()"]
    PlanM["PlanMaintenance / StartMaintenance /<br/>RescheduleMaintenance / FinishMaintenance"] --> CommitTx["model_commit_maintenance_transaction()"]
    CommitTx --> MSvc["maintenance_service_prepare_* / apply / commit"]
    ApplyCfg["ApplyConfig / SetChannelConfig /<br/>чанки импорта конфигурации"] --> CfgSvc["config_service_* / config_validate_service_* /<br/>config_import_service_*"]
    OtaBegin["BeginFirmwareUpdate / WriteFirmwareChunk /<br/>FinalizeFirmwareUpdate / AbortFirmwareUpdate"] --> FwSvc["firmware_update_service_*()"]
    FwSvc --> OtaFlash["ota_flash_writer_*() / ota_flash_reset_state()"]
```

Каждый коллбэк метода, прослеженный за шесть проходов картирования,
завершается ровно одним вызовом сервиса — прямых изменений состояния
внутри `opcua_model` не обнаружено. См.
[modules/opcua_model.md](modules/opcua_model.md#method-callbacks--representative-traces)
для ссылок на источники и
[ota-bootloader.md](ota-bootloader.md#opc-ua-trigger-chain) для полной цепочки
методов OTA.

## Сериализация записи во Flash

```mermaid
graph TD
    ConfigTxn["drivers/config_transaction_backend.c<br/>(staged/running/last-known-good, v0.5.0 T3)"]
    Security["security_service.c"]
    SecurityState["security_state_service.c"]
    MJ["drivers/maintenance_journal_backend.c<br/>(транзакционный журнал)"]
    HFB["drivers/history_flash_backend.c<br/>(нетранзакционное кольцо)"]
    FOC["drivers/flash_operation_coordinator.c<br/>ЕДИНСТВЕННЫЙ вызывающий flash_range_program/erase<br/>мьютекс FreeRTOS + защита от повторного входа"]

    ConfigTxn --> FOC
    Security --> FOC
    SecurityState --> FOC
    MJ --> FOC
    HFB --> FOC

    ConfigStoreSvc["services/config_store_service.c<br/>(тонкая обёртка, сама Flash не касается)"] --> ConfigTxn
```

Шесть независимых механизмов сохранения состояния более высокого уровня,
одна общая точка сериализации. См.
[hardware.md](hardware.md#flash-driver-layer) и [persistence.md](persistence.md).

## Паттерн аренды чтения/записи канала

```mermaid
sequenceDiagram
    participant Caller as channel_manager_poll_all / write_value
    participant Lock as глобальная критическая секция
    participant Driver as telemetry_driver_vtbl_t

    Caller->>Lock: acquire_driver_operation() — снимок generation+token
    Lock-->>Caller: снятие блокировки
    Caller->>Driver: read()/write() — БЕЗ удержания блокировки
    Driver-->>Caller: результат
    Caller->>Lock: publish_driver_operation() — повторная проверка generation
    alt generation не изменилась
        Lock-->>Caller: фиксация состояния времени выполнения
    else generation изменилась (конкурентная реконфигурация)
        Lock-->>Caller: отклонение — CHANNEL_MANAGER_WRITE_RESULT_STALE
    end
```

## Связанная документация

- [modules/opcua_model.md](modules/opcua_model.md), [modules/services.md](modules/services.md),
  [modules/drivers.md](modules/drivers.md) — полные детали по каждому модулю.
- [runtime-flows.md](runtime-flows.md) — сквозные трассировки потоков.
- `docs/code/topology/topology.json` / `TOPOLOGY.html` — граф на уровне файлов,
  сгенерированный автоматически.
