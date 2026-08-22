# Потоки запросов OPC UA

Прослеживает путь реализации для Read и Write, от клиента до ответа, как он
реализован фактически. Оба типа запросов проходят через **одни и те же два
обобщённых коллбэка DataSource** для подавляющего большинства узлов — здесь
нет разрастания коллбэков по типам узлов. Полный жизненный цикл сервера и
опись модулей см. в [modules/opcua_model.md](modules/opcua_model.md).

## Read

```mermaid
sequenceDiagram
    participant Client as Клиент
    participant open62541
    participant model_read_value as model_read_value<br/>(model_history.c:721)
    participant Service as сервис (напр., channel_manager)
    participant State as состояние времени выполнения,<br/>принадлежащее сервису

    Client->>open62541: запрос Read (NodeId)
    open62541->>model_read_value: коллбэк UA_DataSource.read
    model_read_value->>model_read_value: диспетчеризация по model_node_context_t.kind
    model_read_value->>Service: напр., channel_manager_copy_snapshot_by_id()
    Service->>State: снимок под критической секцией (O(1))
    State-->>Service: собственная копия (значение, StatusCode, временная метка источника)
    Service-->>model_read_value: снимок
    model_read_value-->>open62541: UA_DataValue (значение + статус + временная метка)
    open62541-->>Client: ответ Read
```

Каждый переменный узел, зарегистрированный через `model_add_*_node` в
`model_address_space.c`, подключает `data_source.read = model_read_value`.
Коллбэк никогда сам не удерживает блокировку сервиса — он вызывает
функцию доступа сервиса (`channel_manager_copy_snapshot_by_id`,
`config_service_get`, `alarm_service_get_condition_by_index` и т. д.),
которая внутренне захватывает и снимает собственную блокировку, а затем
возвращает собственное значение. Отображение качества
(`GOOD`/`UNCERTAIN`/`BAD`→`BadNotConnected`) и преобразование временной
метки источника следуют
[docs/channel_model.md](../channel_model.md#quality-timestamp-and-writes).

## Write

```mermaid
sequenceDiagram
    participant Client as Клиент
    participant open62541
    participant model_write_value as model_write_value<br/>(model_configuration.c:76)
    participant Access as model_session_has_role /<br/>model_required_role_for_write_kind
    participant Service as сервис (напр., channel_manager)
    participant State as состояние времени выполнения,<br/>принадлежащее сервису

    Client->>open62541: запрос Write (NodeId, значение)
    open62541->>model_write_value: коллбэк UA_DataSource.write
    model_write_value->>Access: проверка анонимности/роли для этого типа узла
    Access-->>model_write_value: разрешено / BadUserAccessDenied
    model_write_value->>model_write_value: диспетчеризация по model_node_context_t.kind
    model_write_value->>Service: напр., channel_manager_write_value()
    Service->>State: получение аренды (generation+token) под критической секцией
    Service->>Service: запись через драйвер, ВНЕ какой-либо блокировки
    Service->>State: публикация только если generation всё ещё совпадает
    State-->>Service: успех / CHANNEL_MANAGER_WRITE_RESULT_STALE
    Service-->>model_write_value: перечисление результата
    model_write_value-->>open62541: UA_StatusCode
    open62541-->>Client: ответ Write
```

`model_write_value` — чистый адаптер для каждой найденной ветви
диспетчеризации: `channel_manager_write_value` (значения каналов),
`config_service_set_asset_id`, `channel_manager_update_settings` (период
выборки/публикации, смещение/масштаб, пороги тревог),
`config_service_set_timezone_offset_minutes`. Прямого изменения полей
состояния, принадлежащего сервису, в этой функции не обнаружено.

Авторизация проверяется **дважды**: один раз на уровне плагина
`UA_AccessControl` (`model_get_user_access_level_wrapper`,
[modules/opcua_model.md](modules/opcua_model.md#access-control)) ещё до того,
как open62541 вообще вызывает запись DataSource, и повторно внутри самой
`model_write_value`.

## Вызов метода OPC UA (пример: Acknowledge для тревоги канала)

```mermaid
sequenceDiagram
    participant Client as Клиент
    participant open62541 as диспетчер методов open62541
    participant Callback as model_channel_alarm_acked_callback<br/>(model_events.c:948)
    participant AlarmService as alarm_service_acknowledge_channel()

    Client->>open62541: Call(Acknowledge, conditionId, comment)
    open62541->>open62541: встроенная обработка двухсостояниевого перехода A&C
    open62541->>Callback: UA_TwoStateVariableChangeCallback
    Callback->>Callback: разрешение NodeId условия -> слот канала
    Callback->>AlarmService: alarm_service_acknowledge_channel(channel_id)
    AlarmService-->>Callback: acknowledgement_revision увеличена
    Callback-->>open62541: статус
    open62541-->>Client: ответ Call
```

Путь обобщённого узла-Method (например, `PlanMaintenance`,
`BeginFirmwareUpdate`, `AcknowledgeAllAlarms`) следует той же схеме:
`model_require_method_role()` защищает вызов, затем тело делегирует ровно
одной функции сервиса. Трассировки для конфигурации и обслуживания см. в
[runtime-flows.md](runtime-flows.md), а методы OTA — в
[ota-bootloader.md](ota-bootloader.md#opc-ua-trigger-chain).

## Связанная документация

- [modules/opcua_model.md](modules/opcua_model.md) — полная опись модуля и
  жизненный цикл сервера.
- [call-graphs.md](call-graphs.md) — диаграммы таблиц диспетчеризации.
- [freertos.md](freertos.md) — `OpcUaTask`.
