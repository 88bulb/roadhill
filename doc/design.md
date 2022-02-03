# Design

## 生产模式和工程模式

系统启动后搜索特殊ap名称，如有名称内包含`roadhill_testing`字符的ap，则进入工程模式，否则进入生产模式。

- 在生产模式下，设备连接指定的ssid名称的ap（ssid名称和密码均保存在nvs内，不hardcode在代码里），然后tcp连接云服务器；
- 在工程模式下，设备连接含`roadhill_testing`字符的ap，并直接以gateway的`6015`端口作为模拟云的tcp连接工作；
- 两种情况下使用同样的tcp协议和http协议，其中tcp协议为长连接，http协议用于下载mp3文件，对于工程模式，本地模拟云的服务器（mock server）需要实现两种协议服务；
- 无论是生产模式还是工程模式，系统wifi（指wrover模块）都工作在sta+ap模式下，其中ap使用的ssid名称为`roadhill_${xxxxxx}`，其中`xxxxxx`是bulbboot协议中的group id；



## TCP协议与设备状态

从服务器的角度看，gateway只有两种状态：`PLAYING`和`STOPPED`。通讯上，不设计类似rpc的应答协议，但gateway会持续向服务器报告自己的状态，在状态的定义里可以包含诸如`last_command`成员，记录最后收到的命令，和命令的执行结果，如果有错误，则记录在这里。

gateway也会报告自己的状态，是在播放还是已经停止，在播放状态下还可以有更细节的状态，例如播放的进度，下载缓冲的进度等等；如果服务器有需要可以利用这些信息更新客户端的显示。gateway报告自己的状态的时间间隔自己决定，但有最长时间限制；原则上在收到命令后，和出现重要状态变化时，例如缓冲达到一定容量开始播放时，gateway应尽快向服务器报告自己的状态；服务器可以发送`PING`命令要求gateway立刻报告状态，如果在规定时间内没有收到应答，可以视为gateway死机或者通讯故障；但`PING`命令主要用于开发测试，在生产环境中并无业务需求需要使用此命令。

服务器向gateway发送`PLAY`命令时不需要先检查或停止当前的状态，gateway接受在`PLAYING`状态下接受新的`PLAY`命令。

服务器向gateway发送的命令仅有4种：

1. PREPARE
2. PLAY
3. STOP
4. QUIT

Gateway向服务器报告的信息有两种：

1. 设备信息
2. 状态信息

其中设备信息（DeviceInfo）仅在刚刚开始建立连接时报告一次，之后仅报告状态信息（StateInfo）；



### 握手

1. 建立tcp连接后，gateway首先发送设备信息（DeviceInfo）；
2. 云向设备发送的第一个命令必须是`PREPARE`，包含组成员（灯）信息；此时云可以假设gateway的初始状态为`STOPPED`；
3. 云可以立刻向设备发送`PLAY`命令，该命令不意味着立刻开始播放，可以利用命令中的选项实现仅下载，或者满足成员数量要求时才开始播放；也可以仅循环播放暖场内容，避免在所有灯都开始工作前有较长等待；



### DeviceInfo

gateway -> cloud

```json
{
    "hardware": {
    	"codename": "roadhill",
        "revision": "a0"
	},
    "firmware": {
        "version": "01000000"
    }
}
```

codename代表了硬件类型；revision原则上应该是通过硬件检测出来的，目前没有多个硬件版本定义，revision缺省设置为`a0`。

目前系统中还包含了c3模块，但c3模块的固件信息不放在`DeviceInfo`里定义了。



### StateInfo

gateway -> cloud

（待定）



### PREPARE

cloud->gateway

```json
{
    "cmd": "prepare",
    "firmware": {
        "url": "http://xxx",
        "size": 123456,
        "version": "01000000",
    },
    "bulbs":[
        ["7a0764e689fb", "7a0764e6a70a"],
        [],
        ["7a0764b68533"],
        []
    ]
}
```

### PLAY

cloud->gateway

```json
{
	"cmd": "play",
	"audio": [
        {
            "url": "http://",
            "digest": "jkljkljl",
            "size": 123456
        },
        {
            "url": "http://",
            "digest": "jkljkljl",
            "size": 127878            
        }
    ],
    "light": [
        [10, "code"]
    ]
}
```

### STOP

cloud->gateway

```json
{
	"cmd": "stop"
}
```

停止当前播放（但未定义是否停止下载）

### PING (optional)

cloud->gateway

```json
{
    "cmd": "ping"
}
```



### QUIT

```json
{
	"cmd": "quit"
}
```

会立刻停止播放和让所有的灯重启。



## 实现

和`bulbboot`一样，`roadboot`项目负责升级和启动`roadhill`固件，`roadhill`固件只负责下载播放功能。



## roadboot

本节内容无效，即将merge进roadhill，改用乐鑫官方的a/b升级方式。



`roadboot`目前采用最小化设计，仅idf项目（即不包含音频功能），也不处理按键和C3模块，这些都是`roadhill`功能。



`roadboot`仅工作在wifi sta模式下，启动后通过扫描，寻求连接ssid包含`roadhill_test`或`roadhill_prod`（本文档中最终连接的ssid名称和密码均使用`roadhill_prod`代替）的ap，如果发现则使用和token相同的字符串做密码去连接。



连接成功后，使用`GET`访问预定义的http服务，通过query string提交设备的型号（`juwanke-gateway-speaker-01`），硬件版本号（`a1`），firmware版本（`01000000`），设备id（使用mac），获得返回的json。

```json
{
    "url":"http url",
    "size": 123456,
    "sha256": "the last 32 bytes in hex code"
}
```



如果成功获得该返回结果，`roadboot`比对当前的ota1固件的hash是否一致，如果不一致则下载指定固件安装，否则启动该固件。



如果连接的是本地的服务，即`roadhill_test`的gateway，boot url使用：

```
GET http://<gateway ip>/gateway/boot?model=juwanke-gateway-speaker-01&rev=a1&fw=01000000&mac=1234567890ab
```



**TODO**

- [ ] 应该有声音播放反馈；
- [ ] 生产环境的HTTP的URL定义；
- [ ] 应实际校验固件，因为有可能在刷入新固件过程中，未能擦除旧固件的hash，如果因为某种原因云决定回卷到旧固件，如果没有校验只是读取了hash后启动，则可能陷入死循环；



## roadhill

