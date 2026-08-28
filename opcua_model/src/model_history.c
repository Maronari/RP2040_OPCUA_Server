/**
 * @file
 * @brief Generic `UA_DataSource.read` dispatch (`model_read_value`) and the
 *        `UA_HistoryDatabase.readRaw` backend (`model_history_read_raw`),
 *        including the HistoryRead continuation-point table.
 *
 * Responsibilities:
 * - `UA_DataValue` typed-setter helpers shared by both the read dispatch and
 *   the history backend (`model_set_*_value`, `model_fill_history_data_value`).
 * - `model_read_value`: the single `UA_DataSource.read` callback installed on
 *   nearly every variable node this model creates, dispatching on
 *   `model_node_context_t::kind` to the owning service's current snapshot.
 *   Read-only — never mutates service state.
 * - `model_history_read_raw`: the `UA_HistoryDatabase.readRaw` plugin
 *   implementation. Resolves each requested NodeId to a channel id
 *   (`model_find_history_channel_id`), manages a fixed-size, TTL'd
 *   continuation-point table for paging (`MODEL_HISTORY_CONTINUATION_CAPACITY`
 *   slots, `MODEL_HISTORY_CONTINUATION_EXPIRY_MS` TTL), and delegates the
 *   actual sample lookup to `history_service_read_channel_samples` — this
 *   file never touches the channel history ring buffer directly.
 *
 * Continuation-point design (see the block comment above
 * `MODEL_HISTORY_CONTINUATION_MAGIC` below): the 12-byte wire-visible
 * continuation point is an opaque, tamper-evident token (magic + token +
 * XOR checksum) into a server-side-only table; the table itself carries the
 * owning session, resolved channel/NodeId, requested range/direction, and a
 * generation counter tying a resume to the exact retained-window state that
 * produced it. A continuation point can never be replayed by another
 * session (`session_context` is compared, never dereferenced), cannot grow
 * without bound (fixed `MODEL_HISTORY_CONTINUATION_CAPACITY` table, oldest
 * slot is not evicted — allocation simply fails with
 * `BADNOCONTINUATIONPOINTS` when full), and is invalidated rather than
 * silently returning skewed data if the channel's retained window changed
 * (ring buffer wraparound) since it was issued.
 *
 * Ownership:
 * The continuation table (`opcua_model_context_t::history_continuations`)
 * and the single-shot HistoryRead sample scratch buffer
 * (`history_read_sample_buffer`, PSRAM-backed, sized
 * `HISTORY_SERVICE_MAX_RETURN_COUNT`) live in `opcua_model_context_t`; see
 * `opcua_model_internal.h` for both. Sample data itself is owned by
 * `history_service`'s Flash-backed ring buffer, never cached here beyond
 * one read call.
 *
 * Concurrency:
 * `model_read_value` and `model_history_read_raw` both run on `OpcUaTask`
 * inside open62541's synchronous Read/HistoryRead service dispatch; the
 * continuation table is therefore only ever touched from that one task and
 * needs no locking of its own (unlike `history_service`'s ring buffer,
 * which has its own internal synchronization for cross-task writers).
 */
#include "opcua_model_internal.h"

UA_StatusCode model_set_localized_text_value(UA_DataValue *value,
                                                    const char *data,
                                                    UA_StatusCode status)
{
    UA_LocalizedText localized =
        UA_LOCALIZEDTEXT((char *)"en-US", (char *)(data != NULL ? data : ""));
    UA_Variant_setScalarCopy(&value->value,
                             &localized,
                             &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode model_set_datetime_value(UA_DataValue *value, UA_DateTime data, UA_StatusCode status)
{
    UA_Variant_setScalarCopy(&value->value, &data, &UA_TYPES[UA_TYPES_DATETIME]);
    value->hasValue = true;
    value->hasStatus = true;
    value->status = status;
    return UA_STATUSCODE_GOOD;
}

void model_set_default_variant_value(UA_VariableAttributes *attributes, UA_NodeId data_type)
{
    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_BOOLEAN].typeId))
    {
        UA_Boolean default_value = false;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_BOOLEAN]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_UINT32].typeId))
    {
        UA_UInt32 default_value = 0u;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_UINT32]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_INT32].typeId))
    {
        UA_Int32 default_value = 0;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_INT32]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_INT16].typeId))
    {
        UA_Int16 default_value = 0;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_INT16]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_FLOAT].typeId))
    {
        UA_Float default_value = 0.0f;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_FLOAT]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_DOUBLE].typeId))
    {
        UA_Double default_value = 0.0;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_DOUBLE]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_INT64].typeId))
    {
        UA_Int64 default_value = 0;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_INT64]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_UINT64].typeId))
    {
        UA_UInt64 default_value = 0u;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_UINT64]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_DATETIME].typeId))
    {
        UA_DateTime default_value = 0;
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_DATETIME]);
        return;
    }

    if (UA_NodeId_equal(&data_type, &UA_TYPES[UA_TYPES_STRING].typeId))
    {
        UA_String default_value = UA_STRING((char *)"");
        UA_Variant_setScalarCopy(&attributes->value, &default_value, &UA_TYPES[UA_TYPES_STRING]);
    }
}

UA_StatusCode model_set_channel_runtime_value(UA_DataValue *value,
                                                     const telemetry_channel_t *channel,
                                                     UA_StatusCode status)
{
    if (value == NULL || channel == NULL)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    switch (channel->metadata.value_type)
    {
    case TELEMETRY_CHANNEL_VALUE_DOUBLE:
        return model_set_double_value(value, channel->runtime.value.f64, status);
    case TELEMETRY_CHANNEL_VALUE_INT32:
        return model_set_int32_value(value, channel->runtime.value.i32, status);
    case TELEMETRY_CHANNEL_VALUE_UINT32:
        return model_set_uint32_value(value, channel->runtime.value.u32, status);
    case TELEMETRY_CHANNEL_VALUE_INT64:
        return model_set_int64_value(value, (UA_Int64)channel->runtime.value.i64, status);
    case TELEMETRY_CHANNEL_VALUE_UINT64:
        return model_set_uint64_value(value, (UA_UInt64)channel->runtime.value.u64, status);
    case TELEMETRY_CHANNEL_VALUE_BOOLEAN:
        return model_set_boolean_value(value, channel->runtime.value.boolean, status);
    case TELEMETRY_CHANNEL_VALUE_STRING:
        return model_set_string_value(value, channel->runtime.value.string, status);
    case TELEMETRY_CHANNEL_VALUE_FLOAT:
    default:
        return model_set_float_value(value, channel->runtime.value.f32, status);
    }
}

#ifdef UA_ENABLE_HISTORIZING
/**
 * @brief Resolves a HistoryRead-requested NodeId to its stable channel id by
 *        linearly scanning every historized channel's `Value` NodeId (both
 *        the v0.4.2 generic channel-namespace form and the legacy ns=1
 *        form) for a match.
 *
 * @param node_id NodeId as supplied in the `UA_HistoryReadValueId`.
 * @param out_channel_id Output buffer for the matched channel id.
 * @return false if @p node_id does not resolve to any channel currently
 *         flagged `TELEMETRY_CHANNEL_FLAG_HISTORIZED`, or a buffer is too
 *         small; callers map this to `UA_STATUSCODE_BADHISTORYOPERATIONUNSUPPORTED`.
 * @note External linkage but only called from within this translation unit
 *       (`model_history_read_raw`); not declared in
 *       `opcua_model_internal.h`.
 */
bool model_find_history_channel_id(const UA_NodeId *node_id,
                                   char *out_channel_id,
                                   size_t out_channel_id_size)
{
    char channel_value_node_id[128];

    if (node_id == NULL || out_channel_id == NULL || out_channel_id_size == 0u)
    {
        return false;
    }

    for (size_t channel_index = 0u; channel_index < PLATFORM_CHANNEL_LIMIT; ++channel_index)
    {
        telemetry_channel_snapshot_t snapshot = {0};
        const telemetry_channel_t *channel = &snapshot.channel;
        UA_NodeId channel_value_node;

        if (!channel_manager_copy_snapshot_by_index(channel_index, &snapshot) ||
            (channel->metadata.flags & TELEMETRY_CHANNEL_FLAG_HISTORIZED) == 0u ||
            !model_build_channel_node_id(channel, "Value", channel_value_node_id, sizeof(channel_value_node_id)))
        {
            continue;
        }

        channel_value_node = UA_NODEID_STRING(g_model_channel_namespace_index,
                                              channel_value_node_id);
        if (UA_NodeId_equal(node_id, &channel_value_node))
        {
            model_copy_string_safe(out_channel_id, out_channel_id_size, channel->metadata.id);
            return out_channel_id[0] != '\0';
        }
        if (model_build_legacy_channel_node_id(channel, "Value", channel_value_node_id,
                                               sizeof(channel_value_node_id)))
        {
            channel_value_node = UA_NODEID_STRING(1, channel_value_node_id);
            if (UA_NodeId_equal(node_id, &channel_value_node))
            {
                model_copy_string_safe(out_channel_id, out_channel_id_size, channel->metadata.id);
                return out_channel_id[0] != '\0';
            }
        }
    }

    return false;
}

uint32_t model_datetime_to_epoch_seconds(UA_DateTime value)
{
    if (value <= 0)
    {
        return 0u;
    }

    return (uint32_t)UA_DateTime_toUnixTime(value);
}

void model_fill_history_data_value(const history_service_channel_sample_t *sample,
                                          UA_TimestampsToReturn timestamps_to_return,
                                          UA_DataValue *value)
{
    UA_DateTime timestamp = 0;
    const UA_StatusCode status = model_quality_to_status(sample->quality);

    UA_DataValue_init(value);

    /* Mirrors model_set_channel_runtime_value()'s live-read type mapping so
     * HistoryRead reports the channel's real type instead of a hardcoded
     * Float, for every type history_service can retain (STRING channels are
     * never historized, so no STRING case is needed here). */
    switch (sample->value_type)
    {
    case TELEMETRY_CHANNEL_VALUE_DOUBLE:
        (void)model_set_double_value(value, sample->value.f64, status);
        break;
    case TELEMETRY_CHANNEL_VALUE_INT32:
        (void)model_set_int32_value(value, sample->value.i32, status);
        break;
    case TELEMETRY_CHANNEL_VALUE_UINT32:
        (void)model_set_uint32_value(value, sample->value.u32, status);
        break;
    case TELEMETRY_CHANNEL_VALUE_INT64:
        (void)model_set_int64_value(value, (UA_Int64)sample->value.i64, status);
        break;
    case TELEMETRY_CHANNEL_VALUE_UINT64:
        (void)model_set_uint64_value(value, (UA_UInt64)sample->value.u64, status);
        break;
    case TELEMETRY_CHANNEL_VALUE_BOOLEAN:
        (void)model_set_boolean_value(value, sample->value.boolean, status);
        break;
    case TELEMETRY_CHANNEL_VALUE_FLOAT:
    default:
        (void)model_set_float_value(value, sample->value.f32, status);
        break;
    }

    (void)model_source_timestamp_to_datetime(sample->source_timestamp_unix_us, &timestamp);

    if (timestamps_to_return == UA_TIMESTAMPSTORETURN_SOURCE ||
        timestamps_to_return == UA_TIMESTAMPSTORETURN_BOTH)
    {
        value->hasSourceTimestamp = true;
        value->sourceTimestamp = timestamp;
    }

    if (timestamps_to_return == UA_TIMESTAMPSTORETURN_SERVER ||
        timestamps_to_return == UA_TIMESTAMPSTORETURN_BOTH)
    {
        value->hasServerTimestamp = true;
        value->serverTimestamp = timestamp;
    }
}

void model_history_database_clear(UA_HistoryDatabase *history_database)
{
    (void)history_database;
}

/*
 * v0.4.3 deliberately keeps HistoryReadRaw cursors server-side in a fixed
 * table.  The 12-byte wire value is only an opaque table key; the table holds
 * the owner session, node/channel, query range and retained-window generation.
 * Consequently a cursor cannot be replayed by another session, cannot grow
 * without bound, and is invalidated (rather than silently skewed) after the
 * underlying history ring generation changes.  Clients restart from their
 * requested range after BADCONTINUATIONPOINTINVALID.
 */
#define MODEL_HISTORY_CONTINUATION_MAGIC 0x48524350u /* "PCRH", little-endian */

/** @brief Writes @p value to @p out as 4 little-endian bytes; wire-format
 *         primitive for the continuation-point encoding below. */
static void model_history_write_u32(uint8_t *out, uint32_t value)
{
    for (size_t i = 0u; i < 4u; ++i)
    {
        out[i] = (uint8_t)(value >> (8u * i));
    }
}

/** @brief Reads 4 little-endian bytes from @p in as a `uint32_t`; inverse of
 *         `model_history_write_u32`. */
static uint32_t model_history_read_u32(const uint8_t *in)
{
    uint32_t value = 0u;
    for (size_t i = 0u; i < 4u; ++i)
    {
        value |= (uint32_t)in[i] << (8u * i);
    }
    return value;
}

/**
 * @brief Encodes @p token into the 12-byte wire continuation-point format:
 *        magic (4 bytes) + token (4 bytes) + XOR checksum of both against a
 *        fixed constant (4 bytes). Allocates @p out via
 *        `UA_ByteString_allocBuffer`.
 *
 * The checksum exists only to reject a client-mutated/garbage continuation
 * point cheaply (`model_history_decode_continuation_point`) before doing a
 * table lookup; it is not a security boundary — the actual authority check
 * is the server-side table's `session_context`/query-match comparison.
 *
 * @return false if @p out is NULL or the buffer allocation fails.
 */
static bool model_history_encode_continuation_point(uint32_t token, UA_ByteString *out)
{
    uint8_t bytes[MODEL_HISTORY_CONTINUATION_POINT_SIZE];

    if (out == NULL)
    {
        return false;
    }
    model_history_write_u32(&bytes[0], MODEL_HISTORY_CONTINUATION_MAGIC);
    model_history_write_u32(&bytes[4], token);
    model_history_write_u32(&bytes[8], token ^ MODEL_HISTORY_CONTINUATION_MAGIC ^ 0xa5c3f17du);
    if (UA_ByteString_allocBuffer(out, sizeof(bytes)) != UA_STATUSCODE_GOOD)
    {
        return false;
    }
    memcpy(out->data, bytes, sizeof(bytes));
    return true;
}

/**
 * @brief Validates and decodes a client-supplied continuation-point
 *        `UA_ByteString` (magic + checksum), the inverse of
 *        `model_history_encode_continuation_point`.
 * @param out_token Set to the decoded token only on success.
 * @return false for a wrong-length buffer, wrong magic, mismatched
 *         checksum, or a decoded token of 0 (never issued, so treated as
 *         invalid). Callers map a false return to
 *         `UA_STATUSCODE_BADCONTINUATIONPOINTINVALID`.
 */
static bool model_history_decode_continuation_point(const UA_ByteString *in, uint32_t *out_token)
{
    uint32_t token;

    if (in == NULL || out_token == NULL || in->data == NULL ||
        in->length != MODEL_HISTORY_CONTINUATION_POINT_SIZE ||
        model_history_read_u32(&in->data[0]) != MODEL_HISTORY_CONTINUATION_MAGIC)
    {
        return false;
    }
    token = model_history_read_u32(&in->data[4]);
    if (token == 0u || model_history_read_u32(&in->data[8]) !=
                           (token ^ MODEL_HISTORY_CONTINUATION_MAGIC ^ 0xa5c3f17du))
    {
        return false;
    }
    *out_token = token;
    return true;
}

/** @brief Wraparound-safe `now >= deadline` check for `TickType_t`
 *         (unsigned) tick counters, using the standard signed-difference
 *         trick so a `xTaskGetTickCount()` rollover does not cause an
 *         already-expired continuation point to look freshly valid. */
static bool model_history_tick_reached(TickType_t now, TickType_t deadline)
{
    return (TickType_t)(now - deadline) < (TickType_t)(~(TickType_t)0u / 2u);
}

/** @brief Zeroes (deactivates) every continuation-point slot whose
 *         `expires_at` has passed as of @p now; called once per
 *         `model_history_read_raw` invocation before any lookup/reservation
 *         so expired slots are never matched or hold up the table. */
static void model_history_expire_continuations(TickType_t now)
{
    for (size_t index = 0u; index < MODEL_HISTORY_CONTINUATION_CAPACITY; ++index)
    {
        model_history_continuation_t *const continuation = &g_model_history_continuations[index];
        if (continuation->active && model_history_tick_reached(now, continuation->expires_at))
        {
            memset(continuation, 0, sizeof(*continuation));
        }
    }
}

/**
 * @brief Extracts the namespace index and string identifier from a
 *        string-form `UA_NodeId`, the normalized key
 *        `model_history_continuation_query_matches`/
 *        `model_history_reserve_continuation` use to identify which node a
 *        continuation point belongs to (channel Value NodeIds are always
 *        string NodeIds in this model).
 * @return false for a non-string `node_id`, a NULL identifier buffer, or an
 *         identifier that does not fit @p out_identifier_size.
 */
static bool model_history_capture_node(const UA_NodeId *node_id,
                                       UA_UInt16 *out_namespace_index,
                                       char *out_identifier,
                                       size_t out_identifier_size)
{
    if (node_id == NULL || out_namespace_index == NULL || out_identifier == NULL ||
        out_identifier_size == 0u || node_id->identifierType != UA_NODEIDTYPE_STRING ||
        node_id->identifier.string.data == NULL ||
        node_id->identifier.string.length >= out_identifier_size)
    {
        return false;
    }
    *out_namespace_index = node_id->namespaceIndex;
    memcpy(out_identifier, node_id->identifier.string.data, node_id->identifier.string.length);
    out_identifier[node_id->identifier.string.length] = '\0';
    return true;
}

/**
 * @brief True if @p continuation is active and every field of the *current*
 *        HistoryRead request (session, requested NodeId, resolved channel,
 *        time range, direction) matches what was recorded when the
 *        continuation point was reserved.
 *
 * This is the authority check behind resuming a page: a decoded token alone
 * is not sufficient — the request must also still match, so a token cannot
 * be reused across sessions or replayed against a different node/range even
 * if somehow guessed.
 */
static bool model_history_continuation_query_matches(const model_history_continuation_t *continuation,
                                                      const void *session_context,
                                                      const UA_NodeId *node_id,
                                                      const char *channel_id,
                                                      uint32_t start_epoch_s,
                                                      uint32_t end_epoch_s,
                                                      bool descending)
{
    UA_UInt16 namespace_index = 0u;
    char node_identifier[MODEL_CHANNEL_NODE_ID_MAX_LEN];

    return continuation != NULL && continuation->active &&
           continuation->session_context == session_context &&
           continuation->start_epoch_s == start_epoch_s &&
           continuation->end_epoch_s == end_epoch_s &&
           continuation->descending == descending && channel_id != NULL &&
           strcmp(continuation->channel_id, channel_id) == 0 &&
           model_history_capture_node(node_id, &namespace_index, node_identifier, sizeof(node_identifier)) &&
           continuation->node_namespace_index == namespace_index &&
           strcmp(continuation->node_identifier, node_identifier) == 0;
}

/**
 * @brief Linear lookup of the active continuation-point slot matching
 *        @p token whose recorded query also matches the current request
 *        (`model_history_continuation_query_matches`).
 * @return NULL if no slot matches token+query, mapped by the caller to
 *         `UA_STATUSCODE_BADCONTINUATIONPOINTINVALID`.
 */
static model_history_continuation_t *model_history_find_continuation(
    uint32_t token,
    const void *session_context,
    const UA_NodeId *node_id,
    const char *channel_id,
    uint32_t start_epoch_s,
    uint32_t end_epoch_s,
    bool descending)
{
    for (size_t index = 0u; index < MODEL_HISTORY_CONTINUATION_CAPACITY; ++index)
    {
        model_history_continuation_t *const continuation = &g_model_history_continuations[index];
        if (continuation->active && continuation->token == token &&
            model_history_continuation_query_matches(continuation, session_context, node_id, channel_id,
                                                     start_epoch_s, end_epoch_s, descending))
        {
            return continuation;
        }
    }
    return NULL;
}

/**
 * @brief Allocates a free continuation-point slot for a `HistoryReadRaw`
 *        page that has more data remaining, recording the query/skip-cursor
 *        state needed to resume it.
 *
 * Scans linearly for the first inactive slot (no LRU/eviction — a full
 * table simply fails allocation); the returned slot's `token` is a
 * monotonically incrementing counter (`g_model_history_continuation_next_token`,
 * skipping 0 since 0 is reserved to mean "no valid token" by
 * `model_history_decode_continuation_point`), unique across all currently
 * active continuation points regardless of session.
 *
 * @return NULL if @p node_id/@p channel_id are invalid, or the table is
 *         full (`MODEL_HISTORY_CONTINUATION_CAPACITY` slots all active);
 *         mapped by the caller to `UA_STATUSCODE_BADNOCONTINUATIONPOINTS`.
 */
static model_history_continuation_t *model_history_reserve_continuation(
    const void *session_context,
    const UA_NodeId *node_id,
    const char *channel_id,
    uint32_t generation,
    uint32_t skip_matches,
    uint32_t start_epoch_s,
    uint32_t end_epoch_s,
    bool descending,
    TickType_t now)
{
    UA_UInt16 namespace_index = 0u;
    char node_identifier[MODEL_CHANNEL_NODE_ID_MAX_LEN];
    model_history_continuation_t *continuation = NULL;
    TickType_t lifetime = pdMS_TO_TICKS(MODEL_HISTORY_CONTINUATION_EXPIRY_MS);

    if (!model_history_capture_node(node_id, &namespace_index, node_identifier, sizeof(node_identifier)) ||
        channel_id == NULL || channel_id[0] == '\0')
    {
        return NULL;
    }
    if (lifetime == 0u)
    {
        lifetime = 1u;
    }
    for (size_t index = 0u; index < MODEL_HISTORY_CONTINUATION_CAPACITY; ++index)
    {
        if (!g_model_history_continuations[index].active)
        {
            continuation = &g_model_history_continuations[index];
            break;
        }
    }
    if (continuation == NULL)
    {
        return NULL;
    }

    ++g_model_history_continuation_next_token;
    if (g_model_history_continuation_next_token == 0u)
    {
        ++g_model_history_continuation_next_token;
    }
    memset(continuation, 0, sizeof(*continuation));
    continuation->active = true;
    continuation->session_context = session_context;
    continuation->token = g_model_history_continuation_next_token;
    continuation->generation = generation;
    continuation->skip_matches = skip_matches;
    continuation->start_epoch_s = start_epoch_s;
    continuation->end_epoch_s = end_epoch_s;
    continuation->descending = descending;
    continuation->expires_at = now + lifetime;
    continuation->node_namespace_index = namespace_index;
    (void)snprintf(continuation->channel_id, sizeof(continuation->channel_id), "%s", channel_id);
    (void)snprintf(continuation->node_identifier, sizeof(continuation->node_identifier), "%s", node_identifier);
    return continuation;
}

void model_history_release_session_points(const void *session_context)
{
    if (session_context == NULL)
    {
        return;
    }
    for (size_t index = 0u; index < MODEL_HISTORY_CONTINUATION_CAPACITY; ++index)
    {
        if (g_model_history_continuations[index].active &&
            g_model_history_continuations[index].session_context == session_context)
        {
            memset(&g_model_history_continuations[index], 0,
                   sizeof(g_model_history_continuations[index]));
        }
    }
}

void model_history_read_raw(UA_Server *server,
                                   void *history_database_context,
                                   const UA_NodeId *session_id,
                                   void *session_context,
                                   const UA_RequestHeader *request_header,
                                   const UA_ReadRawModifiedDetails *history_read_details,
                                   UA_TimestampsToReturn timestamps_to_return,
                                   UA_Boolean release_continuation_points,
                                   size_t nodes_to_read_size,
                                   const UA_HistoryReadValueId *nodes_to_read,
                                   UA_HistoryReadResponse *response,
                                   UA_HistoryData *const *const history_data)
{
    /* open62541 dispatches this database callback on the single server task. */
    history_service_channel_sample_t *const sample_buffer = g_model_history_read_sample_buffer;
    const uint32_t start_epoch_s = history_read_details != NULL
                                       ? model_datetime_to_epoch_seconds(history_read_details->startTime)
                                       : 0u;
    const uint32_t end_epoch_s = history_read_details != NULL
                                     ? model_datetime_to_epoch_seconds(history_read_details->endTime)
                                     : 0u;
    const bool descending = history_read_details != NULL &&
                            history_read_details->startTime > history_read_details->endTime;
    /* HISTORY_SERVICE_MAX_RETURN_COUNT is the frozen hard per-call cap
     * (see services/include/services/history_service.h); a smaller
     * client-requested numValuesPerNode is honored, a larger or unbounded
     * (0) request is capped, with a continuation point offered for the
     * remainder. */
    const size_t max_values_per_node = (history_read_details == NULL ||
                                        history_read_details->numValuesPerNode == 0u ||
                                        history_read_details->numValuesPerNode > HISTORY_SERVICE_MAX_RETURN_COUNT)
                                           ? HISTORY_SERVICE_MAX_RETURN_COUNT
                                           : (size_t)history_read_details->numValuesPerNode;
    const TickType_t now = xTaskGetTickCount();
    (void)server;
    (void)history_database_context;
    (void)session_id;
    (void)request_header;

    if (sample_buffer == NULL)
    {
        for (size_t index = 0u; index < nodes_to_read_size; ++index)
        {
            response->results[index].statusCode = UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
        }
        return;
    }

    for (size_t index = 0u; index < nodes_to_read_size; ++index)
    {
        UA_HistoryData *result_history_data = history_data[index];
        size_t sample_count = 0u;
        bool history_more = false;
        bool history_busy = false;
        uint32_t token = 0u;
        uint32_t requested_generation = 0u;
        uint32_t skip_matches = 0u;
        uint32_t observed_generation = 0u;
        char history_channel_id[TELEMETRY_CHANNEL_ID_MAX_LEN] = {0};
        model_history_continuation_t *continuation = NULL;
        bool reserved_continuation = false;

        if (result_history_data == NULL)
        {
            response->results[index].statusCode = UA_STATUSCODE_BADOUTOFMEMORY;
            continue;
        }

        if (nodes_to_read[index].indexRange.length > 0u)
        {
            response->results[index].statusCode = UA_STATUSCODE_BADINDEXRANGENODATA;
            continue;
        }

        if (!model_find_history_channel_id(&nodes_to_read[index].nodeId,
                                           history_channel_id,
                                           sizeof(history_channel_id)))
        {
            response->results[index].statusCode = UA_STATUSCODE_BADHISTORYOPERATIONUNSUPPORTED;
            continue;
        }

        model_history_expire_continuations(now);

        if (nodes_to_read[index].continuationPoint.length > 0u)
        {
            if (!model_history_decode_continuation_point(&nodes_to_read[index].continuationPoint,
                                                         &token))
            {
                response->results[index].statusCode = UA_STATUSCODE_BADCONTINUATIONPOINTINVALID;
                continue;
            }
            continuation = model_history_find_continuation(token, session_context,
                                                            &nodes_to_read[index].nodeId,
                                                            history_channel_id, start_epoch_s,
                                                            end_epoch_s, descending);
            if (continuation == NULL)
            {
                response->results[index].statusCode = UA_STATUSCODE_BADCONTINUATIONPOINTINVALID;
                continue;
            }
        }

        if (release_continuation_points)
        {
            if (continuation == NULL)
            {
                response->results[index].statusCode = UA_STATUSCODE_BADCONTINUATIONPOINTINVALID;
                continue;
            }
            memset(continuation, 0, sizeof(*continuation));
            response->results[index].statusCode = UA_STATUSCODE_GOOD;
            continue;
        }

        if (continuation != NULL)
        {
            requested_generation = continuation->generation;
            skip_matches = continuation->skip_matches;
        }

        if (history_service_get_channel_restore_status(history_channel_id) !=
            HISTORY_SERVICE_RESTORE_STATUS_READY)
        {
            /* Persisted history was not restored completely.  Exposing only
             * a volatile tail would silently lose history, so fail closed. */
            response->results[index].statusCode = UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
            continue;
        }

        sample_count = history_service_read_channel_samples(history_channel_id,
                                                            start_epoch_s,
                                                            end_epoch_s,
                                                            descending,
                                                            max_values_per_node,
                                                            (size_t)skip_matches,
                                                            sample_buffer,
                                                            HISTORY_SERVICE_MAX_RETURN_COUNT,
                                                            &observed_generation,
                                                            &history_more,
                                                            &history_busy);

        if (history_busy)
        {
            response->results[index].statusCode = UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
            continue;
        }

        if (continuation != NULL && observed_generation != requested_generation)
        {
            /* The channel's retained window changed (a wraparound evicted
             * samples) since this continuation point was issued: the skip
             * cursor no longer means the same thing. */
            memset(continuation, 0, sizeof(*continuation));
            response->results[index].statusCode = UA_STATUSCODE_BADCONTINUATIONPOINTINVALID;
            continue;
        }

        if (history_more && sample_count == 0u)
        {
            response->results[index].statusCode = UA_STATUSCODE_BADINTERNALERROR;
            continue;
        }

        if (history_more)
        {
            const uint32_t next_skip_matches = skip_matches + (uint32_t)sample_count;

            if (next_skip_matches < skip_matches)
            {
                response->results[index].statusCode = UA_STATUSCODE_BADOUTOFRANGE;
                continue;
            }
            if (continuation == NULL)
            {
                continuation = model_history_reserve_continuation(session_context,
                                                                   &nodes_to_read[index].nodeId,
                                                                   history_channel_id,
                                                                   observed_generation,
                                                                   next_skip_matches,
                                                                   start_epoch_s,
                                                                   end_epoch_s,
                                                                   descending,
                                                                   now);
                if (continuation == NULL)
                {
                    response->results[index].statusCode = UA_STATUSCODE_BADNOCONTINUATIONPOINTS;
                    continue;
                }
                reserved_continuation = true;
            }
            if (!model_history_encode_continuation_point(continuation->token,
                                                         &response->results[index].continuationPoint))
            {
                if (reserved_continuation)
                {
                    memset(continuation, 0, sizeof(*continuation));
                }
                response->results[index].statusCode = UA_STATUSCODE_BADOUTOFMEMORY;
                continue;
            }
        }

        if (sample_count > 0u)
        {
            result_history_data->dataValues =
                (UA_DataValue *)UA_Array_new(sample_count, &UA_TYPES[UA_TYPES_DATAVALUE]);
            if (result_history_data->dataValues == NULL)
            {
                UA_ByteString_clear(&response->results[index].continuationPoint);
                if (reserved_continuation)
                {
                    memset(continuation, 0, sizeof(*continuation));
                }
                response->results[index].statusCode = UA_STATUSCODE_BADOUTOFMEMORY;
                result_history_data->dataValuesSize = 0u;
                continue;
            }

            result_history_data->dataValuesSize = sample_count;
            for (size_t sample_index = 0u; sample_index < sample_count; ++sample_index)
            {
                model_fill_history_data_value(&sample_buffer[sample_index],
                                              timestamps_to_return,
                                              &result_history_data->dataValues[sample_index]);
            }
        }

        if (history_more)
        {
            TickType_t lifetime = pdMS_TO_TICKS(MODEL_HISTORY_CONTINUATION_EXPIRY_MS);
            if (lifetime == 0u)
            {
                lifetime = 1u;
            }
            continuation->generation = observed_generation;
            continuation->skip_matches = skip_matches + (uint32_t)sample_count;
            continuation->expires_at = now + lifetime;
        }
        else if (continuation != NULL)
        {
            /* A final page consumes the point even if it contains no values. */
            memset(continuation, 0, sizeof(*continuation));
        }

        response->results[index].statusCode = UA_STATUSCODE_GOOD;
    }
}
#endif

UA_StatusCode model_read_value(UA_Server *server,
                                      const UA_NodeId *session_id,
                                      void *session_context,
                                      const UA_NodeId *node_id,
                                      void *node_context,
                                      UA_Boolean include_source_timestamp,
                                      const UA_NumericRange *range,
                                      UA_DataValue *value)
{
    model_node_context_t *context = (model_node_context_t *)node_context;
    const device_profile_config_t *config = config_service_get();
    (void)server;
    (void)session_id;
    (void)session_context;
    (void)node_id;

    if (context == NULL)
    {
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADINTERNALERROR;
        return UA_STATUSCODE_GOOD;
    }

    if (range != NULL)
    {
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADINDEXRANGEINVALID;
        return UA_STATUSCODE_GOOD;
    }

    if (model_session_is_anonymous(session_context) && model_node_is_anonymous_hidden(node_id))
    {
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADUSERACCESSDENIED;
        return UA_STATUSCODE_GOOD;
    }

    switch (context->kind)
    {
    case MODEL_NODE_IDENT_MANUFACTURER:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_localized_text_value(value, config->manufacturer, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_MODEL:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_localized_text_value(value, config->model, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_SERIAL_NUMBER:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->serial_number, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_HW_REVISION:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->hw_revision, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_FW_REVISION:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->fw_revision, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_BUILD_TIME:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->build_time, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_GIT_HASH:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->git_hash, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_DEVICE_NAME:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->device_name, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_PRODUCT_INSTANCE_URI:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->product_instance_uri, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_ASSET_ID:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->asset_id, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_REVISION_COUNTER:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_int32_value(value,
                                     (UA_Int32)config_service_get_running_revision(),
                                     UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_MAC_ADDRESS:
    {
        char mac_address[18] = {0};
        model_format_mac_string(config->mac, mac_address, sizeof(mac_address));
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, mac_address, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_IDENT_MANUFACTURE_DATE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_datetime_value(value, (UA_DateTime)config->manufacture_date_utc, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_CALIBRATION_DATE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_datetime_value(value, (UA_DateTime)config->calibration_date_utc, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_LOCATION:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->location, UA_STATUSCODE_GOOD);
    case MODEL_NODE_IDENT_ASSET_CLASS:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, config->asset_class, UA_STATUSCODE_GOOD);
    case MODEL_NODE_DEVICE_HEALTH:
    {
        device_health_report_t report = {0};

        device_health_service_get_report(&report);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_int32_value(value, (UA_Int32)report.state, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_CHANNEL_VALUE:
    case MODEL_NODE_PROCESS_TEMPERATURE_VALUE:
    {
        telemetry_channel_snapshot_t snapshot = {0};
        const telemetry_channel_t *channel = &snapshot.channel;

        if (!model_copy_channel_from_context(context, &snapshot))
        {
            value->hasStatus = true;
            value->status = UA_STATUSCODE_BADNODEIDUNKNOWN;
            return UA_STATUSCODE_GOOD;
        }

        model_apply_channel_source_timestamp(include_source_timestamp,
                                             channel->runtime.source_timestamp_unix_us,
                                             value);
        return model_set_channel_runtime_value(value, channel, model_quality_to_status(channel->runtime.quality));
    }
    case MODEL_NODE_PROCESS_TEMPERATURE_STATUS:
    {
        telemetry_channel_snapshot_t snapshot = {0};
        const telemetry_channel_t *channel = &snapshot.channel;

        if (!model_copy_channel_from_context(context, &snapshot))
        {
            value->hasStatus = true;
            value->status = UA_STATUSCODE_BADNODEIDUNKNOWN;
            return UA_STATUSCODE_GOOD;
        }

        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value,
                                      model_quality_to_string(channel->runtime.quality),
                                      model_quality_to_status(channel->runtime.quality));
    }
    case MODEL_NODE_PROCESS_TEMPERATURE_SOURCE_TIMESTAMP:
    {
        telemetry_channel_snapshot_t snapshot = {0};
        const telemetry_channel_t *channel = &snapshot.channel;
        UA_DateTime timestamp = 0;

        if (!model_copy_channel_from_context(context, &snapshot))
        {
            value->hasStatus = true;
            value->status = UA_STATUSCODE_BADNODEIDUNKNOWN;
            return UA_STATUSCODE_GOOD;
        }

        (void)model_source_timestamp_to_datetime(channel->runtime.source_timestamp_unix_us, &timestamp);
        model_apply_channel_source_timestamp(include_source_timestamp,
                                             channel->runtime.source_timestamp_unix_us,
                                             value);
        return model_set_datetime_value(value, timestamp, model_quality_to_status(channel->runtime.quality));
    }
    case MODEL_NODE_PROCESS_TEMPERATURE_UNITS:
    {
        telemetry_channel_snapshot_t snapshot = {0};
        const telemetry_channel_t *channel = &snapshot.channel;

        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_from_context(context, &snapshot) &&
            channel->metadata.unit != NULL &&
            channel->metadata.unit[0] != '\0')
        {
            return model_set_string_value(value, channel->metadata.unit, UA_STATUSCODE_GOOD);
        }
        return model_set_string_value(value, "", UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_CHANNEL_SAMPLE_PERIOD_MS:
    case MODEL_NODE_PROCESS_TEMPERATURE_UPDATE_PERIOD_MS:
    {
        telemetry_channel_snapshot_t snapshot = {0};
        const telemetry_channel_t *channel = &snapshot.channel;

        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_from_context(context, &snapshot) && channel->settings.sample_period_ms > 0u)
        {
            return model_set_uint32_value(value, channel->settings.sample_period_ms, UA_STATUSCODE_GOOD);
        }
        return model_set_uint32_value(value, 0u, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_CHANNEL_PUBLISH_PERIOD_MS:
    {
        telemetry_channel_snapshot_t snapshot = {0};
        const telemetry_channel_t *channel = &snapshot.channel;

        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_from_context(context, &snapshot))
        {
            return model_set_uint32_value(value, channel->settings.publish_period_ms, UA_STATUSCODE_GOOD);
        }
        return model_set_uint32_value(value, 0u, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_PROCESS_TEMPERATURE_HISTORY_INTERVAL_MS:
    {
        telemetry_channel_snapshot_t snapshot = {0};
        const telemetry_channel_t *channel = &snapshot.channel;

        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_from_context(context, &snapshot))
        {
            return model_set_uint32_value(value, channel->settings.history_interval_ms, UA_STATUSCODE_GOOD);
        }
        return model_set_uint32_value(value, 0u, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_PROCESS_TEMPERATURE_ALARM_STATE:
    {
        alarm_condition_status_t condition = {0};
        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (context != NULL && context->channel_id != NULL &&
            alarm_service_get_condition_by_channel_id(context->channel_id, &condition))
        {
            /* Lossy legacy 2-state projection of the v0.4.3 per-channel
             * condition: LOW/LOW_LOW have no legacy equivalent and are
             * reported as WARNING/CRITICAL, same as HIGH/HIGH_HIGH, matching
             * the alarm_service compatibility shim's own projection. A
             * quality fault takes precedence over process/discrete state, so
             * a channel that was WARNING/HIGH and then goes stale still
             * reports "Invalid" here even though the service keeps the
             * sticky process state visible via alarm_service_get_condition_by_channel_id
             * to any v0.4.3-aware caller. */
            telemetry_alarm_state_t legacy_state = TELEMETRY_ALARM_STATE_NORMAL;
            if (condition.quality_fault != ALARM_QUALITY_FAULT_NONE)
            {
                legacy_state = TELEMETRY_ALARM_STATE_INVALID;
            }
            else if (condition.is_discrete)
            {
                legacy_state = condition.discrete_active ? TELEMETRY_ALARM_STATE_WARNING : TELEMETRY_ALARM_STATE_NORMAL;
            }
            else
            {
                switch (condition.process_state)
                {
                case ALARM_CONDITION_STATE_HIGH_HIGH:
                case ALARM_CONDITION_STATE_LOW_LOW:
                    legacy_state = TELEMETRY_ALARM_STATE_CRITICAL;
                    break;
                case ALARM_CONDITION_STATE_HIGH:
                case ALARM_CONDITION_STATE_LOW:
                    legacy_state = TELEMETRY_ALARM_STATE_WARNING;
                    break;
                default:
                    legacy_state = TELEMETRY_ALARM_STATE_NORMAL;
                    break;
                }
            }
            return model_set_string_value(value,
                                          model_alarm_state_to_string(legacy_state),
                                          model_quality_to_status(condition.quality));
        }
        return model_set_string_value(value, "Invalid", UA_STATUSCODE_BADNOTSUPPORTED);
    }
    case MODEL_NODE_NETWORK_IPV4:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.ipv4_address, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_NETWORK_MAC_ADDRESS:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.mac_address, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_NETWORK_IP_MODE:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.ip_mode, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_NETWORK_MASK:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.netmask, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_NETWORK_GATEWAY:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.gateway, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_NETWORK_LINK_UP:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_boolean_value(value, status.link_up, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_NETWORK_DHCP_STATE:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.dhcp_state, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_NETWORK_RECONNECT_COUNT:
    {
        network_manager_status_t status = {0};
        network_manager_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, status.reconnect_count, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_TIME_LAST_SYNC_EPOCH:
    {
        time_service_status_t status = {0};
        time_service_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, status.last_sync_epoch_s, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_TIME_SYNCED:
    {
        time_service_status_t status = {0};
        time_service_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_boolean_value(value, status.synchronized, model_quality_to_status(status.quality));
    }
    case MODEL_NODE_TIME_NTP_SERVER:
    {
        time_service_status_t status = {0};
        time_service_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, status.ntp_server, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_TIME_CLOCK_QUALITY:
    {
        time_service_status_t status = {0};
        time_service_get_status(&status);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value,
                                      model_quality_to_string(status.quality),
                                      model_quality_to_status(status.quality));
    }
    case MODEL_NODE_TIME_TIMEZONE_OFFSET_MINUTES:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_int16_value(value, (UA_Int16)config->timezone_offset_minutes, UA_STATUSCODE_GOOD);
    case MODEL_NODE_SECURITY_MODE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, model_security_mode_to_string(), UA_STATUSCODE_GOOD);
    case MODEL_NODE_SECURITY_COMMISSIONING_MODE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_boolean_value(value,
                                       security_service_is_commissioning_mode(),
                                       UA_STATUSCODE_GOOD);
    case MODEL_NODE_SECURITY_TRUST_LIST_VERSION:
    {
        uint32_t trust_list_version = 0u;
        (void)security_service_get_trust_list_version(&trust_list_version);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, trust_list_version, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_SECURITY_TRUSTED_CLIENT_COUNT:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value,
                                      security_service_get_trusted_client_count(),
                                      UA_STATUSCODE_GOOD);
    case MODEL_NODE_SECURITY_LAST_AUTH_FAILURE:
    {
        uint32_t last_auth_failure_epoch_s = 0u;
        UA_DateTime timestamp = 0;
        if (security_service_get_last_auth_failure(&last_auth_failure_epoch_s))
        {
            timestamp = UA_DateTime_fromUnixTime((UA_Int64)last_auth_failure_epoch_s);
        }
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_datetime_value(value, timestamp, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_SECURITY_FAILED_LOGIN_COUNT:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value,
                                      security_service_get_failed_login_count(),
                                      UA_STATUSCODE_GOOD);
    case MODEL_NODE_SECURITY_CERT_THUMBPRINT:
    {
        char thumbprint[SECURITY_SERVICE_CERT_THUMBPRINT_LEN + 1u];
        const char *value_text = "";

        if (security_service_get_device_certificate_thumbprint(thumbprint, sizeof(thumbprint)))
        {
            value_text = thumbprint;
        }

        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, value_text, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_SECURITY_CERT_NOT_BEFORE:
    {
        uint32_t not_before_epoch_s = 0u;
        UA_DateTime timestamp = 0;
        if (security_service_get_device_certificate_not_before(&not_before_epoch_s))
        {
            timestamp = UA_DateTime_fromUnixTime((UA_Int64)not_before_epoch_s);
        }
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_datetime_value(value, timestamp, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_SECURITY_CERT_NOT_AFTER:
    {
        uint32_t not_after_epoch_s = 0u;
        UA_DateTime timestamp = 0;
        if (security_service_get_device_certificate_not_after(&not_after_epoch_s))
        {
            timestamp = UA_DateTime_fromUnixTime((UA_Int64)not_after_epoch_s);
        }
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_datetime_value(value, timestamp, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_SECURITY_DAYS_UNTIL_EXPIRY:
    {
        uint32_t days_until_expiry = 0u;

        (void)model_get_certificate_expiry_state(&days_until_expiry, NULL);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, days_until_expiry, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_SECURITY_EXPIRY_STATE:
    {
        const model_certificate_expiry_state_t expiry_state =
            model_get_certificate_expiry_state(NULL, NULL);

        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value,
                                      model_certificate_expiry_state_to_string(expiry_state),
                                      UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_SECURITY_COMMISSIONING_STATE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value,
                                      model_security_commissioning_state_to_string(),
                                      UA_STATUSCODE_GOOD);
    case MODEL_NODE_SECURITY_ACTIVE_IDENTITY_GENERATION:
    {
        security_state_service_snapshot_t snapshot = {0};

        (void)security_state_service_get_snapshot(&snapshot);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, snapshot.active_identity_generation, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_SECURITY_ACTIVE_TRUST_GENERATION:
    {
        security_state_service_snapshot_t snapshot = {0};

        (void)security_state_service_get_snapshot(&snapshot);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, snapshot.active_trust_generation, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_SECURITY_CURRENT_USER_ROLE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value,
                                      model_session_role_to_string(session_context),
                                      UA_STATUSCODE_GOOD);
    case MODEL_NODE_SECURITY_LAST_EVENT_ID:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, g_last_event_state.last_event_id, UA_STATUSCODE_GOOD);
    case MODEL_NODE_NETWORK_LAST_NETWORK_CHANGE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, g_last_event_state.last_network_change, UA_STATUSCODE_GOOD);
    case MODEL_NODE_DIAG_LAST_FAULT:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, g_last_event_state.last_fault, UA_STATUSCODE_GOOD);
    case MODEL_NODE_CONFIG_LAST_CONFIG_CHANGE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, g_last_event_state.last_config_change, UA_STATUSCODE_GOOD);
    case MODEL_NODE_CONFIG_REBOOT_REQUIRED:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_boolean_value(value, model_is_reboot_required() ? UA_TRUE : UA_FALSE, UA_STATUSCODE_GOOD);
    case MODEL_NODE_CONFIG_COMMISSIONED:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_boolean_value(value, model_is_commissioned() ? UA_TRUE : UA_FALSE, UA_STATUSCODE_GOOD);
    case MODEL_NODE_DIAG_FREE_HEAP:
    {
        diag_service_heap_stats_t stats = {0};
        diag_service_get_heap_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, stats.available_heap_bytes, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_MIN_EVER_FREE_HEAP:
    {
        diag_service_heap_stats_t stats = {0};
        diag_service_get_heap_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, stats.minimum_ever_free_bytes, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_PSRAM_READY:
    {
        ua_port_alloc_stats_t stats = {0};

        ua_port_alloc_get_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_boolean_value(value,
                                       stats.psram_ready ? UA_TRUE : UA_FALSE,
                                       UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_PSRAM_TOTAL_BYTES:
    {
        ua_port_alloc_stats_t stats = {0};

        ua_port_alloc_get_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value,
                                      (UA_UInt32)stats.total_bytes,
                                      UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_PSRAM_USED_BYTES:
    {
        ua_port_alloc_stats_t stats = {0};

        ua_port_alloc_get_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value,
                                      (UA_UInt32)(stats.total_bytes >= stats.free_bytes
                                                      ? stats.total_bytes - stats.free_bytes
                                                      : 0u),
                                      UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_PSRAM_FREE_BYTES:
    {
        ua_port_alloc_stats_t stats = {0};

        ua_port_alloc_get_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value,
                                      (UA_UInt32)stats.free_bytes,
                                      UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_PSRAM_LARGEST_FREE_BLOCK_BYTES:
    {
        ua_port_alloc_stats_t stats = {0};

        ua_port_alloc_get_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value,
                                      (UA_UInt32)stats.largest_free_block_bytes,
                                      UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_PSRAM_ALLOCATOR_MODE:
    {
        ua_port_alloc_stats_t stats = {0};
        const char *mode = "sram-fallback";

        ua_port_alloc_get_stats(&stats);
        if (stats.psram_ready)
        {
            mode = stats.psram_enabled ? "psram-first" : "sram-runtime";
        }
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, mode, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_UPTIME_SEC:
    {
        diag_service_system_stats_t stats = {0};
        diag_service_get_system_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, stats.uptime_sec, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_RESET_CAUSE:
    {
        diag_service_system_stats_t stats = {0};
        diag_service_get_system_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, stats.reset_cause, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_CPU_LOAD_APPROX:
    {
        diag_service_system_stats_t stats = {0};
        diag_service_get_system_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, stats.cpu_like_load_approx, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_LAST_FAULT_CODE:
    {
        diag_service_system_stats_t stats = {0};
        diag_service_get_system_stats(&stats);
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, stats.last_fault_code, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_DIAG_TASK_STACK_HIGH_WATER:
    {
        diag_service_task_info_t task_info = {0};
        if (context->task_name == NULL || !diag_service_get_task_info_by_name(context->task_name, &task_info))
        {
            value->hasStatus = true;
            value->status = UA_STATUSCODE_BADNOTFOUND;
            return UA_STATUSCODE_GOOD;
        }

        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, task_info.stack_high_water_mark, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_CONFIG_SAMPLING_PERIOD_MS:
    {
        telemetry_channel_snapshot_t snapshot = {0};
        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_by_id(DEVICE_CHANNEL_ID_TEMPERATURE_1, &snapshot))
        {
            return model_set_uint32_value(value, snapshot.channel.settings.sample_period_ms, UA_STATUSCODE_GOOD);
        }
        return model_set_uint32_value(value, 0u, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_CONFIG_PUBLISH_PERIOD_MS:
    {
        telemetry_channel_snapshot_t snapshot = {0};
        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_by_id(DEVICE_CHANNEL_ID_TEMPERATURE_1, &snapshot))
        {
            return model_set_uint32_value(value, snapshot.channel.settings.publish_period_ms, UA_STATUSCODE_GOOD);
        }
        return model_set_uint32_value(value, 0u, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_CONFIG_TEMP_OFFSET:
    {
        telemetry_channel_snapshot_t snapshot = {0};
        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_by_id(DEVICE_CHANNEL_ID_TEMPERATURE_1, &snapshot))
        {
            return model_set_float_value(value, (UA_Float)snapshot.channel.settings.offset, UA_STATUSCODE_GOOD);
        }
        return model_set_float_value(value, 0.0f, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_CONFIG_TEMP_SCALE:
    {
        telemetry_channel_snapshot_t snapshot = {0};
        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_by_id(DEVICE_CHANNEL_ID_TEMPERATURE_1, &snapshot))
        {
            return model_set_float_value(value, (UA_Float)snapshot.channel.settings.scale, UA_STATUSCODE_GOOD);
        }
        return model_set_float_value(value, 1.0f, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_CONFIG_HIGH_ALARM:
    {
        telemetry_channel_snapshot_t snapshot = {0};
        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_by_id(DEVICE_CHANNEL_ID_TEMPERATURE_1, &snapshot))
        {
            return model_set_float_value(value, (UA_Float)snapshot.channel.settings.warning_threshold, UA_STATUSCODE_GOOD);
        }
        return model_set_float_value(value, 0.0f, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_CONFIG_HIGH_HIGH_ALARM:
    {
        telemetry_channel_snapshot_t snapshot = {0};
        model_apply_now_source_timestamp(include_source_timestamp, value);
        if (model_copy_channel_by_id(DEVICE_CHANNEL_ID_TEMPERATURE_1, &snapshot))
        {
            return model_set_float_value(value, (UA_Float)snapshot.channel.settings.critical_threshold, UA_STATUSCODE_GOOD);
        }
        return model_set_float_value(value, 0.0f, UA_STATUSCODE_GOOD);
    }
    case MODEL_NODE_MESSAGING_EVENT_SEQUENCE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, g_model_pubsub_event_mirror.sequence, UA_STATUSCODE_GOOD);
    case MODEL_NODE_MESSAGING_EVENT_ID:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, g_model_pubsub_event_mirror.event_id, UA_STATUSCODE_GOOD);
    case MODEL_NODE_MESSAGING_EVENT_TYPE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, g_model_pubsub_event_mirror.event_type, UA_STATUSCODE_GOOD);
    case MODEL_NODE_MESSAGING_EVENT_SOURCE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, g_model_pubsub_event_mirror.source, UA_STATUSCODE_GOOD);
    case MODEL_NODE_MESSAGING_EVENT_MESSAGE:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_string_value(value, g_model_pubsub_event_mirror.message, UA_STATUSCODE_GOOD);
    case MODEL_NODE_MESSAGING_EVENT_SEVERITY:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_uint32_value(value, g_model_pubsub_event_mirror.severity, UA_STATUSCODE_GOOD);
    case MODEL_NODE_MESSAGING_EVENT_TIME:
        model_apply_now_source_timestamp(include_source_timestamp, value);
        return model_set_datetime_value(value, g_model_pubsub_event_mirror.time, UA_STATUSCODE_GOOD);
    default:
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADNOTSUPPORTED;
        return UA_STATUSCODE_GOOD;
    }
}
