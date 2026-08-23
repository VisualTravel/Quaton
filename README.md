# Quaton

>目前项目为临时适配版本，待主项目开源并完善后会进行完整开源

Quaton 是一个用于下载某些资源的工具，提供完整的下载与更新能力，以共享库形式提供。


## 构建

### 依赖

- CMake ≥ 3.20
- 支持 C++17 的编译器（Windows 上使用 MSVC）
- [vcpkg](https://github.com/microsoft/vcpkg)（依赖通过 `vcpkg.json` 管理）

### 步骤

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## 目录结构

- `src/`、`include/` — 客户端库源码与头文件
- `third_party/HDiffPatch` — 内联的 [HDiffPatch](https://github.com/sisong/HDiffPatch) 补丁库（MIT）
- `packager/`、`server/` — 配套组件，未随本仓库开源

## 许可证

本项目以 GPL-3.0 许可发布，详见 [LICENSE](LICENSE)。
