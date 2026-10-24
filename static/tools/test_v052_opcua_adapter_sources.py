#!/usr/bin/env python3
"""Fail-closed source and additive public-contract checks for v0.5.2 T2/T3."""

from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"v0.5.2-opcua-source: FAIL: {message}")


def main() -> int:
    inherited = subprocess.run(
        [sys.executable, "static/tools/test_v051_opcua_adapter_sources.py",
         "--skip-public-contract"], cwd=ROOT, check=False)
    require(inherited.returncode == 0, "inherited v0.5.1 adapter invariants failed")

    access = (ROOT / "opcua_model/src/model_access.c").read_text(encoding="utf-8")
    address = (ROOT / "opcua_model/src/model_address_space.c").read_text(encoding="utf-8")
    history = (ROOT / "opcua_model/src/model_history.c").read_text(encoding="utf-8")
    lifecycle = (ROOT / "opcua_model/src/model_core_lifecycle.c").read_text(encoding="utf-8")
    common = (ROOT / "opcua_model/src/model_common.c").read_text(encoding="utf-8")
    port = (ROOT / "port/open62541/open62541.c").read_text(encoding="utf-8")

    require("model_node_is_t2_resource_diagnostic" in access and
            "SECURITY_ROLE_SERVICE" in access,
            "resource nodes are not browse/read restricted to service/admin")
    require("model_add_t2_task_subtree" in address and
            "UA_Server_deleteNode(server, UA_NODEID_STRING(1, object_id), true)" in address,
            "dynamic task subtree construction is not transactional")
    require("g_model_resource_task_model_incomplete = incomplete" in address,
            "task-model construction failure is not published")
    require("UA_STATUSCODE_BADNODATA" in history and
            "UA_STATUSCODE_BADNOTSUPPORTED" in history and
            "UA_STATUSCODE_UNCERTAINLASTUSABLEVALUE" in history,
            "frozen unavailable/stale quality mapping is incomplete")
    require("g_model_resource_snapshot_cache_monotonic_ms" in history and
            "snapshot->age_ms + elapsed_ms" in history,
            "SnapshotAgeMs is frozen when the adapter cache becomes stale")
    require("pvPortMalloc(sizeof(*g_model_resource_snapshot_cache))" in lifecycle and
            "psram_malloc(sizeof(*g_model_resource_snapshot_cache))" in lifecycle,
            "bounded snapshot cache lacks PSRAM preference and heap fallback")
    for resource in ("Sessions", "SecureChannels", "Subscriptions", "MonitoredItems",
                     "PublishRequests", "EventQueue"):
        for field in ("Current", "Peak", "Capacity"):
            require(f'"Device.Diagnostics.OpcUa.{resource}.{field}"' in address,
                    f"missing T3 resource node: {resource}.{field}")
    for field in ("RejectedRequests", "RejectedRequestsWrapCount",
                  "EventQueueDrops", "EventQueueDropsWrapCount"):
        require(f'"Device.Diagnostics.OpcUa.{field}"' in address,
                f"missing T3 counter node: {field}")
    require("config->secureChannelNotificationCallback = model_server_notification_callback" in lifecycle and
            "config->sessionNotificationCallback = model_server_notification_callback" in lifecycle and
            "config->subscriptionNotificationCallback = model_server_notification_callback" in lifecycle,
            "T3 lifecycle callbacks are not installed")
    begin_offset = lifecycle.index("opcua_resource_service_begin_server(&resource_config)")
    first_active_sample = lifecycle.index("model_sample_resource_diagnostics(server);", begin_offset)
    address_build = lifecycle.index("model_build_address_space(server)", begin_offset)
    require(begin_offset < first_active_sample < address_build,
            "T3 active capacities are not sampled before first address-space publication")
    require("opcua_resource_service_acquire(OPCUA_RESOURCE_SERVICE_MONITORED_ITEMS)" in common and
            "opcua_resource_service_release(OPCUA_RESOURCE_SERVICE_MONITORED_ITEMS)" in common,
            "T3 monitored-item lifecycle is not accounted")
    require("model_read_t3_opcua_resource_diagnostic" in history and
            "OPCUA_RESOURCE_SERVICE_QUALITY_BAD_NOT_SUPPORTED" in history,
            "T3 snapshot status adapter is missing")
    for owner_hook in ("UA_Server_resourcePublishRequestQueued(server)",
                       "UA_Server_resourcePublishRequestDequeued(server)",
                       "UA_Server_resourceEventQueued(mon->subscription)",
                       "UA_Server_resourceEventDequeued(mon->subscription)",
                       "UA_Server_resourceEventDropped(sub)"):
        require(owner_hook in port, f"missing definitive T3 queue hook: {owner_hook}")
    require("UA_Server_getResourceUsage(UA_Server *server, UA_ServerResourceUsage *usage)" in port and
            "usage->eventQueueCapacity +=" in port and
            "usage->rejectedRequestsAvailable = true" in port,
            "T3 owner snapshot does not expose the bounded resource counters")
    overflow_function = port.split("createEventOverflowNotification", 1)[-1]
    require("UA_Server_resourceEventQueued(mon->subscription);" in overflow_function,
            "overflow marker bypasses the event occupancy hook")
    require(port.count("++mon->queueSize;") == 2 and
            port.count("--mon->queueSize;") == 1,
            "unreviewed per-item event queue mutation site was added")
    subscription_delete = port.split(
        "UA_Subscription_delete(UA_Server *server, UA_Subscription *sub) {", 1)[1]
    subscription_delete = subscription_delete.split("Subscription_resetLifetime", 1)[0]
    require("if(sub->resourceLifecycleNotified)" in subscription_delete and
            "UA_APPLICATIONNOTIFICATIONTYPE_SUBSCRIPTION_DELETED" in subscription_delete and
            "sub->resourceLifecycleNotified = false" in subscription_delete,
            "subscription destruction is not centrally reported exactly once")
    delete_operation = port.split("Operation_DeleteSubscription", 1)[1]
    delete_operation = delete_operation.split("Service_DeleteSubscriptions", 1)[0]
    require("UA_APPLICATIONNOTIFICATIONTYPE_SUBSCRIPTION_DELETED" not in delete_operation,
            "explicit DeleteSubscriptions duplicates the centralized lifecycle notification")
    transfer_operation = port.split("Operation_TransferSubscription", 1)[1]
    transfer_operation = transfer_operation.split("Service_TransferSubscriptions", 1)[0]
    require("newSub->resourceLifecycleNotified = sub->resourceLifecycleNotified" in
            transfer_operation and
            "sub->resourceLifecycleNotified = false" in transfer_operation,
            "subscription transfer does not preserve lifecycle counter ownership")
    create_operation = port.rsplit("Service_CreateSubscription", 1)[1]
    create_operation = create_operation.split("Service_ModifySubscription", 1)[0]
    require(create_operation.index("UA_APPLICATIONNOTIFICATIONTYPE_SUBSCRIPTION_CREATED") <
            create_operation.index("sub->resourceLifecycleNotified = true"),
            "failed subscription construction can be treated as application-visible")
    session_remove = port.split("UA_Session_remove(UA_Server *server,", 1)[1]
    session_remove = session_remove.split("/* Callback into userland access control */", 1)[0]
    require("UA_SHUTDOWNREASON_TIMEOUT" in session_remove and
            "UA_Session_detachSubscription(server, session, sub, true)" in session_remove and
            "UA_Subscription_delete(server, sub)" in session_remove,
            "session close/timeout subscription lifecycle paths changed without review")

    contract = subprocess.run(
        [sys.executable, "static/tools/verify_opcua_public_contract.py",
         "--baseline", "2881230c2ccbb98b8701730c4de930dc95da30fc",
         "--allow-v052-additions"], cwd=ROOT, check=False)
    require(contract.returncode == 0, "v0.5.1 additive public-contract comparison failed")
    print("v0.5.2-opcua-source: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
