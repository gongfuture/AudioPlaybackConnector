
<!-- markdownlint-disable -->

复刻自[ysc3839/AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector)

[English](https://github.com/gongfuture/AudioPlaybackConnector/blob/master/README.md) | **简体中文**

[Releases](https://github.com/gongfuture/AudioPlaybackConnector/releases)

## 鸣谢
- 感谢 [ysc3839](https://github.com/ysc3839) 创建并开源了 [AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector)，使得我能够参考其实现并复刻该项目。
- 感谢 [dmagician038](https://github.com/dmagician038) 的 [复刻](https://github.com/dmagician038/AudioPlaybackConnector)，从中整合功能 防止多个实例同时运行
- 感谢 [kb5000](https://github.com/kb5000) 的 [复刻](https://github.com/kb5000/AudioPlaybackConnector)，从中整合功能 配置登陆时自动运行
- 感谢 [hmkim123](https://github.com/hmkim123) 的 [复刻](https://github.com/hmkim123/AudioPlaybackConnector)，从中整合功能 连接时改变托盘颜色以及bug修复
- 感谢 [lkk9898969](https://github.com/lkk9898969) 的 [复刻](https://github.com/lkk9898969/AudioPlaybackConnector)，从中整合功能 应用启动后通知
- 感谢 [MoroccoBS](https://github.com/MoroccoBS) 的 [复刻](https://github.com/MoroccoBS/AudioPlaybackConnector)，从中整合功能 ~~音频路由和设备管理~~以及CI更新

> 音频路由和设备管理一开始整合了，但是后来发现用不着，直接在控制面板的声音设置，改对应设备的监听输出设备就行，所以就没整合

## 功能
除上述复刻项目的功能外，还制作了以下功能：
* 应用启动通知可选

只在Windows 11 23H2上测试过（因为我用的是WIn11），不保证其他版本的可用性。
其他版本遇到的问题可以提issue，但是不保证能复现和修复。

## 截图
![通知](https://cdn.jsdelivr.net/gh/gongfuture/AudioPlaybackConnector@master/assets/通知.png)
![托盘图标](https://cdn.jsdelivr.net/gh/gongfuture/AudioPlaybackConnector@master/assets/托盘图标.png)
![右键菜单](https://cdn.jsdelivr.net/gh/gongfuture/AudioPlaybackConnector@master/assets/右键菜单.png)
![重复实例警告](https://cdn.jsdelivr.net/gh/gongfuture/AudioPlaybackConnector@master/assets/重复实例警告.png)

## 疑难杂症解决
参考[MoroccoBS](https://github.com/MoroccoBS)的[复刻](https://github.com/MoroccoBS/AudioPlaybackConnector)时，发现 Windows11 找不到 A2DP Sink 设备的问题，解决方法如下：
1. 打开注册表编辑器（Win + R，输入 regedit，回车）
2. 导航到 `\HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\`
3. 找到你的 A2DP Sink 设备对应的条目（可以一项项翻子项`Properties`中的设备名称）
4. 删除对应设备名为 `{9c119480-ddc2-4954-a150-5bd240d454ad},6` 的键

这个时候再打开控制面板的声音设置，应该就能看到 A2DP Sink 设备了。
参考来源：[bluetooth a2dp snk device not showing up in Sound since Windows 11 - Microsoft Q&A](https://learn.microsoft.com/en-us/answers/questions/4123061/bluetooth-a2dp-snk-device-not-showing-up-in-sound)

## ↓以下是源项目 README 内容↓

# AudioPlaybackConnector
[English](https://github.com/ysc3839/AudioPlaybackConnector/blob/master/README.md) | **简体中文**

Windows 10 2004+ 蓝牙音频接收 (A2DP Sink) 连接工具。

微软在 Windows 10 2004 加入了蓝牙 A2DP Sink 支持。但是需要第三方软件来管理连接。\
已经有了一个可以实现此功能的 app。但是它不可以隐藏到通知区域，而且不开源。\
所以我写了这个 app，提供一个简单的，现代且开源的替代品。

# 预览
![预览](https://cdn.jsdelivr.net/gh/ysc3839/AudioPlaybackConnector@master/AudioPlaybackConnector.gif)

# 使用方法
* 从 [releases](https://github.com/ysc3839/AudioPlaybackConnector/releases) 下载并运行 AudioPlaybackConnector。
* 在系统蓝牙设置中添加蓝牙设备。你可以右键点击通知区域的 AudioPlaybackConnector 图标然后选择“蓝牙设置”。
* 点击 AudioPlaybackConnector 图标然后选择想要连接的设备。
* 尽情享受吧！
