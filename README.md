# 按键宏 (KeyMacro)

一个基于 **Qt6 + OpenCV** 的 Windows 全局按键宏工具：录制/回放键鼠宏、按前台进程自动激活宏组、图像识别自动执行、全局热键、系统托盘常驻。

> 仅用于个人效率/自动化场景，请遵守目标软件的服务条款与当地法律法规。

## 功能

- **全局键鼠钩子**：`WH_KEYBOARD_LL` / `WH_MOUSE_LL`，单键或组合键（如 `Ctrl+A`）热键，可屏蔽热键防止穿透
- **宏录制/编辑/回放**：按键、鼠标点击、滚轮、延迟；4 种重复模式（单次 / 按住重复 / 按住执行 / 按一下重复再按停止）
- **宏组**：按前台进程名（如 `game.exe`）自动激活对应组，支持优先级与"全局激活"
- **图像识别**：OpenCV 模板匹配；优先使用 Windows Graphics Capture 硬件截屏，失败回退 GDI；命中后自学习位置，后续只匹配局部区域
- **图像自动执行**：检测到目标图像后移动鼠标到图像中心并循环执行组内宏，目标消失/切走前台自动停止
- **其他**：屏蔽动作键位、鼠标锁定、单实例运行、开机自启（计划任务）、托盘最小化、分类管理
- **配置持久化**：程序目录 `macros.ini`（JSON），保存时原子写入并自动生成 `.bak` 备份

## 环境要求

| 依赖 | 版本 |
|---|---|
| Windows | 10 / 11 x64 |
| Qt | 6.5+（Widgets、Network，MSVC 2022 64-bit 构建） |
| OpenCV | 4.x（core、imgproc、imgcodecs） |
| 编译器 | Visual Studio 2022 / 2026（MSVC） |
| 构建工具 | CMake 3.16+ |

程序以管理员权限运行（全局键鼠钩子与输入注入需要）。

## 构建

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_PREFIX_PATH="C:/Qt/6.8.0/msvc2022_64" `
      -DOpenCV_DIR="C:/opencv/build"
cmake --build build --config Release --parallel
```

产物：`build/Release/KeyMacro.exe`。

运行时需要：

1. 用 `windeployqt` 部署 Qt 运行库到 exe 同目录：
   ```powershell
   windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw build\Release\KeyMacro.exe
   ```
2. 把 OpenCV 运行库（如 `opencv_world4120.dll`，或 vcpkg 的 `opencv_core4.dll` 等）复制到 exe 同目录。

## 使用

1. 首次启动会创建默认宏组和演示宏。
2. **新建宏** → 用 F11 开始 / F12 停止录制，或手动添加延迟/按键步骤 → 点"录制绑定"设置触发键 → 保存。
3. **宏组**：双击组设置目标进程名（如 `game.exe`）与组内宏；该程序在前台时组内宏才会触发。也可勾选"全局激活"。
4. **图像识别**：在组编辑里添加目标图像；勾选"移动鼠标到目标图像中央并自动执行组内宏"后，检测到图像即自动执行。
5. 配置保存在程序目录 `macros.ini`；保存时自动生成 `macros.ini.bak`。

## 打包安装程序

仓库 `installer/KeyMacro.iss` 是 [Inno Setup 6](https://jrsoftware.org/isinfo.php) 脚本。先按上文构建出 `dist/KeyMacro/`（exe + Qt/OpenCV 运行库），再执行：

```powershell
iscc installer\KeyMacro.iss
```

生成的安装包会安装程序文件，并**保留用户已有的 `macros.ini`、`image/`、`config/` 数据**（卸载时也不删除）。

## 目录结构

```
CMakeLists.txt
main.cpp            # UI（MainWindow、宏组/宏/步骤编辑、托盘、单实例）
MacroEngine.cpp/.h  # 全局钩子、录制、宏播放线程、图像识别、配置持久化
resources.qrc       # 图标资源
version.rc          # Windows 版本信息
installer/          # Inno Setup 安装脚本
```

## 许可证

[GPL-3.0](LICENSE)
