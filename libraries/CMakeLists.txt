# Ethernet
add_library(ETHERNET_FILES STATIC)

target_sources(ETHERNET_FILES PRIVATE
        ${WIZNET_DIR}/Ethernet/socket.c
        ${WIZNET_DIR}/Ethernet/wizchip_conf.c
        )

if(${WIZNET_CHIP} STREQUAL W5100S)
target_include_directories(ETHERNET_FILES INTERFACE
        ${WIZNET_DIR}/Ethernet
        ${WIZNET_DIR}/Ethernet/W5100S
        )

target_link_libraries(ETHERNET_FILES PUBLIC
        W5100S_FILES
        )
elseif(${WIZNET_CHIP} STREQUAL W5500)
target_include_directories(ETHERNET_FILES INTERFACE
        ${WIZNET_DIR}/Ethernet
        ${WIZNET_DIR}/Ethernet/W5500
        )

target_link_libraries(ETHERNET_FILES PUBLIC
        W5500_FILES
        )
endif()

if(${WIZNET_CHIP} STREQUAL W5100S)
add_library(W5100S_FILES STATIC)

target_sources(W5100S_FILES PRIVATE
        ${WIZNET_DIR}/Ethernet/W5100S/w5100s.c
        )

target_include_directories(W5100S_FILES INTERFACE
        ${WIZNET_DIR}/Ethernet
        ${WIZNET_DIR}/Ethernet/W5100S
        )

target_link_libraries(W5100S_FILES PRIVATE
        ETHERNET_FILES
        )
elseif(${WIZNET_CHIP} STREQUAL W5500)
add_library(W5500_FILES STATIC)

target_sources(W5500_FILES PRIVATE
        ${WIZNET_DIR}/Ethernet/W5500/w5500.c
        )

target_include_directories(W5500_FILES INTERFACE
        ${WIZNET_DIR}/Ethernet
        ${WIZNET_DIR}/Ethernet/W5500
        )

target_link_libraries(W5500_FILES PRIVATE
        ETHERNET_FILES
        )
endif()

# Loopback
add_library(LOOPBACK_FILES STATIC)

target_sources(LOOPBACK_FILES PRIVATE
        ${WIZNET_DIR}/Application/loopback/loopback.c
        )

target_include_directories(LOOPBACK_FILES PUBLIC
        ${WIZNET_DIR}/Ethernet
        ${WIZNET_DIR}/Application/loopback
        )

# DHCP
add_library(DHCP_FILES STATIC)

target_sources(DHCP_FILES PRIVATE
        ${WIZNET_DIR}/Internet/DHCP/dhcp.c
        )

target_include_directories(DHCP_FILES PUBLIC
        ${WIZNET_DIR}/Ethernet
        ${WIZNET_DIR}/Internet/DHCP
        )

# DNS
add_library(DNS_FILES STATIC)

target_sources(DNS_FILES PRIVATE
        ${WIZNET_DIR}/Internet/DNS/dns.c
        )

target_include_directories(DNS_FILES PUBLIC
        ${WIZNET_DIR}/Ethernet
        ${WIZNET_DIR}/Internet/DNS
        )

# FTP Client
# add_library(FTPCLIENT_FILES STATIC)

# target_sources(FTPCLIENT_FILES PUBLIC
#         ${WIZNET_DIR}/Internet/FTPClient/ftpc.c
#         )

# target_include_directories(FTPCLIENT_FILES PUBLIC
#         ${WIZNET_DIR}/Ethernet
#         ${WIZNET_DIR}/Internet/FTPClient
#         )

# # FTP Server
# add_library(FTPSERVER_FILES STATIC)

# target_sources(FTPSERVER_FILES PUBLIC
#         ${WIZNET_DIR}/Internet/FTPServer/ftpd.c
#         )

# target_include_directories(FTPSERVER_FILES PUBLIC
#         ${WIZNET_DIR}/Ethernet
#         ${WIZNET_DIR}/Internet/FTPServer
#         )

# HTTP Server
add_library(HTTPSERVER_FILES STATIC)

target_sources(HTTPSERVER_FILES PRIVATE
        ${WIZNET_DIR}/Internet/httpServer/httpParser.c
        ${WIZNET_DIR}/Internet/httpServer/httpServer.c
        ${WIZNET_DIR}/Internet/httpServer/httpUtil.c
        )

target_include_directories(HTTPSERVER_FILES PUBLIC
        ${WIZNET_DIR}/Ethernet
        ${WIZNET_DIR}/Internet/httpServer
        )

# MQTT
add_library(MQTT_FILES STATIC)

target_sources(MQTT_FILES PRIVATE
        ${WIZNET_DIR}/Internet/MQTT/mqtt_interface.c
        ${WIZNET_DIR}/Internet/MQTT/MQTTClient.c
        ${WIZNET_DIR}/Internet/MQTT/MQTTPacket/src/MQTTConnectClient.c
        ${WIZNET_DIR}/Internet/MQTT/MQTTPacket/src/MQTTConnectServer.c
        ${WIZNET_DIR}/Internet/MQTT/MQTTPacket/src/MQTTDeserializePublish.c
        ${WIZNET_DIR}/Internet/MQTT/MQTTPacket/src/MQTTFormat.c
        ${WIZNET_DIR}/Internet/MQTT/MQTTPacket/src/MQTTPacket.c
        ${WIZNET_DIR}/Internet/MQTT/MQTTPacket/src/MQTTSerializePublish.c
        ${WIZNET_DIR}/Internet/MQTT/MQTTPacket/src/MQTTSubscribeClient.c
        ${WIZNET_DIR}/Internet/MQTT/MQTTPacket/src/MQTTSubscribeServer.c
        ${WIZNET_DIR}/Internet/MQTT/MQTTPacket/src/MQTTUnsubscribeClient.c
        ${WIZNET_DIR}/Internet/MQTT/MQTTPacket/src/MQTTUnsubscribeServer.c
        )

target_include_directories(MQTT_FILES PUBLIC
        ${WIZNET_DIR}/Ethernet
        ${WIZNET_DIR}/Internet/MQTT
        ${WIZNET_DIR}/Internet/MQTTPacket/src
        ) 

# SNTP
add_library(SNTP_FILES STATIC)

target_sources(SNTP_FILES PRIVATE
        ${WIZNET_DIR}/Internet/SNTP/sntp.c
        )

target_include_directories(SNTP_FILES PUBLIC
        ${WIZNET_DIR}/Ethernet
        ${WIZNET_DIR}/Internet/SNTP
        )

# FreeRTOS
add_library(FREERTOS_FILES STATIC)

if (PICO_RP2040)
    set(FREERTOS_PORT_DIRECTORY ${FREERTOS_DIR}/portable/ThirdParty/GCC/RP2040)
    set(FREERTOS_PORT_SOURCES
            ${FREERTOS_PORT_DIRECTORY}/port.c)
    set(FREERTOS_PORT_INCLUDE ${FREERTOS_PORT_DIRECTORY}/include)
    set(FREERTOS_PORT_ADAPTER_HEADER ${FREERTOS_PORT_INCLUDE}/freertos_sdk_config.h)
elseif (PICO_RP2350)
    set(FREERTOS_PORT_DIRECTORY ${FREERTOS_DIR}/portable/ThirdParty/GCC/RP2350_ARM_NTZ)
    set(FREERTOS_PORT_SOURCES
            ${FREERTOS_PORT_DIRECTORY}/non_secure/port.c
            ${FREERTOS_PORT_DIRECTORY}/non_secure/portasm.c)
    set(FREERTOS_PORT_INCLUDE ${FREERTOS_PORT_DIRECTORY}/non_secure)
    set(FREERTOS_PORT_ADAPTER_HEADER ${FREERTOS_PORT_INCLUDE}/freertos_sdk_config.h)
else()
    message(FATAL_ERROR "Unsupported RP2 platform for FreeRTOS sources")
endif()

target_sources(FREERTOS_FILES PRIVATE
        ${FREERTOS_DIR}/croutine.c
        ${FREERTOS_DIR}/event_groups.c
        ${FREERTOS_DIR}/list.c
        ${FREERTOS_DIR}/queue.c
        ${FREERTOS_DIR}/stream_buffer.c
        ${FREERTOS_DIR}/tasks.c
        ${FREERTOS_DIR}/timers.c
        ${FREERTOS_PORT_SOURCES}
        ${FREERTOS_DIR}/portable/MemMang/heap_4.c
        )

target_include_directories(FREERTOS_FILES PUBLIC
        ${PORT_DIR}/FreeRTOS-Kernel/inc
        ${FREERTOS_DIR}/include
        ${FREERTOS_PORT_INCLUDE}
        )

target_compile_definitions(FREERTOS_FILES PUBLIC
        PICO_CONFIG_RTOS_ADAPTER_HEADER=${FREERTOS_PORT_ADAPTER_HEADER}
        LIB_FREERTOS_KERNEL=1
        FREE_RTOS_KERNEL_SMP=1
        )

target_link_libraries(FREERTOS_FILES PUBLIC
        pico_base_headers
        hardware_clocks
        hardware_exception
        pico_multicore
        )
