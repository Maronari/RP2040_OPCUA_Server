/**
 * @file
 * @brief AMB (Asset Management Basic) companion-spec location hierarchy,
 *        DI (Device Integration) functional-group/identification-property
 *        node builders, and the AMB `FindAlias`/asset-alias glob matcher.
 *
 * Responsibilities:
 * - Builds and rebuilds the AMB `Locations.Hierarchical`/`Locations.Operational`
 *   subtrees from `device_profile_config_t` location paths, including their
 *   `NodeVersion` change-notification properties.
 * - Thin DI node builders (`FunctionalGroupType` folders, identification
 *   property variables) shared by `model_address_space.c`.
 * - A self-contained SQL-LIKE-style glob matcher (`%`, `_`, `[...]` classes)
 *   used only to resolve `FindAlias` method calls against configured asset
 *   aliases; no external glob library is used.
 *
 * Ownership:
 * No persistent state owned here. Location/alias node identities are
 * transient open62541 server-side nodes rebuilt from `config_service`-owned
 * `device_profile_config_t` data; see `opcua_model_internal.h` for the full
 * per-function contract.
 *
 * Concurrency:
 * All functions run on `OpcUaTask`: the location/DI builders during
 * `model_build_address_space` (startup) or from the `SetLocationPaths`
 * method callback (`model_refresh_amb_locations`), the alias matcher from
 * the `FindAlias` method callback (`model_find_asset_alias`).
 */
#include "opcua_model_internal.h"

UA_StatusCode model_add_location_node_version(UA_Server *server,
                                                     const char *node_id,
                                                     UA_NodeId parent_id)
{
    UA_VariableAttributes attributes = UA_VariableAttributes_default;
    UA_String initial_value = UA_STRING((char *)"0");
    UA_StatusCode status;

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)"NodeVersion");
    attributes.description = UA_LOCALIZEDTEXT((char *)"en-US",
                                               (char *)"Version of the location hierarchy");
    attributes.dataType = UA_TYPES[UA_TYPES_STRING].typeId;
    attributes.valueRank = UA_VALUERANK_SCALAR;
    attributes.accessLevel = UA_ACCESSLEVELMASK_READ;
    attributes.userAccessLevel = UA_ACCESSLEVELMASK_READ;
    status = UA_Variant_setScalarCopy(&attributes.value,
                                      &initial_value,
                                      &UA_TYPES[UA_TYPES_STRING]);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = UA_Server_addVariableNode(
        server,
        UA_NODEID_STRING(1, (char *)node_id),
        parent_id,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
        UA_QUALIFIEDNAME(0, (char *)"NodeVersion"),
        UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE),
        attributes,
        NULL,
        NULL);
    UA_Variant_clear(&attributes.value);
    return status;
}

UA_StatusCode model_write_location_node_version(UA_Server *server,
                                                       const char *node_id)
{
    char revision_text[16];
    UA_String revision;
    UA_Variant value;

    snprintf(revision_text,
             sizeof(revision_text),
             "%lu",
             (unsigned long)config_service_get_running_revision());
    revision = UA_STRING(revision_text);
    UA_Variant_init(&value);
    UA_Variant_setScalar(&value, &revision, &UA_TYPES[UA_TYPES_STRING]);
    return UA_Server_writeValue(server,
                                UA_NODEID_STRING(1, (char *)node_id),
                                value);
}

/* Deletes deepest-level-first ("<prefix>.N" down to "<prefix>.0") so no
 * child ever briefly outlives a deleted parent. BADNODEIDUNKNOWN is treated
 * as success (idempotent: a level that was never created, e.g. because the
 * configured path was shallower than CONFIG_SERVICE_LOCATION_MAX_DEPTH, is
 * not an error). */
UA_StatusCode model_delete_location_tree(UA_Server *server,
                                                const char *node_id_prefix)
{
    char node_id[96];

    for (size_t depth = CONFIG_SERVICE_LOCATION_MAX_DEPTH; depth > 0u; --depth)
    {
        UA_StatusCode status;

        if (snprintf(node_id,
                     sizeof(node_id),
                     "%s.%u",
                     node_id_prefix,
                     (unsigned)(depth - 1u)) >= (int)sizeof(node_id))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }
        status = UA_Server_deleteNode(server,
                                      UA_NODEID_STRING(1, node_id),
                                      true);
        if (status != UA_STATUSCODE_GOOD && status != UA_STATUSCODE_BADNODEIDUNKNOWN)
        {
            return status;
        }
    }

    return UA_STATUSCODE_GOOD;
}

/* Tokenizes a copy of path (strtok_r; the original string is left intact)
 * on '/' and creates one Object node per segment, chaining each new node
 * as the parent of the next. Bounded by CONFIG_SERVICE_LOCATION_MAX_DEPTH
 * and by the fixed-size node_id/parent_node_id stack buffers (no heap
 * allocation). On a mid-loop failure this returns immediately without
 * deleting the nodes already created for this path — callers that need a
 * clean rebuild (model_refresh_amb_locations) call
 * model_delete_location_tree first/rely on being retried, there is no
 * automatic rollback here. */
UA_StatusCode model_build_location_path(UA_Server *server,
                                               const char *path,
                                               const char *node_id_prefix,
                                               UA_UInt32 entry_point_id,
                                               UA_UInt32 contains_reference_id,
                                               UA_NodeId device_id)
{
    char path_buffer[CONFIG_SERVICE_LOCATION_PATH_MAX_LEN];
    char node_id[96];
    char parent_node_id[96] = {0};
    char *save_pointer = NULL;
    char *segment = NULL;
    UA_NodeId contains_id = UA_NODEID_NUMERIC(g_model_amb_namespace_index,
                                              contains_reference_id);
    size_t depth = 0u;
    UA_StatusCode status = UA_STATUSCODE_GOOD;

    if (path == NULL || path[0] == '\0')
    {
        return UA_STATUSCODE_GOOD;
    }

    strncpy(path_buffer, path, sizeof(path_buffer));
    path_buffer[sizeof(path_buffer) - 1u] = '\0';
    segment = strtok_r(path_buffer, "/", &save_pointer);
    while (segment != NULL)
    {
        UA_ObjectAttributes attributes = UA_ObjectAttributes_default;
        UA_NodeId parent_id = depth == 0u
                                  ? UA_NODEID_NUMERIC(g_model_amb_namespace_index,
                                                      entry_point_id)
                                  : UA_NODEID_STRING(1, parent_node_id);
        UA_NodeId reference_type = depth == 0u
                                       ? UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES)
                                       : contains_id;

        if (depth >= CONFIG_SERVICE_LOCATION_MAX_DEPTH ||
            snprintf(node_id,
                     sizeof(node_id),
                     "%s.%u",
                     node_id_prefix,
                     (unsigned)depth) >= (int)sizeof(node_id))
        {
            return UA_STATUSCODE_BADOUTOFRANGE;
        }

        attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", segment);
        attributes.description = UA_LOCALIZEDTEXT((char *)"en-US",
                                                   (char *)"Configured asset location");
        status = UA_Server_addObjectNode(
            server,
            UA_NODEID_STRING(1, node_id),
            parent_id,
            reference_type,
            UA_QUALIFIEDNAME(1, segment),
            UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE),
            attributes,
            NULL,
            NULL);
        if (status != UA_STATUSCODE_GOOD)
        {
            return status;
        }

        strncpy(parent_node_id, node_id, sizeof(parent_node_id));
        parent_node_id[sizeof(parent_node_id) - 1u] = '\0';
        depth++;
        segment = strtok_r(NULL, "/", &save_pointer);
    }

    if (depth == 0u)
    {
        return UA_STATUSCODE_GOOD;
    }

    return UA_Server_addReference(server,
                                  UA_NODEID_STRING(1, parent_node_id),
                                  contains_id,
                                  UA_NODEID2EXPANDEDNODEID(device_id),
                                  true);
}

UA_StatusCode model_refresh_amb_locations(UA_Server *server,
                                                 const device_profile_config_t *config)
{
    UA_StatusCode status;

    if (server == NULL || config == NULL || g_model_amb_namespace_index == 0u)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    status = model_delete_location_tree(server, "Locations.Hierarchical");
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_delete_location_tree(server, "Locations.Operational");
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_build_location_path(
            server,
            config->hierarchical_location_path,
            "Locations.Hierarchical",
            UA_AMBID_HIERARCHICAL_LOCATIONS,
            UA_AMBID_HIERARCHICAL_CONTAINS,
            UA_NODEID_STRING(1, (char *)"Device"));
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_build_location_path(
            server,
            config->operational_location_path,
            "Locations.Operational",
            UA_AMBID_OPERATIONAL_LOCATIONS,
            UA_AMBID_OPERATIONAL_CONTAINS,
            UA_NODEID_STRING(1, (char *)"Device"));
    }
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    status = model_write_location_node_version(server,
                                               "Locations.Hierarchical.NodeVersion");
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_write_location_node_version(server,
                                                   "Locations.Operational.NodeVersion");
    }
    return status;
}

UA_StatusCode model_build_amb_locations(UA_Server *server,
                                               const device_profile_config_t *config)
{
    UA_StatusCode status;

    status = model_add_location_node_version(
        server,
        "Locations.Hierarchical.NodeVersion",
        UA_NODEID_NUMERIC(g_model_amb_namespace_index,
                          UA_AMBID_HIERARCHICAL_LOCATIONS));
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_add_location_node_version(
            server,
            "Locations.Operational.NodeVersion",
            UA_NODEID_NUMERIC(g_model_amb_namespace_index,
                              UA_AMBID_OPERATIONAL_LOCATIONS));
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        status = model_refresh_amb_locations(server, config);
    }
    if (status == UA_STATUSCODE_GOOD)
    {
        printf("[OPC UA]\tAMB Locations ready, hierarchical='%s' operational='%s'\n",
               config->hierarchical_location_path,
               config->operational_location_path);
        fflush(stdout);
    }
    return status;
}

UA_StatusCode model_add_di_functional_group(UA_Server *server,
                                                   const char *node_id,
                                                   const char *browse_name,
                                                   UA_NodeId parent_node_id)
{
    UA_ObjectAttributes attributes = UA_ObjectAttributes_default;
    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browse_name);

    return UA_Server_addObjectNode(
        server,
        UA_NODEID_STRING(1, (char *)node_id),
        parent_node_id,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
        UA_QUALIFIEDNAME(g_model_di_namespace_index, (char *)browse_name),
        UA_NODEID_NUMERIC(g_model_di_namespace_index, UA_DIID_FUNCTIONALGROUPTYPE),
        attributes,
        NULL,
        NULL);
}

UA_StatusCode model_add_di_identification_property(
    UA_Server *server,
    const char *node_id,
    const char *browse_name,
    UA_NodeId parent_node_id,
    UA_NodeId data_type,
    model_node_context_t *context,
    UA_Boolean writable)
{
    UA_VariableAttributes attributes = UA_VariableAttributes_default;
    UA_DataSource data_source = {0};
    UA_NodeId created_node_id = UA_NODEID_NULL;
    const UA_Byte access_level = writable
                                     ? (UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE)
                                     : UA_ACCESSLEVELMASK_READ;
    UA_StatusCode status;

    attributes.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browse_name);
    attributes.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browse_name);
    attributes.dataType = data_type;
    attributes.valueRank = UA_VALUERANK_SCALAR;
    attributes.accessLevel = access_level;
    attributes.userAccessLevel = access_level;
    data_source.read = model_read_value;
    data_source.write = writable ? model_write_value : NULL;

    status = UA_Server_addVariableNode(
        server,
        UA_NODEID_STRING(1, (char *)node_id),
        parent_node_id,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
        UA_QUALIFIEDNAME(g_model_di_namespace_index, (char *)browse_name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE),
        attributes,
        context,
        &created_node_id);
    if (status != UA_STATUSCODE_GOOD)
    {
        return status;
    }

    return UA_Server_setVariableNode_dataSource(server, created_node_id, data_source);
}

bool model_alias_match_class(const UA_Byte *pattern,
                                    size_t pattern_length,
                                    UA_Byte value,
                                    size_t *consumed,
                                    bool *valid)
{
    size_t index = 0u;
    bool negate = false;
    bool matched = false;
    bool have_entry = false;

    if (pattern == NULL || consumed == NULL || valid == NULL)
    {
        return false;
    }
    if (index < pattern_length && pattern[index] == '^')
    {
        negate = true;
        index++;
    }

    while (index < pattern_length && pattern[index] != ']')
    {
        UA_Byte range_start;
        UA_Byte range_end;

        if (pattern[index] == '\\')
        {
            index++;
            if (index >= pattern_length)
            {
                *valid = false;
                return false;
            }
        }
        range_start = pattern[index++];
        range_end = range_start;
        have_entry = true;

        if (index + 1u < pattern_length && pattern[index] == '-' &&
            pattern[index + 1u] != ']')
        {
            index++;
            if (pattern[index] == '\\')
            {
                index++;
                if (index >= pattern_length)
                {
                    *valid = false;
                    return false;
                }
            }
            range_end = pattern[index++];
            if (range_start > range_end)
            {
                *valid = false;
                return false;
            }
        }

        if (value >= range_start && value <= range_end)
        {
            matched = true;
        }
    }

    if (index >= pattern_length || pattern[index] != ']' || !have_entry)
    {
        *valid = false;
        return false;
    }

    *consumed = index + 1u;
    return negate ? !matched : matched;
}

bool model_alias_pattern_matches(const UA_String *pattern,
                                        const char *value,
                                        bool *valid)
{
    bool current[CONFIG_SERVICE_ASSET_ID_MAX_LEN] = {false};
    bool next[CONFIG_SERVICE_ASSET_ID_MAX_LEN] = {false};
    const UA_Byte *value_bytes = (const UA_Byte *)value;
    const UA_Byte *class_pattern = NULL;
    size_t class_pattern_length = 0u;
    size_t pattern_index = 0u;
    const size_t value_length = value != NULL ? strlen(value) : 0u;

    if (pattern == NULL || value == NULL || valid == NULL)
    {
        return false;
    }
    if (pattern->length > 256u || value_length >= CONFIG_SERVICE_ASSET_ID_MAX_LEN)
    {
        *valid = false;
        return false;
    }

    *valid = true;
    current[0] = true;
    while (pattern_index < pattern->length)
    {
        UA_Byte token = pattern->data[pattern_index++];
        bool match_any_string = false;
        bool match_any_character = false;
        UA_Byte literal = 0u;

        class_pattern = NULL;
        class_pattern_length = 0u;
        if (token == '%')
        {
            match_any_string = true;
        }
        else if (token == '_')
        {
            match_any_character = true;
        }
        else if (token == '\\')
        {
            if (pattern_index >= pattern->length)
            {
                *valid = false;
                return false;
            }
            literal = pattern->data[pattern_index++];
        }
        else if (token == '[')
        {
            size_t consumed = 0u;
            class_pattern = pattern->data + pattern_index;
            class_pattern_length = pattern->length - pattern_index;
            (void)model_alias_match_class(class_pattern,
                                          class_pattern_length,
                                          0u,
                                          &consumed,
                                          valid);
            if (!*valid)
            {
                return false;
            }
            pattern_index += consumed;
        }
        else
        {
            literal = token;
        }

        memset(next, 0, sizeof(next));
        if (match_any_string)
        {
            next[0] = current[0];
            for (size_t value_index = 1u; value_index <= value_length; ++value_index)
            {
                next[value_index] = current[value_index] || next[value_index - 1u];
            }
        }
        else
        {
            for (size_t value_index = 1u; value_index <= value_length; ++value_index)
            {
                bool token_matches = match_any_character;
                if (class_pattern != NULL)
                {
                    size_t ignored = 0u;
                    token_matches = model_alias_match_class(class_pattern,
                                                            class_pattern_length,
                                                            value_bytes[value_index - 1u],
                                                            &ignored,
                                                            valid);
                }
                else if (!match_any_character)
                {
                    token_matches = literal == value_bytes[value_index - 1u];
                }
                next[value_index] = current[value_index - 1u] && token_matches;
            }
        }
        memcpy(current, next, sizeof(current));
    }

    return current[value_length];
}

bool model_alias_pattern_is_valid(const UA_String *pattern)
{
    size_t index = 0u;

    if (pattern == NULL)
    {
        return false;
    }
    while (index < pattern->length)
    {
        if (pattern->data[index] == '\\')
        {
            index += 2u;
            if (index > pattern->length)
            {
                return false;
            }
            continue;
        }
        if (pattern->data[index] == '[')
        {
            size_t consumed = 0u;
            bool valid = true;
            (void)model_alias_match_class(pattern->data + index + 1u,
                                          pattern->length - index - 1u,
                                          0u,
                                          &consumed,
                                          &valid);
            if (!valid)
            {
                return false;
            }
            index += consumed + 1u;
            continue;
        }
        index++;
    }
    return true;
}

/* matches[]/references[] are fixed at size 2 because at most two alias
 * candidates exist (ProductInstanceUri, AssetId); which ones are considered
 * is selected by object_id (Aliases root vs the three AMB alias-category
 * folders) rather than by the reference_filter input, which is only used to
 * short-circuit with an empty result if the caller asked for a reference
 * type other than AliasFor (UA_NS0ID_ALIASFOR). */
UA_StatusCode model_find_asset_alias(UA_Server *server,
                                            const UA_NodeId *session_id,
                                            void *session_context,
                                            const UA_NodeId *method_id,
                                            void *method_context,
                                            const UA_NodeId *object_id,
                                            void *object_context,
                                            size_t input_size,
                                            const UA_Variant *input,
                                            size_t output_size,
                                            UA_Variant *output)
{
    const device_profile_config_t *config = config_service_get();
    UA_AliasNameDataType matches[2];
    UA_ExpandedNodeId references[2];
    const UA_String *pattern;
    const UA_NodeId *reference_filter;
    const char *asset_id;
    size_t matches_size = 0u;
    bool include_product = false;
    bool include_asset_id = false;
    bool valid_pattern = true;
    UA_NodeId alias_for_id = UA_NODEID_NUMERIC(0, UA_NS0ID_ALIASFOR);
    (void)server;
    (void)session_id;
    (void)session_context;
    (void)method_id;
    (void)method_context;
    (void)object_context;

    if (config == NULL || object_id == NULL || input == NULL || output == NULL ||
        input_size != 2u || output_size != 1u)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }
    if (!UA_Variant_hasScalarType(&input[0], &UA_TYPES[UA_TYPES_STRING]) ||
        !UA_Variant_hasScalarType(&input[1], &UA_TYPES[UA_TYPES_NODEID]))
    {
        return UA_STATUSCODE_BADTYPEMISMATCH;
    }

    pattern = (const UA_String *)input[0].data;
    reference_filter = (const UA_NodeId *)input[1].data;
    if (!model_alias_pattern_is_valid(pattern))
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }
    if (!UA_NodeId_isNull(reference_filter) &&
        !UA_NodeId_equal(reference_filter, &alias_for_id))
    {
        return UA_Variant_setArrayCopy(&output[0], NULL, 0u,
                                       &UA_TYPES[UA_TYPES_ALIASNAMEDATATYPE]);
    }

    if (object_id->identifierType != UA_NODEIDTYPE_NUMERIC)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }
    if (object_id->namespaceIndex == 0u &&
        object_id->identifier.numeric == UA_NS0ID_ALIASES)
    {
        include_product = true;
        include_asset_id = true;
    }
    else if (object_id->namespaceIndex != g_model_amb_namespace_index)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }
    else switch (object_id->identifier.numeric)
    {
    case UA_AMBID_ASSETS:
        include_product = true;
        include_asset_id = true;
        break;
    case UA_AMBID_ASSETS_BY_PRODUCT_INSTANCE_URI:
        include_product = true;
        break;
    case UA_AMBID_ASSETS_BY_ASSET_ID:
        include_asset_id = true;
        break;
    default:
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    memset(matches, 0, sizeof(matches));
    memset(references, 0, sizeof(references));
    if (include_product &&
        model_alias_pattern_matches(pattern, config->product_instance_uri, &valid_pattern))
    {
        references[matches_size] = UA_EXPANDEDNODEID_STRING(1, (char *)"Device");
        matches[matches_size].aliasName =
            UA_QUALIFIEDNAME(g_model_amb_namespace_index,
                             (char *)config->product_instance_uri);
        matches[matches_size].referencedNodesSize = 1u;
        matches[matches_size].referencedNodes = &references[matches_size];
        matches_size++;
    }
    if (!valid_pattern)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    asset_id = config->asset_id[0] != '\0' ? config->asset_id : MODEL_ASSET_ALIAS_UNASSIGNED;
    if (include_asset_id &&
        model_alias_pattern_matches(pattern, asset_id, &valid_pattern))
    {
        references[matches_size] = UA_EXPANDEDNODEID_STRING(1, (char *)"Device");
        matches[matches_size].aliasName =
            UA_QUALIFIEDNAME(g_model_amb_namespace_index, (char *)asset_id);
        matches[matches_size].referencedNodesSize = 1u;
        matches[matches_size].referencedNodes = &references[matches_size];
        matches_size++;
    }
    if (!valid_pattern)
    {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    return UA_Variant_setArrayCopy(&output[0], matches, matches_size,
                                   &UA_TYPES[UA_TYPES_ALIASNAMEDATATYPE]);
}

