#ifndef OPCUA_MODEL_OPCUA_MODEL_H
#define OPCUA_MODEL_OPCUA_MODEL_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Server identity and optional-feature configuration for the OPC UA
 *        model.
 *
 * Passed once to opcua_model_init(); not retained by pointer — the model
 * copies out anything it needs.
 */
typedef struct
{
    const char *application_name; /**< OPC UA ApplicationName / product name. */
    const char *hostname;         /**< Hostname used to build the endpoint URL. */
    const char *mqtt_broker_url;  /**< MQTT broker URL, used only if enable_mqtt_pubsub. */
    const char *mqtt_topic_root;  /**< Root topic prefix for PubSub/MQTT publishing. */
    uint16_t port;                /**< OPC UA TCP endpoint port. */
    bool enable_alarm_methods;    /**< Register the AcknowledgeAllAlarms method. */
    bool enable_heap_diagnostics; /**< Expose FreeRTOS heap diagnostic nodes. */
    bool enable_task_diagnostics; /**< Expose per-task stack diagnostic nodes. */
    bool enable_mqtt_pubsub;      /**< Configure OPC UA PubSub over MQTT. */
    bool mqtt_pubsub_use_json;    /**< Use JSON (vs. binary UADP) PubSub encoding. */
} opcua_model_config_t;

/**
 * @brief Prepare the OPC UA model adapter for later startup.
 *
 * Allocates bounded PSRAM scratch buffers (maintenance-export payload,
 * history read-sample buffer) and copies configuration out of @p config.
 * Does **not** create the `UA_Server` instance or build the address space —
 * that happens later, inside opcua_model_task(), gated on network readiness.
 * Call once from the init task, before creating the task that will run
 * opcua_model_task().
 *
 * @param config Server identity/feature configuration. Not retained by
 *        pointer.
 * @return true on success; false if a required PSRAM allocation failed.
 */
bool opcua_model_init(const opcua_model_config_t *config);

/**
 * @brief FreeRTOS task entry point that owns the OPC UA server's lifetime.
 *
 * Requires an initialized local network stack, but does not wait for an IPv4
 * lease in production. Builds the `UA_ServerConfig`, creates the `UA_Server`,
 * builds the full address space, starts the server, and runs
 * `UA_Server_run_iterate()` in a loop. An addressless boot retains the wildcard
 * listener and publishes a discovery address after IPv4 arrives. Explicit
 * synthetic benchmark builds retain their lease-inclusive startup timing.
 * Never returns under normal operation. Two distinct events can end a server's
 * current lifetime:
 * - A scheduled reboot: the server shuts down and the task then blocks
 *   forever, expecting a watchdog/hardware reset to actually restart the
 *   device.
 * - A controlled restart (v0.5.0 T6), armed when a committed configuration
 *   change requires rebuilding the OPC UA namespace
 *   (`model_config_requires_server_reboot()`): the server shuts down and
 *   this same task rebuilds it in place against the newly-committed
 *   configuration, without exiting. Existing sessions/subscriptions do not
 *   survive this (clients must reconnect); a transaction that does not
 *   require it leaves the running server, its sessions, and its
 *   subscriptions completely untouched.
 * Must be started via `xTaskCreate()` after a successful opcua_model_init()
 * call.
 *
 * @param argument Unused; required by the FreeRTOS task-entry signature.
 */
void opcua_model_task(void *argument);

#endif
