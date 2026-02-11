cmake_minimum_required(VERSION 3.10)
project(limitUpHittingMarket)

# 设置C++标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 核心：禁用所有警告（适用于 GCC、Clang）
# 对 C 语言代码禁用警告
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w")
# 对 C++ 语言代码禁用警告
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")

# 不使用Qt库（与CONFIG -= qt对应）
set(USE_QT OFF)

# 输出目录配置（可选，保持与Qt默认行为一致）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 包含目录（对应INCLUDEPATH）
include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include/itpdk
    ${PROJECT_SOURCE_DIR}/include/secitpdk
    ${PROJECT_SOURCE_DIR}/include/libzmq
    ${PROJECT_SOURCE_DIR}/lib
)

# 源文件（对应SOURCES）
set(SOURCES
    main.cpp
    market_data_api.cpp
    settings_manager.cpp
    spdlog_api.cpp
    utility_functions.cpp
    time_span_manager.cpp
    msg_queue.cpp
    order_manager_withdraw.cpp
    market_data_processor.cpp
    trade_return_monitor.cpp
    stock_data_manager.cpp
    stock_data_manager_factory.cpp
    trader_api.cpp
)

# 头文件（对应HEADERS，CMake中主要用于IDE显示）
set(HEADERS
    stdafx.h
    market_data_api.h
    settings_manager.h
    spdlog_api.h
    utility_functions.h
    time_span_manager.h
    market_data_processor.h
    msg_queue.h
    order_manager_withdraw.h
    stock_data_manager.h
    stock_data_manager_factory.h
    trade_return_monitor.h
    trader_api.h
)

# 分发文件（对应DISTFILES，用于安装或打包）
set(DISTFILES
    src/spdlog/fmt/bundled/LICENSE.rst
)

set(appName "limitUpHittingMarket")  # 最终生成的可执行文件名为 limitUpHittingBuy

# 创建可执行文件
add_executable(${appName} ${SOURCES} ${HEADERS})

# 链接库文件（根据不同操作系统）
if(WIN32)
    # Windows系统库链接
    target_link_libraries(${appName} PRIVATE
        ${PROJECT_SOURCE_DIR}/lib/fixapi50.lib
        ${PROJECT_SOURCE_DIR}/lib/secitpdk.lib
        ${PROJECT_SOURCE_DIR}/lib/TDFAPI30.lib
    )
elseif(UNIX AND NOT APPLE)
    # Linux系统库链接
    target_link_libraries(${appName} PRIVATE
        ${PROJECT_SOURCE_DIR}/lib/libTDFAPI30.so
        ${PROJECT_SOURCE_DIR}/lib/libfixapi.so
        ${PROJECT_SOURCE_DIR}/lib/libfixapitool.so
        ${PROJECT_SOURCE_DIR}/lib/libsecitpdk.so
        ${PROJECT_SOURCE_DIR}/lib/libzmq.so
    )
    # 添加运行时库路径（等效于-rpath）
    set_target_properties(${appName} PROPERTIES
        INSTALL_RPATH "$ORIGIN/lib"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()

# 控制台应用配置（对应CONFIG += console）
if(WIN32)
    # Windows下强制控制台输出
    set_target_properties(${appName} PROPERTIES
        WIN32_EXECUTABLE FALSE
    )
endif()

# 链接线程库（保留此部分，解决pthread错误，添加PRIVATE保持语法一致）
find_package(Threads REQUIRED)
target_link_libraries(${appName} PRIVATE Threads::Threads)

# 安装配置（可选）
install(TARGETS ${appName}
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES ${DISTFILES} DESTINATION share/${appName})
