=============================================================================
 mgpu ReShade add-on 部署包
=============================================================================

这个包把「游戏里需要的那一半」装进游戏目录，让 mgpu 的 bridge 能拿到
颜色 / 深度 /（可选）运动矢量：

    ReShade 6.8 (dxgi.dll)  ->  宿主，负责把 add-on 加载进游戏进程
    game_capture.addon64    ->  真正做抓取的那一半（共享内存 producer）
    game_capture.ini        ->  它的配置（Color=0 / StreamPercent=100 / SkipMv=1）
    mgpu_depth_tap.fx       ->  一个空转效果，唯一作用是让 ReShade 绑定游戏深度
    ReShadePreset.ini       ->  启用上面那个效果
    ReShade.ini             ->  shader 搜索路径 + 预设路径

------------------------------------------------------------------------------
 安装
------------------------------------------------------------------------------
方式一（最省事）：把游戏的 .exe 文件拖到 install.bat 上松开。

方式二：install.bat "D:\Games\XXX\bin\x64"

方式三：双击 install.bat，按提示粘贴游戏目录。

  注意：目录要**直接包含游戏的 exe**，不是上一级。
        例： Cyberpunk 2077\bin\x64
        PBZ 一般是 ...\PBZ\platform\steam

  如果游戏用的是 dxgi 以外的入口（少数老引擎），加第二个参数换代理名：
        install.bat "D:\Games\XXX" d3d11
        可选：dxgi（默认）| d3d11 | d3d12 | opengl32 | version

  目标 DLL 已存在时会先备份成 <名字>.dll.mgpu_bak，不会直接覆盖丢东西。

------------------------------------------------------------------------------
 装完之后
------------------------------------------------------------------------------
1. 启动游戏。第一次 ReShade 会在左上角显示加载信息，按 Home 可以打开面板
   （面板里 Effects 列表应该能看到 MGPU_DepthTap，并且它是勾上的）。
2. 游戏内设置：关掉帧生成；抗锯齿选 DLSS（不要 TSR / FSR）。
   —— add-on 是从 DLSS 的 NGX 上报里拿渲染分辨率，没有 DLSS 就拿不到。
3. 打开 mgpu_ui.exe（在 tools 目录）选这个游戏的窗口，按热键开始；
   或者命令行：bridge.exe --game <游戏exe文件名> ...

------------------------------------------------------------------------------
 卸载 / 排错
------------------------------------------------------------------------------
卸载：uninstall.bat "D:\Games\XXX\bin\x64"
      会把 add-on、ini、shader、预设删掉，并把备份的 DLL / ReShade.ini 还原。

add-on 没生效的最常见三个原因：
  1. 没有一个启用的效果 —— ReShade 只在有启用效果时才会调 add-on 的
     begin_effects。检查 ReShadePreset.ini 里有没有
         Techniques=MGPU_DepthTap@mgpu_depth_tap.fx
  2. 游戏没走 ReShade —— 看游戏目录有没有生成 ReShade.log；
     没有就是代理 DLL 名字不对（换 d3d11/d3d12 再试）。
  3. 游戏开着的时候装的 —— 那时 DLL 被占用，写入会失败。先关游戏。

日志：
  ReShade.log            ReShade 自己的日志（add-on 加载失败会写在这里）
  game_capture.log       add-on 的日志 **默认不生成**：把 game_capture.ini 的 Log 改成 1
                         （或设环境变量 MGPU_CAPTURE_LOG=1）才写

------------------------------------------------------------------------------
 兼容性
------------------------------------------------------------------------------
  - 只支持 64 位游戏（add-on 是 .addon64，宿主是 ReShade64）。
  - ReShade 版本钉在 6.8.0：add-on 的 API/ABI 必须和宿主一致，
    换 ReShade 大版本要重新编译 add-on。
  - 需要 Windows 10 1809 以上（WGC 抓帧 + DirectComposition 的要求）。
