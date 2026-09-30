# setup scripts

这些脚本是搭建本机开发环境的完整记录，按顺序执行即可复现两个计算器实现所依赖的工具链。
全部需要**以管理员身份**运行（脚本内部通过 `winget` / `pacman` 安装到系统级路径）。

| 脚本 | 作用 |
| --- | --- |
| `setup-toolchains.ps1` | 安装 JDK 25 (Temurin)、Python 3.13、MinGW-w64 GCC 16.2、CMake、Ninja，并写入 `JAVA_HOME` 与系统 PATH |
| `fix-mingw-path.ps1` | 把 MinGW-w64 从含空格的 `C:\Program Files\WinLibs` 迁到 `C:\mingw64` —— 否则 `ld.exe` 会在空格处截断路径导致链接失败 |
| `msvc-install.ps1` | 安装 Visual Studio 2022 Build Tools 的 VCTools 工作负载（`cl.exe`） |
| `install-msys2-gmp.ps1` | 安装 MSYS2，初始化 pacman keyring，安装 `mingw-w64-x86_64-gcc` 与 `mingw-w64-x86_64-gmp` |
| `fix-msys2-mirror.ps1` | 诊断并处理 pacman 下载卡死：把国内镜像（USTC / TUNA）提到 mirrorlist 最前面 |

## 使用方式

```powershell
# 方式一：逐个执行（每个脚本都会弹一次 UAC）
Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','.\setup-toolchains.ps1' -Wait

# 方式二：先检查语法再提权运行
powershell -NoProfile -Command "$null=[System.Management.Automation.Language.Parser]::ParseFile('.\setup-toolchains.ps1',[ref]$null,[ref]$e); $e"
```

## 两个必须知道的坑

1. **PowerShell 5.1 按 ANSI 读取 `.ps1`、`cmd.exe` 按 OEM 代码页读取 `.cmd`。**
   脚本里出现中文注释会导致解析失败（首个脚本曾因此直接退出 1、连日志都不写）。
   因此这些脚本一律**保持纯 ASCII**，说明文字放在本文件里。

2. **MSYS2 的 `g++` 需要 `C:\msys64\mingw64\bin` 在 PATH 上。**
   `cc1plus.exe` 位于 `lib\gcc\...`，却依赖 `mingw64\bin` 下的 `libgmp-10.dll`、`libmpfr-6.dll`
   等 DLL；该目录不在 PATH 时子进程静默启动失败，`g++` 退出码 1 且**零输出零报错**。
   `../cpp/build.cmd` 已固定处理这一点。
