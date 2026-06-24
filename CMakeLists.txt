cmake_minimum_required(VERSION 3.10)
project(qwen3_vl_2b_npu)

# ── C++17 ──────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -fexceptions -pthread -O3")

# ── ROS2 ament ────────────────────────────────────────────────────
find_package(ament_cmake REQUIRED)

# ── OpenCV（优先 cmake，回退 pkg-config）───────────────────────────
find_package(OpenCV QUIET)
if(NOT OpenCV_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OPENCV REQUIRED opencv4)
    set(OpenCV_INCLUDE_DIRS ${OPENCV_INCLUDE_DIRS})
    set(OpenCV_LIBRARIES   ${OPENCV_LIBRARIES})
endif()

# ── ROS2 运行时依赖 ───────────────────────────────────────────────
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(std_msgs REQUIRED)
find_package(cv_bridge REQUIRED)
find_package(rosidl_default_generators REQUIRED)

# ── 生成自定义接口 ───────────────────────────────────────────
rosidl_generate_interfaces(${PROJECT_NAME}
    "srv/VlmQuery.srv"
    "action/StreamQuery.action"
)

# 获取 typesupport 目标（替代废弃的 rosidl_target_interfaces）
rosidl_get_typesupport_target(cpp_typesupport_target
    ${PROJECT_NAME} "rosidl_typesupport_cpp")

# ── Rockchip NPU 库 ────────────────────────────────────────────────
set(RK_LIB_PATH     "/usr/local/lib"     CACHE PATH "Rockchip .so 搜索路径")
set(RK_INCLUDE_PATH "/usr/local/include" CACHE PATH "Rockchip 头文件搜索路径")

# 兼容旧写法：明确指定库搜索路径，不给 colcon 加 --cmake-args 时
# 默认搜 /usr/local/lib；需要时用下方命令覆盖路径
#   colcon build --cmake-args -DRK_LIB_PATH=/path/to/libs ...
link_directories(${RK_LIB_PATH})

# ═══════════════════════════════════════════════════════════════════
# 静态库：RK35llm（供 vlm_server 链接）
# ═══════════════════════════════════════════════════════════════════
add_library(rk35llm_lib STATIC src/RK35llm.cpp)
target_include_directories(rk35llm_lib PUBLIC
    include
    ${RK_INCLUDE_PATH}
    ${OpenCV_INCLUDE_DIRS}
)
# 注意：rkllmrt 依赖 rknnrt，故 rkllmrt 放前面
target_link_libraries(rk35llm_lib PUBLIC
    ${OpenCV_LIBRARIES}
    -lrkllmrt
    -lrknnrt
    -fopenmp -ldl -lpthread
)

# ═══════════════════════════════════════════════════════════════════
# vlm_server — ROS2 服务端
# ═══════════════════════════════════════════════════════════════════
add_executable(vlm_server
    src/vlm_server.cpp
    src/VlmServerNode.cpp
)
target_include_directories(vlm_server PRIVATE include ${RK_INCLUDE_PATH})
ament_target_dependencies(vlm_server rclcpp rclcpp_action sensor_msgs std_msgs cv_bridge)
# rk35llm_lib(PUBLIC) 已携带 OpenCV + rknnrt + rkllmrt + -fopenmp 等，
# vlm_server 只需显式链接 rk35llm_lib 即可继承全部
target_link_libraries(vlm_server rk35llm_lib ${cpp_typesupport_target})

# ═══════════════════════════════════════════════════════════════════
# vlm_client — ROS2 客户端 REPL
# ═══════════════════════════════════════════════════════════════════
add_executable(vlm_client src/vlm_client.cpp)
ament_target_dependencies(vlm_client rclcpp rclcpp_action)
target_link_libraries(vlm_client ${cpp_typesupport_target})

# ═══════════════════════════════════════════════════════════════════
# 安装规则
# ═══════════════════════════════════════════════════════════════════
install(TARGETS vlm_server vlm_client
    DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY include/
    DESTINATION include)

ament_package()
