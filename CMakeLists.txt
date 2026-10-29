cmake_minimum_required(VERSION 3.16...4.2)
project(KeyMacro VERSION 4.8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 设置 RC 编译器使用 UTF-8 编码，并指定资源包含路径
set(CMAKE_RC_FLAGS "/c65001 /I \"${CMAKE_SOURCE_DIR}\"")

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

# Qt6 与 OpenCV 的查找路径通过 -DCMAKE_PREFIX_PATH / -DOpenCV_DIR 指定，例如：
#   cmake -S . -B build -DCMAKE_PREFIX_PATH="C:/Qt/6.8.0/msvc2022_64" -DOpenCV_DIR="C:/opencv/build"
# 使用 vcpkg 时可改为 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)

add_executable(KeyMacro WIN32
    main.cpp
    MacroEngine.cpp
    resources.qrc
    version.rc
)

# WIN32 子系统默认入口是 WinMain，显式指定 mainCRTStartup 保持 main() 入口
target_link_options(KeyMacro PRIVATE
    "/ENTRY:mainCRTStartup"
    "/MANIFESTUAC:level='requireAdministrator'"
)

target_link_libraries(KeyMacro PRIVATE
    Qt6::Widgets
    Qt6::Network
    opencv_core
    opencv_imgproc
    opencv_imgcodecs
    winmm
    WindowsApp            # 启用 WinRT 支持 (包含 Windows::Graphics::Capture 等)
    d3d11                 # Direct3D 11
    dxgi                  # DXGI
)
 