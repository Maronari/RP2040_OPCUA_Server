if(NOT DEFINED OUTPUT_PATH)
    message(FATAL_ERROR "OUTPUT_PATH is required")
endif()

if(NOT DEFINED FW_REVISION)
    message(FATAL_ERROR "FW_REVISION is required")
endif()

if(NOT DEFINED BUILD_TIME)
    message(FATAL_ERROR "BUILD_TIME is required")
endif()

if(NOT DEFINED GIT_HASH)
    message(FATAL_ERROR "GIT_HASH is required")
endif()

if(NOT DEFINED HW_REVISION)
    message(FATAL_ERROR "HW_REVISION is required")
endif()

# Keep this artifact in sync with g_basic_profile in app/basic_profile/main.c.
set(CONFIG_EXPORT_JSON [==[
{
  "schemaVersion": 1,
  "device": {
    "manufacturer": "RP2 OPC UA",
    "model": "BasicFirmwareProfile",
    "deviceName": "RP2 Basic Device",
    "opcuaHostname": "rp2-basic-profile",
    "serialNumber": "",
    "productInstanceUri": "",
    "assetId": "",
    "hwRevision": "@HW_REVISION@",
    "fwRevision": "@FW_REVISION@",
    "buildTime": "@BUILD_TIME@",
    "gitHash": "@GIT_HASH@",
    "location": "",
    "assetClass": "",
    "manufactureDateUtc": 0,
    "calibrationDateUtc": 0,
    "hierarchicalLocationPath": "",
    "operationalLocationPath": ""
  },
  "network": {
    "mode": "dhcp",
    "mac": "00:08:DC:12:34:56",
    "staticIp": "192.168.1.42",
    "staticNetmask": "255.255.255.0",
    "staticGateway": "192.168.1.0",
    "staticDns": "8.8.8.8"
  },
  "time": {
    "ntpServer": "pool.ntp.org",
    "timezoneOffsetMinutes": 0
  },
  "mqtt": {
    "enabled": false,
    "useJson": true,
    "brokerUrl": "opc.mqtt://127.0.0.1:1883",
    "topicRoot": "devices/rp2-basic-profile"
  },
  "diagnostics": {
    "enableHeapDiagnostics": true,
    "enableTaskDiagnostics": true,
    "watchdogTimeoutMs": 8000
  },
  "opcua": {
    "port": 4840,
    "enableAlarmAckMethod": true
  },
  "channels": [
    {
      "slot": 0,
      "inUse": true,
      "kind": 0,
      "valueType": 0,
      "driverId": 1,
      "sourceIndex": 0,
      "sourceFlags": 0,
      "id": "channel.temperature.1",
      "name": "Temperature1",
      "description": "Internal temperature telemetry channel",
      "unit": "degC",
      "flags": 7,
      "minimum": 0.0,
      "maximum": 0.0,
      "hasMinimum": false,
      "hasMaximum": false,
      "enabled": true,
      "samplePeriodMs": 1000,
      "publishPeriodMs": 1000,
      "historyIntervalMs": 1000,
      "scale": 1.0,
      "offset": 0.0,
      "warningThreshold": 60.0,
      "criticalThreshold": 75.0
    },
    {
      "slot": 1,
      "inUse": false,
      "kind": 0,
      "valueType": 0,
      "driverId": 0,
      "sourceIndex": 0,
      "sourceFlags": 0,
      "id": "",
      "name": "",
      "description": "",
      "unit": "",
      "flags": 0,
      "minimum": 0.0,
      "maximum": 0.0,
      "hasMinimum": false,
      "hasMaximum": false,
      "enabled": false,
      "samplePeriodMs": 0,
      "publishPeriodMs": 0,
      "historyIntervalMs": 0,
      "scale": 0.0,
      "offset": 0.0,
      "warningThreshold": 0.0,
      "criticalThreshold": 0.0
    },
    {
      "slot": 2,
      "inUse": false,
      "kind": 0,
      "valueType": 0,
      "driverId": 0,
      "sourceIndex": 0,
      "sourceFlags": 0,
      "id": "",
      "name": "",
      "description": "",
      "unit": "",
      "flags": 0,
      "minimum": 0.0,
      "maximum": 0.0,
      "hasMinimum": false,
      "hasMaximum": false,
      "enabled": false,
      "samplePeriodMs": 0,
      "publishPeriodMs": 0,
      "historyIntervalMs": 0,
      "scale": 0.0,
      "offset": 0.0,
      "warningThreshold": 0.0,
      "criticalThreshold": 0.0
    },
    {
      "slot": 3,
      "inUse": false,
      "kind": 0,
      "valueType": 0,
      "driverId": 0,
      "sourceIndex": 0,
      "sourceFlags": 0,
      "id": "",
      "name": "",
      "description": "",
      "unit": "",
      "flags": 0,
      "minimum": 0.0,
      "maximum": 0.0,
      "hasMinimum": false,
      "hasMaximum": false,
      "enabled": false,
      "samplePeriodMs": 0,
      "publishPeriodMs": 0,
      "historyIntervalMs": 0,
      "scale": 0.0,
      "offset": 0.0,
      "warningThreshold": 0.0,
      "criticalThreshold": 0.0
    }
  ]
}
]==])

string(CONFIGURE "${CONFIG_EXPORT_JSON}" CONFIG_EXPORT_JSON @ONLY)

if(BENCHMARK_PROFILE STREQUAL "synthetic")
    math(EXPR _period "1000 / ${BENCHMARK_RATE_HZ}")
    math(EXPR _flags "1 + 2 * ${BENCHMARK_HISTORY} + 4 * ${BENCHMARK_EVENTS} + 8")
    string(FIND "${CONFIG_EXPORT_JSON}" "  \"channels\": [" _channels_start)
    string(SUBSTRING "${CONFIG_EXPORT_JSON}" 0 ${_channels_start} CONFIG_EXPORT_JSON)
    string(REPLACE "\"useJson\": true" "\"useJson\": false" CONFIG_EXPORT_JSON "${CONFIG_EXPORT_JSON}")
    string(APPEND CONFIG_EXPORT_JSON "  \"channels\": [\n")
    foreach(_slot RANGE 0 15)
        if(_slot LESS BENCHMARK_CHANNELS)
            math(EXPR _number "${_slot} + 1")
            math(EXPR _source "${_slot} % 8")
            string(APPEND CONFIG_EXPORT_JSON "{\"slot\":${_slot},\"inUse\":true,\"kind\":0,\"valueType\":1,\"driverId\":10,\"sourceIndex\":${_source},\"sourceFlags\":0,\"id\":\"benchmark.synthetic.${_number}\",\"name\":\"BenchmarkSynthetic${_number}\",\"description\":\"Synthetic benchmark channel ${_number}\",\"unit\":\"\",\"flags\":${_flags},\"minimum\":0,\"maximum\":0,\"hasMinimum\":false,\"hasMaximum\":false,\"enabled\":true,\"samplePeriodMs\":${_period},\"publishPeriodMs\":${_period},\"historyIntervalMs\":${_period},\"scale\":1,\"offset\":0,\"warningThreshold\":0,\"criticalThreshold\":0}")
        else()
            string(APPEND CONFIG_EXPORT_JSON "{\"slot\":${_slot},\"inUse\":false,\"kind\":0,\"valueType\":0,\"driverId\":0,\"sourceIndex\":0,\"sourceFlags\":0,\"id\":\"\",\"name\":\"\",\"description\":\"\",\"unit\":\"\",\"flags\":0,\"minimum\":0,\"maximum\":0,\"hasMinimum\":false,\"hasMaximum\":false,\"enabled\":false,\"samplePeriodMs\":0,\"publishPeriodMs\":0,\"historyIntervalMs\":0,\"scale\":0,\"offset\":0,\"warningThreshold\":0,\"criticalThreshold\":0}")
        endif()
        if(_slot LESS 15)
            string(APPEND CONFIG_EXPORT_JSON ",")
        endif()
        string(APPEND CONFIG_EXPORT_JSON "\n")
    endforeach()
    string(APPEND CONFIG_EXPORT_JSON "  ]\n}\n")
endif()

get_filename_component(OUTPUT_DIRECTORY "${OUTPUT_PATH}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
file(WRITE "${OUTPUT_PATH}" "${CONFIG_EXPORT_JSON}\n")
