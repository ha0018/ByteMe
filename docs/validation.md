# 本机验证记录

日期：2026-10-13。

环境：Windows 11 x64、Visual Studio 2022 / MSVC 19.44.35229、Qt 6.7.3 MSVC x64、Release 构建。

## 结果

- CMake 配置、MSVC 编译和链接成功。
- QtTest：21 项通过，0 项失败；包含初始化与结束检查。原始输出保存在 [test-results.txt](test-results.txt)。
- 设置 `QT_FATAL_WARNINGS=1` 后测试全部通过，未出现 Qt 运行时警告。
- 真实本机 IPv4 TCP / UDP：设备探测和单播回复、中文文件、空文件和二进制文件、并行排队、同名文件另存、重启两端后续传、损坏片段重传、拒绝接收、非法协议输入、截断文件和完整校验失败、活动传输安全退出均通过。
- `windeployqt` 部署 Qt DLL、Windows 平台插件和 VC 运行库安装程序。
- 清除 PATH 中的 Qt 路径以及 Qt 插件环境变量后，启动 `dist/ByteMe.exe`，正常生成 [界面截图](screenshots/main-window.png)，进程退出码为 0。
- 生成 `release/ByteMe-windows-x64.zip`，解压到新的目录后，在同样没有 Qt 环境路径的条件下再次启动成功，退出码为 0。

## 实际网络验证范围

本轮在同一电脑的回环网络测试；没有两台实体电脑的跨机器局域网结果。UDP 网卡广播、防火墙策略、不同磁盘和大型文件长期传输需要在使用现场继续验证。

## 复现

```powershell
.\scripts\build.ps1 -QtRoot "D:\Qt6\6.7.3\msvc2019_64" -Configuration Release -Deploy
```

测试源文件为 `tests/integration.cpp`，使用实际 socket、临时目录和磁盘文件，续传用例断开并销毁两个 manager 后重新创建，检查首个传输偏移等于磁盘上保留的字节数，并逐字节核对最终文件。
