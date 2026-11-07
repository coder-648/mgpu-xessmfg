# 报告

0. 先补充一个信息, 使用时AMD有正常负载(浏览器+vscode), 3080ti只在游戏时有负载, 正常状态下占用为0
1. 步骤A执行完成, 输出日志在"<TOOLS_DIR>\step1log"
2. 步骤C执行完成, 输出日志在"<TOOLS_DIR>\step2log"
3. 关于步骤D, lossless scaling在alt+tab切出后会停止运行, 但我还是跑了一次, 日志在"<TOOLS_DIR>\step3log", 可以给脚本加一个延迟, 启动脚本后我进行副卡fg, 应该可以测试出正确数据
4. 2077设置 5k质量档 全高 场景在光追大道, 输出40hz, 显卡占用90%