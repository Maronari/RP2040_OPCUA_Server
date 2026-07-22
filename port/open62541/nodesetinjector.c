/* Minimal OPC UA DI subset used by the embedded profile. Numeric NodeIds and
 * the hierarchy match the normative OPCFoundation/UA-Nodeset DI model. */

#include "nodesetinjector.h"

#include <stdio.h>

static UA_StatusCode
addOptionalModellingRule(UA_Server *server, UA_NodeId nodeId) {
    return UA_Server_addReference(server, nodeId,
                                  UA_NODEID_NUMERIC(0, UA_NS0ID_HASMODELLINGRULE),
                                  UA_EXPANDEDNODEID_NUMERIC(0, UA_NS0ID_MODELLINGRULE_OPTIONAL),
                                  true);
}

static UA_StatusCode
addObjectType(UA_Server *server, UA_UInt16 ns, UA_UInt32 nodeId,
              const char *browseName, UA_NodeId parentType, UA_Boolean isAbstract) {
    UA_ObjectTypeAttributes attr = UA_ObjectTypeAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browseName);
    attr.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browseName);
    attr.isAbstract = isAbstract;
    return UA_Server_addObjectTypeNode(server,
                                       UA_NODEID_NUMERIC(ns, nodeId),
                                       parentType,
                                       UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE),
                                       UA_QUALIFIEDNAME(ns, (char *)browseName),
                                       attr, NULL, NULL);
}

static UA_StatusCode
addDeviceHealthEnumeration(UA_Server *server, UA_UInt16 ns) {
    UA_DataTypeAttributes typeAttr = UA_DataTypeAttributes_default;
    UA_VariableAttributes stringsAttr = UA_VariableAttributes_default;
    UA_UInt32 arrayDimension = 5u;
    UA_LocalizedText enumStrings[5] = {
        UA_LOCALIZEDTEXT((char *)"en-US", (char *)"NORMAL"),
        UA_LOCALIZEDTEXT((char *)"en-US", (char *)"FAILURE"),
        UA_LOCALIZEDTEXT((char *)"en-US", (char *)"CHECK_FUNCTION"),
        UA_LOCALIZEDTEXT((char *)"en-US", (char *)"OFF_SPEC"),
        UA_LOCALIZEDTEXT((char *)"en-US", (char *)"MAINTENANCE_REQUIRED")
    };
    UA_StatusCode res;

    typeAttr.displayName = UA_LOCALIZEDTEXT((char *)"en-US",
                                            (char *)"DeviceHealthEnumeration");
    typeAttr.description = UA_LOCALIZEDTEXT((char *)"en-US",
                                             (char *)"NAMUR NE 107 device health state");
    res = UA_Server_addDataTypeNode(server,
                                    UA_NODEID_NUMERIC(ns, UA_DIID_DEVICEHEALTHENUMERATION),
                                    UA_NODEID_NUMERIC(0, UA_NS0ID_ENUMERATION),
                                    UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE),
                                    UA_QUALIFIEDNAME(ns, (char *)"DeviceHealthEnumeration"),
                                    typeAttr, NULL, NULL);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    stringsAttr.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)"EnumStrings");
    stringsAttr.dataType = UA_TYPES[UA_TYPES_LOCALIZEDTEXT].typeId;
    stringsAttr.valueRank = 1;
    stringsAttr.arrayDimensions = &arrayDimension;
    stringsAttr.arrayDimensionsSize = 1u;
    stringsAttr.accessLevel = UA_ACCESSLEVELMASK_READ;
    stringsAttr.userAccessLevel = UA_ACCESSLEVELMASK_READ;
    res = UA_Variant_setArrayCopy(&stringsAttr.value, enumStrings, 5,
                                  &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);
    if(res != UA_STATUSCODE_GOOD) {
        UA_Variant_clear(&stringsAttr.value);
        return res;
    }

    res = UA_Server_addVariableNode(
        server,
        UA_NODEID_NUMERIC(ns, UA_DIID_DEVICEHEALTHENUMERATION_ENUMSTRINGS),
        UA_NODEID_NUMERIC(ns, UA_DIID_DEVICEHEALTHENUMERATION),
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
        UA_QUALIFIEDNAME(0, (char *)"EnumStrings"),
        UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE),
        stringsAttr, NULL, NULL);
    UA_Variant_clear(&stringsAttr.value);
    return res;
}

static UA_StatusCode
addFunctionalGroupType(UA_Server *server, UA_UInt16 ns) {
    return addObjectType(server, ns, UA_DIID_FUNCTIONALGROUPTYPE,
                         "FunctionalGroupType",
                         UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE), false);
}

static UA_StatusCode
addDeviceHealthInterface(UA_Server *server, UA_UInt16 ns) {
    UA_VariableAttributes healthAttr = UA_VariableAttributes_default;
    UA_ObjectAttributes alarmsAttr = UA_ObjectAttributes_default;
    UA_StatusCode res;

    res = addObjectType(server, ns, UA_DIID_IDEVICEHEALTHTYPE,
                        "IDeviceHealthType",
                        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEINTERFACETYPE), true);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    healthAttr.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)"DeviceHealth");
    healthAttr.description = UA_LOCALIZEDTEXT((char *)"en-US",
                                               (char *)"Overall device health state");
    healthAttr.dataType = UA_NODEID_NUMERIC(ns, UA_DIID_DEVICEHEALTHENUMERATION);
    healthAttr.valueRank = UA_VALUERANK_SCALAR;
    healthAttr.accessLevel = UA_ACCESSLEVELMASK_READ;
    healthAttr.userAccessLevel = UA_ACCESSLEVELMASK_READ;
    res = UA_Server_addVariableNode(
        server,
        UA_NODEID_NUMERIC(ns, UA_DIID_IDEVICEHEALTHTYPE_DEVICEHEALTH),
        UA_NODEID_NUMERIC(ns, UA_DIID_IDEVICEHEALTHTYPE),
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
        UA_QUALIFIEDNAME(ns, (char *)"DeviceHealth"),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
        healthAttr, NULL, NULL);
    if(res != UA_STATUSCODE_GOOD)
        return res;
    res = addOptionalModellingRule(
        server, UA_NODEID_NUMERIC(ns, UA_DIID_IDEVICEHEALTHTYPE_DEVICEHEALTH));
    if(res != UA_STATUSCODE_GOOD)
        return res;

    alarmsAttr.displayName = UA_LOCALIZEDTEXT((char *)"en-US",
                                               (char *)"DeviceHealthAlarms");
    alarmsAttr.description = UA_LOCALIZEDTEXT((char *)"en-US",
                                               (char *)"Device health alarm conditions");
    res = UA_Server_addObjectNode(
        server,
        UA_NODEID_NUMERIC(ns, UA_DIID_IDEVICEHEALTHTYPE_DEVICEHEALTHALARMS),
        UA_NODEID_NUMERIC(ns, UA_DIID_IDEVICEHEALTHTYPE),
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
        UA_QUALIFIEDNAME(ns, (char *)"DeviceHealthAlarms"),
        UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE),
        alarmsAttr, NULL, NULL);
    if(res != UA_STATUSCODE_GOOD)
        return res;
    return addOptionalModellingRule(
        server, UA_NODEID_NUMERIC(ns, UA_DIID_IDEVICEHEALTHTYPE_DEVICEHEALTHALARMS));
}

static UA_StatusCode
addDeviceHealthAlarmTypes(UA_Server *server, UA_UInt16 ns) {
    UA_StatusCode res = addObjectType(
        server, ns, UA_DIID_DEVICEHEALTHDIAGNOSTICALARMTYPE,
        "DeviceHealthDiagnosticAlarmType",
        UA_NODEID_NUMERIC(0, UA_NS0ID_INSTRUMENTDIAGNOSTICALARMTYPE), true);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = addObjectType(server, ns, UA_DIID_FAILUREALARMTYPE, "FailureAlarmType",
                        UA_NODEID_NUMERIC(ns, UA_DIID_DEVICEHEALTHDIAGNOSTICALARMTYPE), false);
    if(res != UA_STATUSCODE_GOOD)
        return res;
    res = addObjectType(server, ns, UA_DIID_CHECKFUNCTIONALARMTYPE,
                        "CheckFunctionAlarmType",
                        UA_NODEID_NUMERIC(ns, UA_DIID_DEVICEHEALTHDIAGNOSTICALARMTYPE), false);
    if(res != UA_STATUSCODE_GOOD)
        return res;
    res = addObjectType(server, ns, UA_DIID_OFFSPECALARMTYPE, "OffSpecAlarmType",
                        UA_NODEID_NUMERIC(ns, UA_DIID_DEVICEHEALTHDIAGNOSTICALARMTYPE), false);
    if(res != UA_STATUSCODE_GOOD)
        return res;
    return addObjectType(server, ns, UA_DIID_MAINTENANCEREQUIREDALARMTYPE,
                         "MaintenanceRequiredAlarmType",
                         UA_NODEID_NUMERIC(ns, UA_DIID_DEVICEHEALTHDIAGNOSTICALARMTYPE), false);
}

static UA_StatusCode
addAmbFindAliasMethod(UA_Server *server, UA_UInt16 ns, UA_UInt32 parentId,
                      UA_UInt32 methodId, UA_UInt32 inputArgumentsId,
                      UA_UInt32 outputArgumentsId) {
#ifdef UA_ENABLE_METHODCALLS
    UA_MethodAttributes attr = UA_MethodAttributes_default;
    UA_Argument inputArguments[2];
    UA_Argument outputArgument;
    UA_UInt32 outputArrayDimension = 0u;

    UA_Argument_init(&inputArguments[0]);
    inputArguments[0].name = UA_STRING((char *)"AliasNameSearchPattern");
    inputArguments[0].dataType = UA_TYPES[UA_TYPES_STRING].typeId;
    inputArguments[0].valueRank = UA_VALUERANK_SCALAR;

    UA_Argument_init(&inputArguments[1]);
    inputArguments[1].name = UA_STRING((char *)"ReferenceTypeFilter");
    inputArguments[1].dataType = UA_TYPES[UA_TYPES_NODEID].typeId;
    inputArguments[1].valueRank = UA_VALUERANK_SCALAR;

    UA_Argument_init(&outputArgument);
    outputArgument.name = UA_STRING((char *)"AliasNodeList");
    outputArgument.dataType = UA_TYPES[UA_TYPES_ALIASNAMEDATATYPE].typeId;
    outputArgument.valueRank = 1;
    outputArgument.arrayDimensionsSize = 1u;
    outputArgument.arrayDimensions = &outputArrayDimension;

    attr.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)"FindAlias");
    attr.description = UA_LOCALIZEDTEXT((char *)"en-US",
                                        (char *)"Find assets by AliasName");
    attr.executable = true;
    attr.userExecutable = true;

    return UA_Server_addMethodNodeEx(
        server,
        UA_NODEID_NUMERIC(ns, methodId),
        UA_NODEID_NUMERIC(ns, parentId),
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
        UA_QUALIFIEDNAME(0, (char *)"FindAlias"),
        attr,
        NULL,
        2u,
        inputArguments,
        UA_NODEID_NUMERIC(ns, inputArgumentsId),
        NULL,
        1u,
        &outputArgument,
        UA_NODEID_NUMERIC(ns, outputArgumentsId),
        NULL,
        NULL,
        NULL);
#else
    (void)server;
    (void)ns;
    (void)parentId;
    (void)methodId;
    (void)inputArgumentsId;
    (void)outputArgumentsId;
    return UA_STATUSCODE_GOOD;
#endif
}

static UA_StatusCode
addAmbNodeVersion(UA_Server *server, UA_UInt16 ns, UA_UInt32 parentId,
                  UA_UInt32 nodeId) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    UA_String initialVersion = UA_STRING((char *)"1");
    UA_StatusCode res;

    attr.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)"NodeVersion");
    attr.description = UA_LOCALIZEDTEXT((char *)"en-US",
                                        (char *)"Version of the asset alias category");
    attr.dataType = UA_TYPES[UA_TYPES_STRING].typeId;
    attr.valueRank = UA_VALUERANK_SCALAR;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ;
    attr.userAccessLevel = UA_ACCESSLEVELMASK_READ;
    res = UA_Variant_setScalarCopy(&attr.value, &initialVersion,
                                   &UA_TYPES[UA_TYPES_STRING]);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = UA_Server_addVariableNode(
        server,
        UA_NODEID_NUMERIC(ns, nodeId),
        UA_NODEID_NUMERIC(ns, parentId),
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
        UA_QUALIFIEDNAME(0, (char *)"NodeVersion"),
        UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE),
        attr,
        NULL,
        NULL);
    UA_Variant_clear(&attr.value);
    return res;
}

static UA_StatusCode
addAmbAliasCategory(UA_Server *server, UA_UInt16 ns, UA_UInt32 nodeId,
                    const char *browseName, UA_NodeId parentId,
                    UA_UInt32 methodId, UA_UInt32 inputArgumentsId,
                    UA_UInt32 outputArgumentsId, UA_UInt32 nodeVersionId) {
    UA_ObjectAttributes attr = UA_ObjectAttributes_default;
    UA_StatusCode res;

    attr.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browseName);
    attr.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browseName);
    res = UA_Server_addNode_begin(
        server,
        UA_NODECLASS_OBJECT,
        UA_NODEID_NUMERIC(ns, nodeId),
        parentId,
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
        UA_QUALIFIEDNAME(ns, (char *)browseName),
        UA_NODEID_NUMERIC(0, UA_NS0ID_ALIASNAMECATEGORYTYPE),
        &attr,
        &UA_TYPES[UA_TYPES_OBJECTATTRIBUTES],
        NULL,
        NULL);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = addAmbFindAliasMethod(server, ns, nodeId, methodId,
                                inputArgumentsId, outputArgumentsId);
    if(res == UA_STATUSCODE_GOOD && nodeVersionId != 0u)
        res = addAmbNodeVersion(server, ns, nodeId, nodeVersionId);
    if(res == UA_STATUSCODE_GOOD)
        res = UA_Server_addNode_finish(server, UA_NODEID_NUMERIC(ns, nodeId));
    if(res != UA_STATUSCODE_GOOD)
        (void)UA_Server_deleteNode(server, UA_NODEID_NUMERIC(ns, nodeId), true);
    return res;
}

static UA_StatusCode
addAmbAssetDiscovery(UA_Server *server, UA_UInt16 ns) {
    UA_StatusCode res;

    res = addAmbAliasCategory(
        server, ns, UA_AMBID_ASSETS, "Assets",
        UA_NODEID_NUMERIC(0, UA_NS0ID_ALIASES),
        UA_AMBID_ASSETS_FIND_ALIAS,
        UA_AMBID_ASSETS_FIND_ALIAS_INPUT_ARGUMENTS,
        UA_AMBID_ASSETS_FIND_ALIAS_OUTPUT_ARGUMENTS,
        0u);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = addAmbAliasCategory(
        server, ns, UA_AMBID_ASSETS_BY_PRODUCT_INSTANCE_URI,
        "AssetsByProductInstanceUri",
        UA_NODEID_NUMERIC(ns, UA_AMBID_ASSETS),
        UA_AMBID_PRODUCT_INSTANCE_URI_FIND_ALIAS,
        UA_AMBID_PRODUCT_INSTANCE_URI_FIND_ALIAS_INPUT_ARGUMENTS,
        UA_AMBID_PRODUCT_INSTANCE_URI_FIND_ALIAS_OUTPUT_ARGUMENTS,
        UA_AMBID_PRODUCT_INSTANCE_URI_NODE_VERSION);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    return addAmbAliasCategory(
        server, ns, UA_AMBID_ASSETS_BY_ASSET_ID,
        "AssetsByAssetId",
        UA_NODEID_NUMERIC(ns, UA_AMBID_ASSETS),
        UA_AMBID_ASSET_ID_FIND_ALIAS,
        UA_AMBID_ASSET_ID_FIND_ALIAS_INPUT_ARGUMENTS,
        UA_AMBID_ASSET_ID_FIND_ALIAS_OUTPUT_ARGUMENTS,
        UA_AMBID_ASSET_ID_NODE_VERSION);
}

static UA_StatusCode
addAmbReferenceType(UA_Server *server, UA_UInt16 ns, UA_UInt32 nodeId,
                    const char *browseName, const char *inverseName,
                    const char *description,
                    UA_NodeId parentType, UA_Boolean isAbstract) {
    UA_ReferenceTypeAttributes attr = UA_ReferenceTypeAttributes_default;

    attr.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browseName);
    attr.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)description);
    attr.inverseName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)inverseName);
    attr.isAbstract = isAbstract;
    attr.symmetric = false;
    return UA_Server_addReferenceTypeNode(
        server,
        UA_NODEID_NUMERIC(ns, nodeId),
        parentType,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE),
        UA_QUALIFIEDNAME(ns, (char *)browseName),
        attr,
        NULL,
        NULL);
}

static UA_StatusCode
addAmbLocationEntryPoint(UA_Server *server, UA_UInt16 ns, UA_UInt32 nodeId,
                         const char *browseName, const char *description) {
    UA_ObjectAttributes attr = UA_ObjectAttributes_default;

    attr.displayName = UA_LOCALIZEDTEXT((char *)"en-US", (char *)browseName);
    attr.description = UA_LOCALIZEDTEXT((char *)"en-US", (char *)description);
    return UA_Server_addObjectNode(
        server,
        UA_NODEID_NUMERIC(ns, nodeId),
        UA_NODEID_NUMERIC(0, UA_NS0ID_LOCATIONS),
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
        UA_QUALIFIEDNAME(ns, (char *)browseName),
        UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE),
        attr,
        NULL,
        NULL);
}

static UA_StatusCode
addAmbLocations(UA_Server *server, UA_UInt16 ns) {
    UA_StatusCode res;

    res = addAmbReferenceType(
        server, ns, UA_AMBID_CONTAINS, "Contains", "LocatedIn",
        "Links a location object to assets located in that location",
        UA_NODEID_NUMERIC(0, UA_NS0ID_HIERARCHICALREFERENCES), true);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = addAmbReferenceType(
        server, ns, UA_AMBID_HIERARCHICAL_CONTAINS,
        "HierarchicalContains", "HierarchicalLocatedIn",
        "Links a hierarchical location object to contained locations or assets",
        UA_NODEID_NUMERIC(ns, UA_AMBID_CONTAINS), false);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = addAmbReferenceType(
        server, ns, UA_AMBID_OPERATIONAL_CONTAINS,
        "OperationalContains", "OperationalLocatedIn",
        "Links an operational location object to contained locations or assets",
        UA_NODEID_NUMERIC(ns, UA_AMBID_CONTAINS), false);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    res = addAmbLocationEntryPoint(
        server, ns, UA_AMBID_HIERARCHICAL_LOCATIONS,
        "HierarchicalLocations",
        "Entry point for objects representing the root of a location hierarchy");
    if(res != UA_STATUSCODE_GOOD)
        return res;

    return addAmbLocationEntryPoint(
        server, ns, UA_AMBID_OPERATIONAL_LOCATIONS,
        "OperationalLocations",
        "Entry point for objects representing the root of an operational location hierarchy");
}

UA_StatusCode
UA_Server_injectNodesets(UA_Server *server) {
    UA_UInt16 diNamespace;
    UA_UInt16 ambNamespace;
    UA_StatusCode res;

    if(!server)
        return UA_STATUSCODE_BADINVALIDARGUMENT;

    diNamespace = UA_Server_addNamespace(server, UA_DI_NAMESPACE_URI);
    if(diNamespace == 0u)
        return UA_STATUSCODE_BADOUTOFMEMORY;

    res = addFunctionalGroupType(server, diNamespace);
    if(res != UA_STATUSCODE_GOOD)
        return res;
    res = addDeviceHealthEnumeration(server, diNamespace);
    if(res != UA_STATUSCODE_GOOD)
        return res;
    res = addDeviceHealthInterface(server, diNamespace);
    if(res != UA_STATUSCODE_GOOD)
        return res;
    res = addDeviceHealthAlarmTypes(server, diNamespace);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    ambNamespace = UA_Server_addNamespace(server, UA_AMB_NAMESPACE_URI);
    if(ambNamespace == 0u)
        return UA_STATUSCODE_BADOUTOFMEMORY;
    res = addAmbAssetDiscovery(server, ambNamespace);
    if(res != UA_STATUSCODE_GOOD)
        return res;
    res = addAmbLocations(server, ambNamespace);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    printf("[open62541/nodesets]\tDI subset ns=%u, AMB subset ns=%u\n",
           (unsigned int)diNamespace, (unsigned int)ambNamespace);
    fflush(stdout);
    return UA_STATUSCODE_GOOD;
}
