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

UA_StatusCode
UA_Server_injectNodesets(UA_Server *server) {
    UA_UInt16 diNamespace;
    UA_StatusCode res;

    if(!server)
        return UA_STATUSCODE_BADINVALIDARGUMENT;

    diNamespace = UA_Server_addNamespace(server, UA_DI_NAMESPACE_URI);
    if(diNamespace == 0u)
        return UA_STATUSCODE_BADOUTOFMEMORY;

    res = addDeviceHealthEnumeration(server, diNamespace);
    if(res != UA_STATUSCODE_GOOD)
        return res;
    res = addDeviceHealthInterface(server, diNamespace);
    if(res != UA_STATUSCODE_GOOD)
        return res;
    res = addDeviceHealthAlarmTypes(server, diNamespace);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    printf("[open62541/DI]\tsubset ready, namespace=%u\n",
           (unsigned int)diNamespace);
    fflush(stdout);
    return UA_STATUSCODE_GOOD;
}
