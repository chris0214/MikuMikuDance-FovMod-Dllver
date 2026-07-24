# MMD Float FOV Mod

面向 MikuMikuDance x64 的独立 Float FOV DLL。它把相机 VMD 的 `Distance`
浮点值作为 FOV，直接随 `MMEffect.dll` 加载，不需要 Cheat Engine 或注入器。

## 功能

- 支持 MikuMikuDance 9.26 x64 和已分析的 9.31 x64 布局。
- 暂停视口、播放和 AVI 输出使用一致的浮点 FOV。
- 每帧渲染期间临时将运行时相机距离设为零，渲染结束后恢复原始载体。
- MME、阴影、Billboard 和相机相关绘制共享同一相机状态。
- 按当前 viewport 宽高比刷新透视投影，同时保留 MMD 原有深度项。
- 正交模式绕过零距离事务，未知 EXE 布局拒绝安装补丁。

## 目录

```text
.
|-- MMEffect.dll                 预编译的 x64 Release DLL
|-- CMakeLists.txt               独立构建入口
|-- include/                     MMD/MME ABI 头文件
|-- src/                         Forwarder 与 Float FOV 补丁源码
|-- tests/                       补丁编码和事务测试
|-- README.md                    开发与开源说明
`-- README.txt                   面向最终用户的安装说明
```

本项目不包含 MikuMikuDance、原版 `MMEffect.dll`、MME 本体或测试用 MMD
可执行文件。

## 安装

1. 完全关闭 MikuMikuDance。
2. 将 MMD 目录中的原版 `MMEffect.dll` 政名为
   `MMEffect.original.dll`。
3. 把本项目的 `MMEffect.dll` 放进同一目录。
4. 正常启动 MMD，不要同时启用旧 Float FOV Cheat Engine 表。

成功后，同目录的 `MMEffect.forwarder.log` 应包含：

```text
Float FOV patch installed: error=none profile=MMD 9.26 x64
```

退出时正常的事务摘要类似：

```text
viewport transaction: active=0 starts=322 restores=322 stale=0
```

`starts` 与 `restores` 应相等，`stale` 通常应为零。

## 使用镜头 VMD

在 MMD 中切换到“相机/照明/附件”模式，然后拖入 Float FOV 相机 VMD，
或使用“文件 -> 载入动作数据”。`Distance` 是 FOV 数据载体，不要把它当作
普通相机距离重新修改。有效透视 FOV 范围为 `0.01` 到 `179.9` 度之间。

## 构建

要求：

- Windows x64
- CMake 3.20 或更高版本
- Visual Studio C++ 工具链和 Windows SDK，或 MinGW-w64

Visual Studio 示例：

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Ninja 或其他单配置生成器需要在配置阶段指定 Release：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

生成的 DLL 位于 `build/Release/MMEffect.dll`。使用单配置生成器时通常位于
`build/MMEffect.dll`。

测试程序默认执行不依赖 MMD 文件的合成测试。也可以手动验证真实 EXE：

```powershell
build/Release/float_fov_patch_fixture.exe `
  "C:\path\to\MMD-9.31\MikuMikudance.exe" `
  "C:\path\to\MMD-9.26\MikuMikudance.exe"
```

## 实现概要

补丁先校验七处机器码签名，再安装相对跳转。透视视口渲染入口会保存原始
`Distance` 位模式，设置浮点 FOV，并在本帧内将运行时距离置零。下游 MME
完成 `OnEndScene` 后恢复原始位模式。Lost/Reset/Cleanup 和下一帧入口均有
兜底恢复逻辑。

MME 的 11 个导出回调会被完整转发到同目录的
`MMEffect.original.dll`。可选的 `MmdTimelineExtension.dll` 和
`MmdUiExtension.dll` 不存在时会被自动跳过，不影响 Float FOV 功能。

## 兼容性与风险

本项目依赖特定 MMD x64 机器码布局。即使版本号相同，不同来源或被修改过的
EXE 也可能不匹配；此时补丁会保持 fail-closed，不修改宿主代码。发布新版本
支持前，应先增加完整签名配置和真实 EXE 夹具验证。

## License

当前目录尚未选择开源许可证。正式发布前请添加 `LICENSE` 文件；未明确授予
许可证时，源码默认不构成对复制、修改或再分发的授权。
