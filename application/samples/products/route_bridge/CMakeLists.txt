#===============================================================================
# @brief    Dual-board SLE route bridge
#===============================================================================

if(DEFINED CONFIG_SAMPLE_SUPPORT_ROUTE_BRIDGE_RX)
    set(SOURCES_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/../route/route_client/route_client.c
        ${CMAKE_CURRENT_SOURCE_DIR}/route_bridge_uart.c
        ${CMAKE_CURRENT_SOURCE_DIR}/route_bridge.c
    )
    set(HEADER_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../route/route_client
    )
elseif(DEFINED CONFIG_SAMPLE_SUPPORT_ROUTE_BRIDGE_TX)
    set(SOURCES_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/../route/route_server/route_server_adv.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../route/route_server/route_server.c
        ${CMAKE_CURRENT_SOURCE_DIR}/route_bridge_uart.c
        ${CMAKE_CURRENT_SOURCE_DIR}/route_bridge.c
    )
    set(HEADER_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../route/route_server
    )
endif()

set(SOURCES "${SOURCES}" ${SOURCES_LIST} PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" ${HEADER_LIST} PARENT_SCOPE)
