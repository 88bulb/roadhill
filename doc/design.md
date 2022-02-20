# Design

## Naming Convention

很多restful api设计实践采用了[Zalando restful api guidelines](https://opensource.zalando.com/restful-api-guidelines/#json-guidelines)的命名方法，对本文档而言主要是：

- 使用`snake_case`命名json的property，例如`bulb_firmware`；
- 使用`UPPER_CASE`命名枚举类型的string，例如`DEVICE_INFO`；



## 生产模式和工程模式

系统启动后搜索特殊ap名称，如有名称内包含`roadhill_testing`字符的ap，则进入工程模式，否则进入生产模式。

- 在生产模式下，设备连接指定的ssid名称的ap（ssid名称和密码均保存在nvs内，不hardcode在代码里），然后tcp连接云服务器；
- 在工程模式下，设备连接含`roadhill_testing`字符的ap，并直接以gateway的`6015`端口作为模拟云的tcp连接工作；
- 两种情况下使用同样的tcp协议和http协议，其中tcp协议为长连接，http协议用于下载mp3文件，对于工程模式，本地模拟云的服务器（mock server）需要实现两种协议服务；
- 无论是生产模式还是工程模式，系统wifi（指wrover模块）都工作在sta+ap模式下，其中ap使用的ssid名称为`roadhill_${xxxxxx}`，其中`xxxxxx`是bulbboot协议中的group id；



## TCP协议与设备状态

从服务器的角度看，网关只有两种状态：`PLAYING`和`STOPPED`。

通讯上，不设计类似rpc的应答协议，但网关会持续向服务器报告自己的状态，在状态的定义里可以包含诸如`last_command`成员，记录最后收到的命令，和命令的执行结果，如果有错误，则记录在这里。

gateway也会报告自己的状态，是在播放还是已经停止，在播放状态下还可以有更细节的状态，例如播放的进度，下载缓冲的进度等等；如果服务器有需要可以利用这些信息更新客户端的显示。gateway报告自己的状态的时间间隔自己决定，但有最长时间限制；原则上在收到命令后，和出现重要状态变化时，例如缓冲达到一定容量开始播放时，gateway应尽快向服务器报告自己的状态；服务器可以发送`PING`命令要求gateway立刻报告状态，如果在规定时间内没有收到应答，可以视为gateway死机或者通讯故障；但`PING`命令主要用于开发测试，在生产环境中并无业务需求需要使用此命令。

服务器向gateway发送`PLAY`命令时不需要先检查或停止当前的状态，gateway接受在`PLAYING`状态下接受新的`PLAY`命令。

服务器向gateway发送的命令仅有4种：

1. OTA
2. CONFIG
3. PLAY
4. STOP

Gateway向服务器报告的信息有两种：

1. 设备信息
2. 状态信息

其中设备信息（`DEVICE_INFO`）仅在刚刚开始建立连接时报告一次，之后仅报告状态信息（StateInfo）；



### Handshake

1. 建立tcp连接后，gateway首先发送设备信息（DeviceInfo）；
2. 云向设备发送的第一个命令必须是`PREPARE`，包含组成员（灯）信息；此时云可以假设gateway的初始状态为`STOPPED`；
3. 云可以立刻向设备发送`PLAY`命令，该命令不意味着立刻开始播放，可以利用命令中的选项实现仅下载，或者满足成员数量要求时才开始播放；也可以仅循环播放暖场内容，避免在所有灯都开始工作前有较长等待；



### DEVICE_INFO

网关发送给云。`DEVICE_INFO`必须是网关连接云后第一个向云发送的消息。

```json
{
    "type":"DEVICE_INFO",
    "hardware":{
        "codename":"roadhill",
        "revision":"a0",
        "mac":"98:fa:9b:90:c6:17"
    },
    "firmware":{
        "version":"000100a1",
        "sha256":"babd8e3a91b7c78e549090ad9b9c2b8bf5ab315ae9f96c92ff833715665bb678"
    },
    "*accept_api_version": [1]
}
```

hardware属性描述硬件型号和版本；codename代表硬件类型，`roadhill`表示当前硬件的soundbar产品；revision原则上应该是通过硬件检测出来的，目前没有多硬件版本定义，revision缺省设置为`a0`。

firmware属性描述固件；其中version暂无特殊约定；sha256是乐鑫固件的bin文件的最后32个字节的内容，其值为去掉这32个字节的前面部分的sha256。



#### TODO

- (device) error report, such as emmc failure.
- api version support.



目前系统中还包含了c3模块，但c3模块的固件信息不放在`DeviceInfo`里定义了。





### STATE_INFO

待详细定义。

STATE_INFO





### OTA

云应该在收到网关的`DEVICE_INFO`命令之后通过其firmware信息判断网关固件是否需要升级，若需要升级应该立刻发送`OTA`命令。

网关目前假定`OTA`只能是第一个收到的命令，收到`OTA`命令后网关立刻中断tcp连接开始`OTA`升级，不再初始化其它任何功能组件；无论`OTA`成功失败网关都会重启。

这也意味着如果`OTA`命令中包含的固件无效或者下载有问题，网关会陷入循环的升级失败中，如果固件有文件完整性问题，反复升级重启会减少flash寿命。可能应该针对同一个固件做升级计数器；反复升级到一定次数后不再升级同一sha256值的固件。

`OTA`命令包含一个固件下载的URL，无需提供其它信息；该命令直接使用乐鑫提供的https ota库实现，如果需要使用https，需要提供PEM格式的服务器证书。

```json
{
    "api_version": 1,
    "cmd":"OTA",
    "*reply": "ERROR",
    "*reply_serial": 4635,
    "url":"http://10.42.0.1/files/roadhill.bin"
}
```



| 属性          | 含义       | 格式          | 值                                |                                         |
| ------------- | ---------- | ------------- | --------------------------------- | --------------------------------------- |
| api_version   | api版本    | integer       | 1                                 | 必须                                    |
| cmd           | 命令       | string (enum) | OTA                               | 必须                                    |
| *reply        | 是否应答   | string (enum) | FINISH, SUCCESS, ERROR, NONE etc. | 可选（default to NONE, if not provided) |
| *reply_serial | 应答的序号 | integer       |                                   | 如果reply!=NONE则应该提供reply_serial   |
| url           | url        | string        |                                   | 必须                                    |

标注（*）号的是建议，未确定实现；



### CONFIG

此命令目前未完成，请忽略本节内容。

`CONFIG`命令设置工作环境。

```json
{
    "api_version": 1,
    "cmd": "CONFIG",
    "bulb_firmware": {
        "url": "http://10.42.0.1/files/bulbcast.bin",
        "sha256": "21e831461f432ca453a3b9b97e0870c2807946f559d2574c5d81a571f08755fe"
    },
    "bulb_groups":[
        ["7a0764e689fb"],
        [],
        ["7a0764e6a70a", "7a0764e6a70a"],
        ["7a0764b68533"]
    ]
}
```

#### version

`version`指当前命令的JSON格式版本。因字段和设计假设较多，在出现breaking change时可以用`version`检查。

#### session_id

网关在汇报状态时，会提供当前的`session_id`，除此之外该属性无其它用途。网关不会关联`session_id`和文件cache，也不会持久化该属性，网关重启后云必须重新发送`SET_SESSION`命令。云可以用任何逻辑生成该字段，包括使用一次性随机uuid或类似web token方式加密一些信息。

#### bulb_firmware

所需的灯的固件信息，包括sha256（乐鑫固件格式）和下载固件的url。

#### bulb_groups

`bulb_groups`是一个array，每个element也是一个array。

每个element里包含0到N个灯的蓝牙地址字符串。element的index是这些灯的index，zero-based，对应的bitmask是`uint16_t(1 << index)`。

如果某个index没有element，可以使用`[]`占位。

允许给多个灯分配同样的index，这意味着这些灯都有同样的bitmask。有同样的bitmask的灯总是执行同样的指令，但指令中可能存在随机性差异化的效果。

例子中定义了3个组。`index=0`的组只有一个灯，对应的bitmask是`0x0001`；`index=1`的组没有灯；`index=2`的组包含两个灯，bitmask是`0x0004`；最后`index=3`的组只有一个灯，bitmask是`0x0008`。

#### tracks_url & tracks

下载mp3文件的url，和文件的md5值（hex格式）列表。其中md5值会在`PLAY`命令中继续使用，也会在网关向云报告session状态时使用。

网关下载track文件时合成完整url的方式为：

```js
`${tracks_url}/${track_md5}.mp3`
```

云提供的`tracks_url`不应该有`/`结尾（但有也不是问题）。

##### 设计说明

`.mp3`扩展名在这里不是一个很好的设计约定。它主要的方便是可以直接使用http的静态文件服务，尤其是开发调试时。但如果使用对象存储或遵循restful设计习惯时不必有这个扩展名约定，使用http stream里的MIME type是更灵活的方式，未来也不一定一直使用mp3格式。

##### 几种常用Digest比较

| -      | digest size in bits | digest size in bytes | digest size in hex string | comment     |
| ------ | ------------------- | -------------------- | ------------------------- | ----------- |
| md5    | 128 bits            | 16 bytes             | 32 characters             |             |
| sha1   | 160 bits            | 20 bytes             | 40 characters             | used in git |
| sha256 | 256 bits            | 32 bytes             | 64 characters             |             |



### PLAY

标注为*的属性目前不支持，时间单位全部为ms（milli-second）。

```json
{
    "api_version": 1,
	"cmd": "PLAY",
    "*reply": "ERROR",
    "*reply_serial": 7625,
    "tracks_url": "http://10.42.0.1/files/album0001",
    "tracks": [
        {
            "name": "43a5155e9d3772406fb51b9fb3c5e668",
            "size": 12345,
            "start": 0,
            "dur": 60000,
            "chan": 0,
        },
        {
        	"name": "972f619d7f82864a3b11b0e7b37d993e",
            "size": 4321,
            "start": 8000,
            "dur"
            "chan": 1
        }
    ],
    "blinks": [
        {
            "time": 0,
            "mask": "00ff",
            "code": "100300000003c680c6f0fa33f0fafa"
        },{
            "time": 1000, 
            "mask": "00ff",
            "code": "100300000003c680c6f0fa33f0fafa"
        }
    ],
    "*start": "immediate"
}
```

#### ap_version

命令格式版本，固定为1。

#### cmd

字符串，对PLAY命令，固定为`PLAY`。

#### tracks_url

字符串，下载地址，需包含`https://`协议前缀，不应有`/`结尾。

#### tracks

需要播放的音乐文件列表，含如下属性：

| -     | -                      | -    | -                                            |
| ----- | ---------------------- | ---- | -------------------------------------------- |
| name  | 32字符的hex code字符串 | 必须 | 声音文件名称，也是其md5值                    |
| size  | 正整数                 | 必须 | 声音文件大小                                 |
| start | 正整数                 | 必须 | 开始播放时间，单位ms                         |
| dur   | 正整数                 | 必须 | 播放长度，注意是播放长度，不是文件的音频时间 |
| chan  | 通道                   | 必须 | 0为背景，1为前景                             |



下载时网关默认给文件加上`.mp3`扩展名，例如上述例子中的`tracks[0]`的下载地址会解释成：

`http://10.42.0.1/files/album0001/43a5155e9d3772406fb51b9fb3c5e668.mp3`

~~网关仅会立刻播放`tracks[0]`指定的音乐文件，并且不会在播放停止后自动开始播放下一个音乐文件，云服务器必须再次发送PLAY命令并且把需要播放的音乐文件设为`tracks[0]`~~

网关会把所有track播放完。



#### blinks

完整的bulbcode灯码是26字节（52个hex char），例如：

`b01bc0de00a5a5a5a500ff100300000003c680c6f0fa33f0fafa`

PLAY命令中不包含magic（`b01bc0de`，4字节)，sequence number（1字节），Group ID（4字节）。Group ID默认使用网关mac地址的最后四个字节。

blinks数组对象需提供剩下的17字节（34个hex char），包括前面2个字节的`mask`，和剩余15字节的`code`，如例子所示。

时间单位固定使用毫秒（msec），不增设属性描述。



#### start

当前支持`immediate`（立即开始播放），和`none`（不播放）。设置为立即开始播放不要求track已经下载完成，网关会尽可能选择尽可能早开始的播放时间。



### STOP

```json
{
	"cmd": "STOP"
}
```

停止当前播放，但不意味着停止所有下载。



## Roadhill Internal Design

### FreeRTOS Tasks, pipeline

程序结构上用FreeRTOS任务划分，包括：

1. main task；
2. http ota，负责ota升级（a/b升级）；(完成)
3. tcp receive，负责接收和解析云命令；（大部分完成）
4. tcp send，负责向云发送信息；（有框架无内部实现，用于发送状态或错误
5. juggler，juggler是资源和子任务的分发器；（正在做）
6. fetcher，fetcher是下载媒体文件的下载器，fetcher是动态任务，有生命周期；（正在做）；
7. cache，cacher负责存储和提取文件cache；（未开始）；
8. audible，audible是音频播放任务，使用audio_pipeline工作；（未开始）；



```
tcp_receive -> audible -> juggler -> [0..n] Fetcher
```

### Control and Data Flow, PLAY, REPLAY, and STOP

#### PLAY, tcp_receive -> audible

`tcp_receive`收到`PLAY`指令后构建通过音频消息接口的消息，使用`periph_cloud_send_event`发送，其中`void* data`参数发送`play_context_t`对象，

```c
struct play_context {
    uint32_t index;
    uint32_t reply_bits;	/* reserved */
    uint32_t reply_serial;	/* reserved */
    char* tracks_url;
    int tracks_array_size;
    track_t *tracks;
    int blinks_array_size;
    blink_t *blinks;
}

struct play_data {
    uint32_t index;
    char *tracks_url;
    int tracks_array_size;
    track_t *tracks;
}
```

memory management

- play_context

  - allocated by tcp_receive (in process_line);
  - audible一直保留最后一个play_context，在收到新的play_context时，旧的被回收（handling PERIPH_CLOUD_CMD_PLAY);

- tracks and tracks_url

  - tracks和tracks_url是可选的，如果不存在，这两个值均为NULL且tracks_array_size为0;
  - allocated by tcp_receive
  - audible仅维护收到的play_context和blinks，不维护tracks和tracks_url；在收到play_context时，audible即时创建一个新的play_data结构，**移动**相应的内存资源到新的结构体发送给juggler; 无论收到的play_context是否包含tracks，audible都会发送play_data给juggler，回收相应的资源责任交给了juggler；

- blinks

  - blinks是可选的，如果不存在，该值为NULL且blinks_array_size为0;
  - allocated by tcp_receive
  - audible的主任务上下文不需要保存blinks，index+blinks应该发送到audio_queue，

  









灯的播放详细设计还没有完全完成。灯谱包含在PLAY消息内，该消息JSON解析后的数据结构完整传递给juggler，juggler传递给audible。应该是先跑通音频下载播放任务之后，



### Juggler

Juggler的资源和状态是程序的核心部分，简化的设计是Juggler拥有全部资源。资源应该针对task私有化，如果担心资源的初始化有跨task的等待，可以使用event group实现。



#### Resources

##### `play_context_t`, struct and object

Since there is no class in C, using `object` as the term for the instance of `struct` is appropriate.

There are **two** objects of type `play_context_t`. This is enough for one is used by juggler and the other used for storing result parsed from json in `tcp_receive` task. They are stored in a freertos Queue, which is used as a thread-safe FIFO. There is no need to have global variables or array of variables to hold the pointer.

- When the object is allocated? in tcp_receive then passed to juggler.
- When the object is deallocated (put back to queue)? juggler always hold a current_play_context, it is replaced only when another one arrives. 



play context may or may not have a track to play, if it have track to play, it's going to have a file resource, with a filename derived from tracks[0] and a corresponding possibly a fetcher task associated. 



每个音频文件使用一个缓冲文件在esp32上不是一个好办法，因为在需要管理时遍历文件系统非常慢，不如自己写index file和在大文件内直接分配。但是在开发阶段，这个不同实际上是可以封装掉的。最终Play过程是通过接口实现文件查询和读写。如果使用临时文件缓冲，例如在内存中建立一次性的映射表或者在emmc上使用有限数量的文件记录cache，在这种设计下，每个文件的FILE指针（fp）可以记录在play_context_t内。



##### fetch_context_t, struct and object

fetch_context_t stores context of a fetch task. a fetch task may be detached from a play process.

- When the object is allocated?
- When the object is deallocated?



##### mem_block_t, struct and object







1. `tcp_receive`能创建的`PLAY`资源；原则上，`tcp_receive`应该request一个`play_context_t`资源，`play_context_t`的动态资源分配应该一次性操作和保证all or none，因此`play_command_data_t`只是过渡设计；
2. 下载器需要的内存块；
3. 播放器需要的内存块，两者都是固定大小，需要包含字段描述实际大小；



Juggler可以使用下载器和cache文件播放，或仅使用cache文件播放。



jjjjkkkj

取消操作（`STOP`）

对下载器而言，取消操作采用等到settled方式，对播放器而言可以不做特殊处理，也可以抢先（指在队列前塞入STOP语义）。



发送给fetcher的消息包括fetch_more（包含mem_block_t）和fetch_abort；前者包含内存块。

```C

```

fetch_more



fetch返回的message包括data_fetched（包含mem_block_t），fetch_error（包含mem_block_t），fetch_finish（包含mem_block_t）；其中







juggler在play任务









`juggler`维护一组文件（64-256个）。每个文件内包含一个数据块，一个`header`，包含数据块的描述，和一个digest，表示文件的完整性。

`juggler`使用这些文件完成如下任务：

1. 让`fetcher`下载音频文件，一块一块的存储在数据块文件中，还给`juggler`；`fetcher`是写入者。
2. 把数据块文件交给`audible`播放，工作在只读模式。
3. 把一组数据块文件交给`cache`，写入cache文件，工作在只读模式。

原则上`juggler`自己并不读写文件。一个例外的优化是`juggler`在启动时检查这些文件，如果发现有完整的文件尚未cache，可以交给cache。但没有这个优化不影响设备正常工作。

#### 通讯

`juggler`使用`juggler_queue`接收消息；`juggler`接收的消息包括：

1. 来自`tcp_receive`的命令，CONFIG， PLAY， STOP；
2. 来自`fetcher`的返回，包括数据块文件，结束命令；从fetcher来的消息可以看作流式的emitter；
3. 来自`cache`的返回，可以是数据块文件，也可能是内存块，或者命令返回结果；









数据块以256K为单位，存放数据块的文件以320K大小，因为文件系统使用64k（64 * 1024）大小的簇。

文件使用md5标记其完整性



first 1K (first 16byte ) 

header

总文件的md5（16字节）+ 总文件的大小（4字节）+ 当前块的位置（4字节）+ 当前块的大小（4字节）=512字节

```c
typedef struct {
    uint8_t bytes[16];
} md5_digest_t;

typedef struct {
    md5_digest_t 	file_digest;
    uint32_t		file_size;
    uint32_t		chunk_index;
    uint32_t		chunk_size;
} data_chunk_header_t;
```





写入顺序

1. 先擦除最后16字节，flush；
2. 从0K处开始写入文件内容，最大256KB（256 * 1024）；
3. 从1K处写入Header
4. 



juggler发出的message

给fetcher的

下载的url，总文件hash，文件大小，一定数量的data chunk file；







### Fetcher

Fetcher是唯一的动态任务，由Juggler创建，在结束时销毁。

Juggler和Fetcher使用[FreeRTOS Queue](https://www.freertos.org/Embedded-RTOS-Queues.html)通讯，Fetcher使用的Queue也是动态创建的。



| 资源                  | 资源内容                                              | 创建者  | 销毁者  |      |
| --------------------- | ----------------------------------------------------- | ------- | ------- | ---- |
| Fetcher Task          | FreeRTOS task                                         | Juggler | Juggler |      |
| Fetcher's recv buffer | memory                                                | Fetcher | Fetcher |      |
| Fetcher's Queue       | FreeRTOS queue                                        | Juggler | Juggler |      |
| Fetch Config          | struct，包含queue, url, digest, (possibly) file size. | Juggler | Juggler |      |









## Communication and Message Data Structure



## Audible



Audible是一个drain模式；播放开始于Juggler第一次给audible_queue发送chunk。





