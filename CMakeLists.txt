cmake_minimum_required(VERSION 3.10)
project(memorypool_http_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

# ---- Muduo 位置 ----
# Muduo 不在系统路径,我们把它编译安装到了本地目录(非 /usr/local,避免 sudo)。
# 默认指向 ~/code/muduo 的本地安装产物;如装到别处可在配置时用 -DMUDUO_PREFIX=... 覆盖。
if(NOT DEFINED MUDUO_PREFIX)
    set(MUDUO_PREFIX "$ENV{HOME}/code/muduo/build/release-install-cpp11")
endif()
message(STATUS "Using MUDUO_PREFIX = ${MUDUO_PREFIX}")

include_directories(
    ${PROJECT_SOURCE_DIR}/src
    ${MUDUO_PREFIX}/include
)
link_directories(${MUDUO_PREFIX}/lib)

# ---- 源文件 ----
# 只收集服务自身代码(http/ + main.cpp)。内存池接管 TU 单独按开关加入,
# 不能用通配把 src/pool/ 也收进来,否则 USE_MEMORY_POOL=OFF 时也会链入池。
file(GLOB SERVER_SRC
    "${PROJECT_SOURCE_DIR}/src/http/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/main.cpp")

# ---- 内存池接入开关(P1 的 A/B 压测核心)----
# ON  = 把自研内存池接管成全局 operator new/delete(池版)
# OFF = 不链入,走默认 glibc malloc(基线版)
# 用法:cmake -DUSE_MEMORY_POOL=ON ..   /   cmake -DUSE_MEMORY_POOL=OFF ..
option(USE_MEMORY_POOL "Route global new/delete through the custom memory pool" ON)
# 池统计计数器(命中率/各层调用次数)。P2 压测分析时打开,P1 默认关。
option(MEMORY_POOL_STATS "Enable memory pool stats counters" OFF)

set(POOL_DIR ${PROJECT_SOURCE_DIR}/third_party/memory_pool)

add_executable(http_server ${SERVER_SRC})

if(USE_MEMORY_POOL)
    file(GLOB POOL_SRC "${POOL_DIR}/src/*.cpp")
    target_sources(http_server PRIVATE
        ${POOL_SRC}
        ${PROJECT_SOURCE_DIR}/src/pool/GlobalNewDelete.cpp)
    target_include_directories(http_server PRIVATE ${POOL_DIR}/include)
    target_compile_definitions(http_server PRIVATE USE_MEMORY_POOL)
    if(MEMORY_POOL_STATS)
        target_compile_definitions(http_server PRIVATE ENABLE_MEMORY_POOL_STATS)
    endif()
    message(STATUS "Memory pool: ON (stats=${MEMORY_POOL_STATS})")
else()
    message(STATUS "Memory pool: OFF (baseline glibc malloc)")
endif()

# muduo_net 依赖 muduo_base;两者都依赖 pthread。顺序:net 在前。
target_link_libraries(http_server
    muduo_net
    muduo_base
    pthread
)
