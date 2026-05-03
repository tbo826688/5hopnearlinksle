# samples/Hybrid_mode/CMakeLists.txt

# 如果选中了 SLE UART (注意：这个宏名字要和 Kconfig 里定义的一致)
if(DEFINED CONFIG_SAMPLE_SUPPORT_SLE_UART_HYBRID_SAMPLE)
    add_subdirectory_if_exist(sle_uart)
endif()

# 如果以后有别的 demo，也可以加在这里
# if(DEFINED CONFIG_SAMPLE_SUPPORT_OTHER_DEMO)
#     add_subdirectory_if_exist(other_demo)
# endif()

# 将源码列表向上层传递
set(SOURCES "${SOURCES}" PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" PARENT_SCOPE)