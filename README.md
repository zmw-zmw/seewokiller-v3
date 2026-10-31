# SeewoKiller v3（远程管理版）

教室希沃一体机批量管理工具，功能限定为远程管理三件套：

- **批量命令**：SSH 批量执行（关机 / 脱控 / 被控 / 重置VNC 插件 + 自定义命令）
- **文件传输**：SFTP 批量上传到全部 / 从首台成功机器拉取
- **远程屏幕**：一键 VNC（内嵌 noVNC，WebSocket 代理）

## 构建

环境：Visual Studio（MSVC + WinSDK）、CMake ≥ 3.21、vcpkg（`libssh` + `webview2`）、Inno Setup 6。

```
cmake -B build -DVCPKG_ROOT=<你的vcpkg installed路径>
cmake --build build --config Release
```

产物在 `build\Release\`，用 `setup.iss` 可打安装包。

## 说明

- 默认并发 8 路，对教室网段友好；批量规模大时耐心等待即可。
- IP 输入支持 `172.16.20.6-12` 短范围写法。
- 使用范围限于本校内你拥有管理权限的设备。
