# RTMP 流程复习：从握手到音视频转发

适用范围：本项目的简单握手、AMF0 命令和传统 H.264/AAC 媒体路径。本文区分“协议含义”和“当前实现”，不把教学代码中的简化当成协议要求。

## 0. 忘了细节，先查这里

| 想回忆的问题 | 看哪里 | 一句话提示 |
|---|---|---|
| TCP 都连上了，为什么还握手？ | §1 | TCP 管字节传输，RTMP 管应用协议初始化 |
| C0/C1/C2 各发什么？ | §2 | 0 是版本，1 是时间与随机数据，2 是对对方 1 的回应 |
| 缓冲区怎么算够不够？ | §3 | 解析看可读数据；接收看可写空间 |
| 一个消息怎么拆成块？ | §4 | 每块最多携带 Chunk Size 字节消息体 |
| fmt、CSID、MSID 怎么区分？ | §5 | 头格式、分块通道、逻辑消息流 |
| 00 03 E8 为什么是 1000？ | §6 | 大端：0×65536 + 3×256 + 232 |
| connect 后发生什么？ | §7 | 设置控制参数 → createStream → publish/play |
| 元数据、序列头、关键帧有什么不同？ | §8 | 描述信息、解码配置、独立解码起点 |
| 在代码里从哪里追？ | §9 | OnRead → Parse → HandleMessage → Session |
| 哪些实现不能直接当标准？ | §10 | 容量检查、扩展 CSID、时间戳、元数据路径等 |

完整主线：

```text
TCP 建连
  → C0C1 / S0S1S2 / C2
  → connect(app)
  → 控制参数 + connect 成功应答
  → createStream → 返回 MSID
  → publish(streamName) 或 play(streamName)
  → 状态通知
  → 元数据、编码配置、音视频消息
  → 关闭流或断开连接
```

注意：握手结束后的命令、控制信息和媒体数据，都使用 Message → Chunk 的封装方式。不是发完命令才开始分块。

## 1. 三个层次别混在一起

| 层次 | 负责什么 | 不保证什么 |
|---|---|---|
| TCP | 可靠、有序地传输字节 | 一次 recv 恰好等于一个协议消息 |
| RTMP Chunk | 分块、头部压缩、按 CSID 重组 | 视频能直接解码 |
| RTMP Message | 表达命令、控制、音视频、元数据 | 一条消息一定只占一个 Chunk |

TCP 握手由网络栈处理；RTMP 握手由程序或协议库处理。握手不提供应用名、流名称，也不等于认证或已经开始播放。

## 2. 握手：双方每一步交换什么

### 2.1 本项目采用的顺序

```text
客户端（推流端或播放器）                     服务端
    |                                          |
    |---- C0(1) + C1(1536) -------------------->|
    |                                          | 检查版本、生成响应
    |<--- S0(1) + S1(1536) + S2(1536) ----------|
    | 检查版本，准备 C2                        |
    |---- C2(1536) ---------------------------->|
    |                                          | 握手完成
    |---- 后续 RTMP 消息 ---------------------->|
```

括号中是字节数。这里的 C/S 表示客户端/服务端；播放器也是客户端。

协议允许握手包按依赖关系分别发送，不要求一次 socket 写入就包含图中整组数据。客户端发 C2 前须收到 S1，发后续数据前须收到 S2；服务端发 S2 前须收到 C1，发后续数据前须收到 C2。本项目把服务端响应合成 3073 字节，并在客户端收齐整组后处理。

### 2.2 C0 和 S0：各 1 字节

```text
03
└─ RTMP 版本号 3
```

当前代码检查 `buf[0] != 3` 就返回 -1。

### 2.3 C1 和 S1：各 1536 字节

```text
| Time：4 字节 | Zero：4 字节 | Random：1528 字节 |
```

Time 是握手时间字段；Zero 在简单握手中置零；Random 是随机数据，供对端回应时回送。这里不是音视频业务数据。

当前实现先清零，再填随机区，因此时间字段也为零。生成 C0C1 或 S0S1 时跳过 9 字节：`1 字节版本 + 4 字节时间 + 4 字节零`。

```cpp
char* p = res_buf;
p += 9;
for (int i = 0; i < 1528; ++i) {
    *p++ = rd(); // 写当前字节，再让指针后移一字节
}
```

### 2.4 C2 和 S2：各 1536 字节

```text
| 对方的 Time：4 | 本地收到对方 1 包的时间 Time2：4 | Random echo：1528 |
```

- C2 回应 S1：回送 S1 的 Time 和随机部分。
- S2 回应 C1：回送 C1 的 Time 和随机部分。
- Time2 是回应方接收对方数据时的时间，不是前一包中的 Zero 字段。

**当前代码直接复制整个 S1/C1 来构造 C2/S2，是简化实现。** 它没有单独生成 Time2，也没有验证收到的回送内容。前面讲解中“把整个包原样传回去”描述的是项目做法，不是完整字段规则。[握手字段及顺序依据：Adobe RTMP 规范 §5.2](https://rtmp.veriskope.com/pdf/rtmp_specification_1.0.pdf)。

### 2.5 状态与返回值速查

| 当前状态 | 至少需要的输入 | 消费输入 | 生成输出 | 下一状态 |
|---|---:|---:|---:|---|
| HANDSHAKE_C0C1 | 1537 | 1537 | 3073（S0S1S2） | HANDSHAKE_C2 |
| HANDSHAKE_S0S1S2 | 3073 | 3073 | 1536（C2） | HANDSHAKE_COMPLETE |
| HANDSHAKE_C2 | 1536 | 1536 | 0 | HANDSHAKE_COMPLETE |

输入不足：返回 0，保留缓冲区和阶段，等下一次接收。版本错误：返回 -1。

`Parse()` 返回的是响应长度，不是消费长度，也不是布尔成功标志。返回 0 既可能是没收够，也可能是处理完 C2 无需回应；还要看 `IsCompleted()`。

## 3. BufferReader：数据不完整为什么能等下一次

```text
0                 reader_index_          writer_index_       size
| 已消费区域      | 已收到、未处理的数据 | 尾部可写空间      |
```

```cpp
ReadableBytes() = writer_index_ - reader_index_;
WritableBytes() = buffer_.size() - writer_index_;
Peek()          = Begin() + reader_index_;
```

| 操作 | 做什么 |
|---|---|
| recv → BeginWrite() | 把网络数据写到 writer_index_ 处 |
| writer_index_ += n | 标记新收到 n 字节 |
| Peek() | 查看未消费数据的地址，不移动索引 |
| Retrieve(n) | 标记已消费 n 字节，不必擦除原内存 |
| RetrieveAll() | 两个索引归零，后续从头覆盖 |

例：C0C1 分两次到达，第一次 1000 字节不足 1537，Parse 返回 0；第二次追加 537 字节，可读量成为 1537，才解析并消费。

例：收到 `C2(1536) + 命令数据(100)`，只消费 1536，剩余 100 字节交给 Chunk 解析器。一次 recv、一个 Chunk、一条 Message 三者没有固定一一对应关系。

“空间够不够”看 WritableBytes；“握手/块的数据收够没”看 ReadableBytes。

## 4. 分块与重组：300 字节怎么传

假设消息体长 300，当前发送方向 Chunk Size=128：

```text
Message Body：300
  → Chunk 1：头 + 128
  → Chunk 2：头 + 128
  → Chunk 3：头 + 44
```

公式：`块数 = 向上取整(消息长度 / Chunk Size)`。Chunk Size 不含头；Message Length 也不含各块头部。

本项目发送时，首块使用 fmt=0，续块使用 fmt=3。收端用每个 CSID 独立保存的 `RtmpMessage` 重组：

```cpp
本块应读长度 = min(完整消息长度 - 已拼长度, in_chunk_size_);
```

过程：

```text
解析头 → 保存 CSID 与消息参数 → 解析本块数据 → index 增加
    ↑                                      |
    └──────── 下一块的头 ←─────────────────┘

index == lenght：整条消息完成 → HandleMessage()
```

注意：读完一个块只是回到解析头的状态，不一定完成消息。只有累计长度达到消息总长度才分发。

不同 CSID 的完整块可以交错，例如视频块、音频块、视频续块。不能把一个块的头和数据中间随意插入另一个块。项目的 CreateChunk 会一次生成同一消息的所有块，并没有实现复杂的块级优先调度。

默认接收和发送 Chunk Size 均为 128，但两个方向独立：A 发 Set Chunk Size=N，告诉 B 后续 A→B 用 N；不会自动改变 B→A。

## 5. 头部速查：fmt / CSID / Type / MSID

```text
| Basic Header：1～3 | Message Header：11/7/3/0 | 扩展时间：可选4 | 本块数据 |
```

### 5.1 Basic Header

首字节：`[fmt 高2位][CSID 编码低6位]`。

```cpp
fmt = flags >> 6;
csid_code = flags & 0x3f;
```

| 低 6 位 | 总 Basic Header 长度 | 实际 CSID |
|---|---:|---|
| 2～63 | 1 | 低 6 位直接取值 |
| 0 | 2 | 64 + 后一个字节 |
| 1 | 3 | 64 + 后第1字节 + 后第2字节×256 |

0/1 是扩展标记。CSID 需使用足够宽的整数保存，不能总用 uint8_t。

### 5.2 Message Header

| fmt | 长度 | 携带字段 | 省略字段从哪里来 |
|---|---:|---|---|
| 0 | 11 | 绝对时间3、消息长度3、类型1、MSID4 | 完整头 |
| 1 | 7 | 时间增量3、消息长度3、类型1 | 同 CSID 的前置状态 |
| 2 | 3 | 时间增量3 | 同 CSID 的前置状态 |
| 3 | 0 | 无 Message Header | 同 CSID 的前置状态 |

fmt=3 仍有 Basic Header，必要时还有扩展时间戳。它可用于同一消息续块，也可在满足继承条件时用于新消息，不是“最后一块”的标记。

时间字段达到 0xFFFFFF 时，用三字节 FF FF FF 标记，附加四字节大端扩展时间。fmt=0 的时间是绝对值，fmt=1/2 是增量；fmt=3 的扩展字段是否存在依赖先前状态。

### 5.3 三种 ID 不要混

| 字段 | 回答的问题 | 示例 |
|---|---|---|
| CSID | 用哪个分块通道的历史头和拼装进度？ | 音频4、视频5（本项目分配） |
| Message Type ID | 消息体应该交给哪个处理器？ | 8音频、9视频、20命令 |
| MSID | 这条连接里的哪个逻辑消息流？ | createStream 返回的编号 |

同一路直播音频和视频通常 MSID 相同、CSID 不同、Type 不同。MSID 的协议作用域是当前连接，不是服务器全局直播间号；不同连接可以使用同一个 MSID。

本项目用 `stream_path_ = /app/streamName` 找 RtmpSession，才把主播与观众关联到同一路内容。示例 `/live/room100` 中，room100 更接近房间名。

## 6. 字节手算：把十六进制变成字段

一位十六进制数占4位；两个十六进制数是一字节。按字节算，每向高位移动一个字节，权重乘256。

```text
三字节大端 [a,b,c]：a×65536 + b×256 + c
三字节小端 [a,b,c]：a + b×256 + c×65536

00 03 E8（大端）= 0×65536 + 3×256 + 232 = 1000
00 01 2C（大端）= 0×65536 + 1×256 + 44  = 300
01 00 00 00（小端）= 1
```

`0xE8 = 14×16+8 = 232`。不是把 00、03、E8 三个数直接相加。

```cpp
// p 为 uint8_t*；显式转换后再移位，便于推广到四字节读取
uint32_t value = (uint32_t(p[0]) << 16)
               | (uint32_t(p[1]) << 8)
               | uint32_t(p[2]);
```

各项占用不同位，所以这里按位或等价于加法。

完整示例：CSID=5、时间1000、视频消息300字节、MSID=1、Chunk Size=128：

```text
首块头：05 | 00 03 E8 | 00 01 2C | 09 | 01 00 00 00
         |      |           |       |        └─ MSID，小端
         |      |           |       └─ 视频类型
         |      |           └─ 总消息长度，大端
         |      └─ 时间戳，大端
         └─ fmt=0，CSID=5

传输：
[上述12字节头][128字节消息数据]
[C5]         [128字节消息数据]
[C5]         [44字节消息数据]

C5 = (3 << 6) | 5：fmt=3、CSID=5
总传输量 = 300 + 12 + 1 + 1 = 314 字节（不含 TCP/IP 开销）
```

## 7. 握手后：connect → createStream → publish/play

### 7.1 命令放在哪里

命令也是 RTMP 消息，本项目使用 Type=20（0x14，AMF0 命令）。消息体通过 AMF 编码字符串、数字、对象等，不是直接拼接一段普通文本。

典型命令结构：`命令名 + transaction ID + 命令对象/null + 参数`。

transaction ID 用来配对请求和响应；MSID 用来指定流；二者不是同一编号。服务端 `_result` 通常回显请求的 transaction ID，`onStatus` 则用于状态通知。

### 7.2 connect：连接应用

```text
客户端 → 服务端：connect，MSID=0，携带 app 等连接参数
服务端 → 客户端：控制参数消息
服务端 → 客户端：_result，NetConnection.Connect.Success
```

项目 HandleConnect 读取 app，并依次调用：

| 调用 | 实际消息 | 消息体 |
|---|---|---|
| SendAcknowlegement | Window Acknowledgement Size，Type=5 | 4字节窗口大小 |
| SetPeerBandWidth | Set Peer Bandwidth，Type=6 | 4字节窗口大小+1字节限制类型 |
| SetChunkSize | Set Chunk Size，Type=1 | 4字节新的块大小 |

**SendAcknowlegement 名字容易误导：这里发的是确认窗口大小，不是 Type=3 的字节计数确认。** RTMP 层确认也不是 TCP ACK。通用协议还包含 User Control（Type=4，如 Ping/Stream Begin）；本项目消息 switch 没有完整处理所有这些控制消息。

### 7.3 createStream：拿到 MSID

```text
客户端 → 服务端：createStream，MSID=0
服务端 → 客户端：_result，携带新分配的 stream ID
```

获得编号后，publish/play 和媒体消息使用该 MSID。项目 HandleCreateStream 从 RtmpChunk::GetStreamId() 取值，回复后保存到连接的 stream_id_。这是项目分配策略，不是协议要求必须使用静态全局计数。

### 7.4 推流端：publish

```text
主播 → 服务端：publish，MSID=已分配编号，参数包含 streamName
服务端：构造 /app/streamName，检查是否已有发布者
服务端 → 主播：onStatus，NetStream.Publish.Start
主播 → 服务端：元数据、编码配置、持续音视频
```

项目成功时创建/获取 RtmpSession，把连接标记为 START_PUBLISH 并加入会话。流名冲突会返回 Publish.BadName。

### 7.5 播放端：play

观众要独立完成自己的 TCP、握手、connect、createStream，再请求播放；不会复用主播的 TCP 连接。

```text
观众 → 服务端：play，MSID=自己的编号，参数包含 streamName
服务端 → 观众：onStatus，NetStream.Play.Reset
服务端 → 观众：onStatus，NetStream.Play.Start
服务端 → 观众：|RtmpSampleAccess 数据通知（项目实现）
服务端：把观众加入相同 stream_path 对应的 Session
服务端 → 观众：配置和持续媒体数据
```

`|RtmpSampleAccess` 描述音视频样本访问相关标志，不是一般意义上的读写文件权限。不同客户端可能还发送 releaseStream/FCPublish 等辅助命令；不要把它们误认为每次必经的固定步骤。

## 8. 媒体交换：元数据、序列头、关键帧

| 数据 | 用处 | 本项目的识别方式 |
|---|---|---|
| 元数据 | 描述宽高、帧率、编码等信息 | Type=18，AMF 的 onMetaData |
| AAC 序列头 | AAC 解码配置 | Type=8，SoundFormat=10，AACPacketType=0 |
| AVC 序列头 | H.264 解码配置，通常含 SPS/PPS | Type=9，CodecID=7，AVCPacketType=0 |
| 普通音频数据 | 实际压缩声音 | AACPacketType=1 的 AAC 数据 |
| 视频编码数据 | 实际压缩画面 | AVCPacketType=1，进一步区分关键帧等 |

序列头是编解码配置，和 RTMP Chunk Header 不是同一种“头”。元数据也不能替代解码配置。

传统 AVC 视频消息体的开头含帧类型/CodecID、AVCPacketType、CompositionTime 等字段；本项目用首字节高4位判断帧类型、低4位判断 CodecID。音频首字节高4位是 SoundFormat，低4位是音频参数位，不能按视频格式当作 CodecID。

收到 Type=9，只知道这是视频消息；还需要看消息体才能判断编码和帧类型。时间戳不等于墙上时钟；AVC 中还要结合合成时间偏移处理显示时间。

本项目转发路径：

```text
主播 RTMP 数据
 → 主播连接 BufferReader
 → RtmpChunk 拼成 Message
 → HandleAudio / HandleVideo / HandleNotify
 → stream_path 对应的 RtmpSession
 → 遍历观众连接
 → 每个观众自己的 MSID、发送 Chunk Size、发送缓冲区
 → 观众播放器
```

因此转发不是简单把主播的所有原始网络字节照搬给观众：项目先重组消息，再为每个观众重新分块封装。

新观众加入时，Session 在后续媒体转发中尝试先发已缓存的 AAC、AVC 序列头。连接层还尝试等待关键帧，再开始发送普通媒体。理由是新观众没有此前参考画面，直接从任意预测帧开始通常不能正确解码。代码未实现完整 GOP 缓存回放，所以新观众可能需要等待后续关键帧。

这描述的是代码意图；元数据转发和关键帧筛选仍有实现问题，见 §10。实际发送顺序也取决于主播何时发送配置、配置是否已经缓存。

## 9. 代码定位与关闭流程

| 文件/函数 | 回忆用途 |
|---|---|
| ../EdoyunNet/TcpConnection.cpp：HandleRead | recv 到连接缓冲区，再调用业务回调 |
| RtmpConnection.cpp：OnRead | 未完成握手走 Handshake，完成后走 Chunk |
| RtmpHandshake.cpp：Parse | 判断阶段、等完整数据、生成握手响应 |
| RtmpChunk.cpp：ParseChunkHeader | 识别 fmt/CSID、读取或继承消息参数 |
| RtmpChunk.cpp：ParseChunkBody | 拷贝本块数据，累计 index |
| RtmpConnection.cpp：HandleMessage | 按 Type 分发控制、命令、音视频 |
| RtmpConnection.cpp：HandleInvoke | AMF 解码命令名，分发 connect/createStream/publish/play |
| RtmpSession.cpp：SendMediaData | 找观众、补配置、转发媒体 |
| RtmpConnection.cpp：SendRtmpChunks | 生成块并交给 TcpConnection::Send |

接收方向：`字节 → 块 → 完整消息 → AMF/媒体解释`。

发送方向：`AMF/媒体内容 → Message → 多个 Chunk → 发送缓冲区 → socket`。

断开时，项目 OnClose 调用 HandleDeleteStream，从 Session 移除连接。发布者移除时，Session 清除序列头缓存并更新发布者状态。协议中的关闭流、删除流、断开 TCP 是不同操作，不要认为发一个删除命令就一定立刻关闭整个 TCP 连接。

## 10. 当前实现与协议规则的差别

这部分是读代码时的提醒，不表示这些问题已修复。

- 握手输出容量参数没有实际校验；C2/S2 直接复制，缺少完整时间字段处理和回送校验。
- 一次 Handshake::Parse 只处理一个阶段；OnRead 没有循环推进所有已缓存的握手阶段。研究合并输入时需考虑此限制。
- 扩展 CSID 解析用 uint8_t，三字节计算写成乘255且有额外偏移，不能据此记协议公式；正确公式见 §5。
- 时间戳状态计算和扩展时间戳收发需要进一步验证；媒体转发使用 timestamp，而重组代码另有 _timestamp，不能仅凭变量名认为已经正确归一化。
- SendMetaData(metaData) 使用的是成员 meta_data_，未使用参数来编码，可能导致观众拿不到预期元数据。
- 关键帧等待逻辑对普通媒体调用 IsKeyFrame，未先限制为视频，不能直接当成完善的音视频启动策略。
- HandleInvoke 中匹配的是 `DeleteStream`，与常见协议命令 `deleteStream` 大小写不同，且所在分支有限制；断开回调清理和命令删除要分别检查。
- Session 的注释说新观众发送元数据，但该媒体转发分支实际显式补发的是 AAC/AVC 序列头，不是完整元数据快照。

## 11. 一分钟自测

1. 为什么握手要应用层处理？——TCP 不理解 RTMP；应用协议需要自己的初始化。
2. C0C1 多长？S0S1S2 多长？——1537、3073。
3. S2 回应谁？C2 回应谁？——C1、S1。
4. Parse 返回0一定失败吗？——不一定，可能等数据或无需回复。
5. 有效未消费长度怎么算？——writer_index - reader_index。
6. 300字节消息、块大小128，要几块？——3块：128、128、44。
7. fmt=3有没有头？——有 Basic Header，可能有扩展时间，没有 Message Header。
8. Type=9是不是H.264关键帧？——只说明视频，需进一步读消息体。
9. 音视频 MSID 相同就一定是全局同一房间？——不能跨连接这样判断，业务流名才用于关联内容。
10. 什么命令告诉服务端应用名、流名？——connect 提供 app；publish/play 提供 streamName。
11. 为什么新观众先需要序列头？——初始化解码器；有配置还可能需要等待关键帧。
12. 一次 recv 就是一条 Message 吗？——不是，要靠缓存和协议长度持续解析。

## 资料范围

主要依据当前项目的 RtmpHandshake、RtmpChunk、RtmpConnection、RtmpSession、RtmpMessage 和 EdoyunNet/BufferReader 实现，以及本次讨论中的字节示例。握手规范细节参考上文链接的 Adobe 原始规范；更复杂握手、增强 RTMP 编码扩展、完整客户端兼容性不在本文范围内。

## 12. 全局架构与源码调用链

本节按实际执行顺序阅读。代码块从当前源码摘录；每个片段前注明来源和行号，未展示的上下文不代表不存在。功能描述不等于完整正确性验证。

### 12.1 先认清每一层负责什么

| 层次 | 类/对象 | 管理的东西 | 产出 |
|---|---|---|---|
| 事件驱动 | EventLoop、TaskScheduler、Channel | 哪个 socket 可读/可写、定时器 | 调用注册的回调 |
| 接受连接 | Acceptor、TcpServer | 监听 socket、已建立连接表 | 每个客户端的连接对象 |
| 网络收发 | TcpConnection、BufferReader、BufferWriter | 输入字节、输出队列、发送进度 | 缓冲区数据/网络输出 |
| RTMP 协议 | RtmpHandshake、RtmpChunk | 握手阶段、分块和重组状态 | 完整 RtmpMessage |
| 数据载体 | RtmpMessage | 给 Chunk 保存字段、消息体和拼装进度 | 供上层处理的消息对象 |
| RTMP 业务 | RtmpConnection、AMF 编解码器 | 命令、主播/观众角色、应用名和流名 | 会话加入、媒体投递、响应消息 |
| 一路内容 | RtmpSession、RtmpSink 接口 | 这一路流的发布者、观众、编码配置 | 向观众分发媒体 |
| 全局管理 | RtmpServer | 按 stream_path 管理不同 Session | 查找、创建、清理会话 |

不要把 RtmpConnection 和 TcpConnection 想成两个独立客户端对象：前者继承后者，同一个派生对象中同时拥有网络能力和 RTMP 业务能力。

```text
RtmpServer（继承 TcpServer）
  ├─ TcpServer 的连接表：fd -> shared_ptr<TcpConnection>
  │    ├─ 主播的 RtmpConnection
  │    └─ 观众的 RtmpConnection
  └─ RTMP 会话表：/app/streamName -> shared_ptr<RtmpSession>
       └─ Session：弱引用关联该路主播、观众，缓存 AAC/AVC 配置

每个 RtmpConnection：
  ├─ 基类部分：socket 的 Channel + 读缓冲区 + 写队列
  ├─ Handshake：本连接的握手进度
  ├─ Chunk：本连接各 CSID 的重组状态
  ├─ AMF：命令和元数据的编解码
  └─ weak_ptr：指回 Server、Session
```

强引用负责维持对象生命；weak_ptr 用来找到对象但不单独延长其生命。lock() 尝试得到临时 shared_ptr，失败说明对象已经不存在。

### 12.2 启动：先有事件线程，再监听端口

main 创建 EventLoop、RtmpServer，设置发送块大小，再 Start。当前实际设置是 60000；前文 128 是协议默认及演算示例，不要把它当成这份 main 的最终发送配置。

源码：`main.cpp:8`

```cpp
    EventLoop loop(2);
    auto rtmp_server = RtmpServer::Create(&loop);
    rtmp_server->SetChunkSize(60000);
    rtmp_server->SetEventCallback([](std::string type,std::string stream_path){
        printf("[Event]%s,stream_path%s\n",type.c_str(),stream_path.c_str());
    });
    if(!rtmp_server->Start("172.21.205.121",1935))
    {
        printf("rtmp server failed\n");
    }
```

EventLoop 构造时调用 Loop，创建 Scheduler 及工作线程。main 后面的 sleep 循环主要用于保持进程运行，不是靠这段循环亲自调用 recv。

RtmpServer::Create 每次创建一个新对象，并不是单例；头文件中“设单例”的注释不准确。

### 12.3 新客户端来了：谁创建 Connection

Acceptor 接受连接后把 fd 交给 TcpServer 注册的回调。回调中的 OnConnect 是虚函数：实际 Server 是 RtmpServer，因此创建的是 RtmpConnection，随后由 TcpServer 的连接表保存。

源码：`../EdoyunNet/TcpServer.cpp:11`

```cpp
    acceptor_->SetNewConnectCallback([this](int fd){
        TcpConnection::Ptr conn = this->OnConnect(fd);
        if(conn)
        {
            this->AddConnection(fd,conn);
            conn->SetDisConnectCallback([this](TcpConnection::Ptr conn){
                int fd = conn->GetSocket();
                this->RemoveConnection(fd);
            });
        }
    });
```

源码：`RtmpServer.cpp:97`

```cpp
TcpConnection::Ptr RtmpServer::OnConnect(int socket)
{
    return std::make_shared<RtmpConnection>(shared_from_this(),loop_->GetTaskSchduler().get(),socket);
}
```

shared_from_this() 得到当前 Server 的 shared_ptr；make_shared 创建连接对象。调度器用于这个连接的 IO 事件处理。一个主播或一个观众，通常各自对应一个 TCP 连接和一个 RtmpConnection。

### 12.4 两层回调：怎么从 socket 到 RTMP

第一层是 Channel 的可读事件回调，进入 TcpConnection::HandleRead；第二层是业务读取回调，进入 RtmpConnection::OnRead。

```text
socket 可读
 → Scheduler 分发 Channel 事件
 → TcpConnection::HandleRead()
 → BufferReader::Read(fd)，执行 recv
 → readCb_(当前连接, 输入缓冲区)
 → RtmpConnection::OnRead(buffer)
```

RtmpConnection 构造时注册业务回调：

源码：`RtmpConnection.cpp:25`

```cpp
    this->SetReadCallback([this](std::shared_ptr<TcpConnection> conn,BufferReader& buffer){
        return this->OnRead(buffer);
    });
```

源码：`../EdoyunNet/TcpConnection.cpp:70`

```cpp
void TcpConnection::HandleRead()
{
    // 先从 socket 读入连接缓冲区，再交给业务层 ReadCallback 解析。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(is_closed_)
        {
            return;
   	    }
        int ret = read_buffer_->Read(channel_->GetSocket());
        if(ret < 0)
   	    {
            this->Close();
            return;
        } //这个锁会释放掉
    }
    if(readCb_)
    {
        bool ret = readCb_(shared_from_this(),*read_buffer_);
        if(!ret)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            this->Close();
        }
    }
}
```

[this] 捕获当前对象地址；BufferReader& 是原始输入缓冲区的引用。协议解析消费的就是该连接持有的缓冲区。注意外层解锁后才调用业务回调，避免一直持有网络读取锁执行整条业务链。

### 12.5 OnRead：先握手，再进入消息处理

Connection 构造时给服务端握手器设置 HANDSHAKE_C0C1。OnRead 是协议入口分流点：

源码：`RtmpConnection.cpp:39`

```cpp
bool RtmpConnection::OnRead(BufferReader &buffer)
{
    bool ret = true;
    if(handshake_->IsCompleted())//是否握手完成，完成之后才能发送message
    {
        ret = HandleChunk(buffer);
    }
    else
    {
        std::shared_ptr<char> res(new char[4096],std::default_delete<char[]>());
        int res_size = handshake_->Parse(buffer,res.get(),4096);
        if(res_size < 0)
        {
            ret = false;
        }
        if(res_size > 0)
        {
            this->Send(res.get(),res_size);
        }
        if(handshake_->IsCompleted())
        {
            if(buffer.ReadableBytes() > 0)
            {
                ret = HandleChunk(buffer);
            }
        }
    }
    return ret;
}
```

三个关键动作：Parse 产生握手响应；Send 发送响应；握手完成后，若缓冲区还有数据，马上处理后续 Chunk。这里的 res_size 是响应长度，而不是消费长度。

### 12.6 Chunk 拼好 Message，Connection 才解释类型

RtmpChunk 内部用 map<CSID,RtmpMessage> 保存各通道的重组进度。Connection 循环驱动解析器，每次推进头或体；完整后才分发业务。

源码：`RtmpConnection.cpp:74`

```cpp
bool RtmpConnection::HandleChunk(BufferReader &buffer)
{
    //处理块
    int ret = -1;
    do
    {
        RtmpMessage rtmp_msg;
        ret = rtmp_chunk_->Parse(buffer,rtmp_msg);
        if(ret >= 0) //解析成功，需要处理message
        {
            if(rtmp_msg.IsCompleted())
            {
                if(!HandleMessage(rtmp_msg))
                {
                    return false;
                }
            }
            if(ret == 0) //缓存区没有数据
            {
                break;
            }
        }
        else
        {
            return false;
        }
    } while (buffer.ReadableBytes() > 0);
    return true;
}

bool RtmpConnection::HandleMessage(RtmpMessage &rtmp_msg)
{
    bool ret = true;
    switch (rtmp_msg.type_id)
    {
    case RTMP_VIDEO: //视频数据
        ret = HandleVideo(rtmp_msg);
        break;
    case RTMP_AUDIO:
        ret = HandleAudio(rtmp_msg);
        break;
    case RTMP_INVOKE:
        ret = HandleInvoke(rtmp_msg);
        break;
    case RTMP_NOTIFY:
        ret = HandleNotify(rtmp_msg);
        break;
    case RTMP_SET_CHUNK_SIZE:
        rtmp_chunk_->SetInChunkSize(ReadUint32BE(rtmp_msg.playload.get()));
        break;
    default:
        break;
    }
    return ret;
}
```

ret > 0 只表示解析消费了字节；IsCompleted 才表示输出 Message 完整。Type=20 进入命令解析，8/9 进入音视频处理，18 进入元数据处理，1 更新本连接接收块大小。RtmpMessage 在这里是 Chunk 与 Connection 之间的数据载体，不是网络直接发送的 C++ 对象。

### 12.7 AMF：把消息体变成命令名和参数

AMF 是字符串、数字、对象等数据的序列化格式。Chunk 只负责重组字节；完整命令消息的 playload 才交给 AmfDecoder。第一个值通常是命令名，后面还有事务编号、命令对象、参数。


源码：`RtmpConnection.cpp:134`

```cpp
    amf_decoder_.reset(); //解析消息

    //解析消息名称
    int bytes_used =  amf_decoder_.decode(rtmp_msg.playload.get(),rtmp_msg.lenght,1);
    if(bytes_used < 0)
    {
        printf("bytes_used < 0\n");
        return false;
    }

    std::string method = amf_decoder_.getString();
```

decode 的最后一个参数 1 表示先解码一个值；bytes_used 记录已经解码多少字节，后面从 playload + bytes_used 继续。getString() 取得这次解出的命令名。

MSID=0 分支处理 connect/createStream；MSID 匹配已创建流的分支处理 publish/play。事务编号用于配对请求与响应，MSID 用于指定消息流，二者作用不同。

### 12.8 connect 和 createStream：选应用、拿编号

HandleConnect 读取 app，例如 live，发送确认窗口、带宽和块大小控制消息，最后 AMF 编码 _result 和连接成功状态。

源码：`RtmpConnection.cpp:307`

```cpp
    AmfObject amfObj = amf_decoder_.getObject("app");
    app_ = amfObj.amf_string; //获取引用程序的值
    if(app_ == "")
    {
        return false;
    }

    SendAcknowlegement();
    SetPeerBandWidth();
    SetChunkSize();
```

源码：`RtmpConnection.cpp:342`

```cpp
bool RtmpConnection::HandleCreateStream()
{

    int stream_id = rtmp_chunk_->GetStreamId();

    AmfObjects objects;
    //结果
    amf_encoder_.reset();
    amf_encoder_.encodeString("_result",7);
    amf_encoder_.encodeNumber(amf_decoder_.getNumber());
    //需要填一个空对象
    amf_encoder_.encodeObjects(objects);
    amf_encoder_.encodeNumber(stream_id);
    SendInvokeMessage(RTMP_CHUNK_INVOKE_ID,amf_encoder_.data(),amf_encoder_.size());
    stream_id_ = stream_id;
    return true;
}
```

createStream 的 _result 携带生成的编号，随后保存为当前连接的 stream_id_。GetStreamId 当前返回 RtmpChunk 的共享静态计数，这是项目的分配实现，不能据此认为协议 MSID 是全局房间号。

### 12.9 publish/play：决定这个连接的角色和所属内容

命令分发时从参数取 stream_name，拼成业务流路径：

源码：`RtmpConnection.cpp:159`

```cpp
    else if(rtmp_msg.stream_id == stream_id_)
    {
        bytes_used += amf_decoder_.decode(rtmp_msg.playload.get() + bytes_used ,rtmp_msg.lenght - bytes_used,3);
        stream_name_ = amf_decoder_.getString();
        stream_path_ = "/" + app_ + "/" + stream_name_;
```

publish 成功路径：检查是否有发布者 → 获取 Session → 回复 Publish.Start → 设置 START_PUBLISH / is_publishing_ → AddSink。

play 路径：回复 Play.Reset / Play.Start 及样本访问通知 → 设置 START_PLAY → 获取同路径 Session → AddSink。

两边的 stream_path 相同，才被放进同一个 Session。GetSession 在找不到时也会创建，因此此项目可以先有观众 Session，再有主播。

Session 通过 RtmpSink 接口使用连接的能力：RtmpConnection 同时继承 TcpConnection 和 RtmpSink。virtual SendMediaData 在运行时调用该连接的实现。

源码：`RtmpSession.cpp:14`

```cpp
void RtmpSession::AddSink(std::shared_ptr<RtmpSink> sink)
{
    std::lock_guard<std::mutex> lock(mutex_);
    rtmp_sinks_[sink->GetId()] = sink;
    if(sink->IsPublisher()) //如果是主播，说明这个直播间刚刚创建，没有设置元数据
    {
        avc_sequence_header_ = nullptr;
        aac_sequence_header_ = nullptr;
        avc_sequence_header_size_ = 0;
        aac_sequence_header_size_ = 0;
        has_publisher_ = true;
        publisher_ = sink;
    }
    return ;
}
```

这里的 rtmp_sinks_ 同时登记主播和观众，不是加入就给它发数据；后面转发时通过 IsPlayer 筛出观众。publisher_ 指向主播。主播加入时清空原序列头，避免沿用上一发布者配置。

### 12.10 主播音视频：先识别配置，再交 Session

看视频路径的关键片段：先前代码已从首字节高4位取 frame_type、低4位取 code_id。

源码：`RtmpConnection.cpp:284`

```cpp
    if(frame_type == 1 && code_id == RTMP_CODEC_ID_H264 && playload[1] == 0) //我们更新264序列头
    {
        avc_sequence_header_size_ = rtmp_msg.lenght;
        avc_sequence_header_.reset(new char[rtmp_msg.lenght],std::default_delete<char[]>());
        memcpy(avc_sequence_header_.get(),rtmp_msg.playload.get(),avc_sequence_header_size_);

        //TODO session来设置avc序列头
        session->SetAvcSequenceHeader(avc_sequence_header_,avc_sequence_header_size_);
        type = RTMP_AVC_SEQUENCE_HEADER;
    }

    //TODO session 发送视频数据；
    session->SendMediaData(type,rtmp_msg.timestamp,rtmp_msg.playload,rtmp_msg.lenght);
    return true;
}
```

识别到 AVC 序列头就复制并缓存；普通视频也继续交 Session。AAC 路径类似，识别 SoundFormat=10 且 AACPacketType=0 的配置。

RTMP_AVC_SEQUENCE_HEADER / RTMP_AAC_SEQUENCE_HEADER 是项目内部转发标签，发送给播放器时会映射回视频 Type=9 / 音频 Type=8，不能当成标准线上的消息类型。

这里并没有解码 H.264 画面，也没有把 AAC 转成 PCM。项目主要识别头部、缓存配置并转发压缩媒体。

### 12.11 Session：把同一份媒体分发给观众


源码：`RtmpSession.cpp:81`

```cpp
void RtmpSession::SendMediaData(uint8_t type, uint64_t timestamp, std::shared_ptr<char> data, uint32_t size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    //再来发媒体数据
    for(auto iter = rtmp_sinks_.begin();iter != rtmp_sinks_.end();){
        auto conn = iter->second.lock();
        if(conn == nullptr)
        {
            rtmp_sinks_.erase(iter++);
        }
        else //在线用户 观看中(发送音视频) 刚进直播间(发送元数据再发音视频数据)
        {
            if(conn->IsPlayer())
            {
                if(!conn->IsPlaying()) //刚进直播间
                {
                    conn->SendMediaData(RTMP_AAC_SEQUENCE_HEADER,timestamp,aac_sequence_header_,aac_sequence_header_size_); //音频元数据
                    conn->SendMediaData(RTMP_AVC_SEQUENCE_HEADER,timestamp,avc_sequence_header_,avc_sequence_header_size_); //视频元数据
                }
                //观看中
                conn->SendMediaData(type,timestamp,data,size);
            }
            iter++;
        }
    }
    return;
}
```

遍历 weak_ptr：失效就移除；有效且 IsPlayer 才发送。刚开始接收的观众，先尝试补发缓存的 AAC/AVC 配置，再发当前数据。

传递 shared_ptr 可以让多个观众暂时共享同一块消息体，减少这一步的深拷贝；后面 CreateChunk 和 BufferWriter 的裸指针 Append 仍会复制，并不是全链路零拷贝。

这段代码在遍历和调用观众发送函数期间持有 Session mutex，所以不要想当然认为已给每个观众投递独立异步任务。网络事件驱动不等于所有业务调用都会自动跨线程排队。

### 12.12 观众连接：重新封装为自己的 Message

每个观众都有自己的 MSID 和发送参数，不能直接复用主播整段原始 Chunk 字节。SendMediaData 中经过配置记录和关键帧等待判断后，创建新的消息描述：

源码：`RtmpConnection.cpp:681`

```cpp
    //收到关键帧之后开始发送message
    RtmpMessage rtmp_msg;
    rtmp_msg._timestamp = timestamp;
    rtmp_msg.stream_id = stream_id_;
    rtmp_msg.playload = playload;
    rtmp_msg.lenght = playload_size;

    //还有msg类型
    if(type == RTMP_VIDEO ||type ==RTMP_AVC_SEQUENCE_HEADER)
    {
        rtmp_msg.type_id = RTMP_VIDEO;
        SendRtmpChunks(RTMP_CHUNK_VIDEO_ID,rtmp_msg);
    }
    else if(type == RTMP_AUDIO ||type ==RTMP_AAC_SEQUENCE_HEADER)
    {
        rtmp_msg.type_id = RTMP_AUDIO;
        SendRtmpChunks(RTMP_CHUNK_AUDIO_ID,rtmp_msg);
    }
```

源码：`RtmpConnection.cpp:608`

```cpp
void RtmpConnection::SendRtmpChunks(uint32_t csid, RtmpMessage &rtmp_msg)
{
    uint32_t capacity = rtmp_msg.lenght + rtmp_msg.lenght / max_chunk_size_ * 5 + 1024; 
    std::shared_ptr<char> buffer(new char[capacity],std::default_delete<char[]>());
    int size = rtmp_chunk_->CreateChunk(csid,rtmp_msg,buffer.get(),capacity);
    if(size > 0)
    {
        this->Send(buffer.get(),size);
    }
}
```

这里 RtmpMessage 的作用变成“给 Chunk 的发送输入”：包含类型、时间、完整消息体和本观众 MSID。CreateChunk 一次把多个带头的块写进连续缓冲区。

```text
[块头1][数据1][块头2][数据2][块头3][数据3]
                         ↓
             Send(buffer, 总字节数)
```

不需要再次拆成一个个 Chunk 才能发。块边界给接收端协议解析器识别，发送队列只处理按序字节。

### 12.13 发送队列：数据没发完怎么办

TcpConnection::Send(const char*,size) 调用写队列 Append，随后尝试 HandleWrite。裸指针 Append 会复制输入，所以 SendRtmpChunks 的局部缓冲区退出后，队列中的数据仍然存在。

源码：`../EdoyunNet/BufferWriter.cpp:27`

```cpp
bool BufferWriter::Append(const char *data, uint32_t size, uint32_t index)
{
    if(size <= index)
    {
        return false;
    }
    if(buffer_.size() >= max_queue_length_)
    {
        return false;
    }

    Packet pkt;
    pkt.data.reset(new char[size + 512],std::default_delete<char[]>());
    memcpy(pkt.data.get(),data,size);
    pkt.size = size;
    pkt.writeIndex = index;
    buffer_.emplace(std::move(pkt));
    return true;
}
```

源码：`../EdoyunNet/BufferWriter.cpp:57`

```cpp
        Packet &pkt = buffer_.front();
        ret = ::send(sockfd,pkt.data.get() + pkt.writeIndex,pkt.size - pkt.writeIndex,0);
        if(ret > 0)
        {
            pkt.writeIndex += ret;
            if(pkt.size == pkt.writeIndex)
            {
                count += 1;
                buffer_.pop();
            }
        }
```

Packet 是发送队列条目，不等于 RTMP Chunk，也不等于 TCP 网络包。一个 Packet 可以包含一条消息拆出的多个 Chunk。

writeIndex 表示这份队列数据已经发送多少字节。send 只发了一部分时，更新 writeIndex，后面从未发送位置继续；发完才 pop。TcpConnection::HandleWrite 在队列未空时关注可写事件，队列空时取消关注。

```text
协议拆块：300字节 → 3个带头块 → 314字节（假设头12+1+1）
发送进度：314字节可能先发200，再发114
接收进度：对端可能先收70，再收180，再收64
```

这三种边界彼此独立。TCP 负责字节有序；BufferReader 与 RtmpChunk 负责处理任意接收分段。

### 12.14 断开：业务登记和网络连接分别清理

TcpConnection::Close 摘除 Channel，调用 closeCb（业务清理），再调用 disconnectCb（TcpServer 连接表移除）。RtmpConnection::OnClose 进入 HandleDeleteStream。

源码：`RtmpConnection.cpp:508`

```cpp
            auto conn = std::dynamic_pointer_cast<RtmpSink>(shared_from_this());
            GetTaskSchduler()->AddTimer([session,conn](){
                session->RemoveSink(conn);
                return false;
            },1);
```

项目没有在这里立刻 RemoveSink，而是注册一个1毫秒定时器。lambda 按值捕获 session、conn，使对象至少保留到回调执行；回调返回 false 表示不重复。

Session 移除主播时清理编码配置并更新发布者状态；Server 每3秒清除没有客户端的 Session。断开一个主播不等于立刻断开所有观众，已有观众连接和 Session 的生命周期还取决于剩余连接。

### 12.15 把两条链路连起来记

```text
主播发 publish：
recv → BufferReader → OnRead → Chunk::Parse
→ 完整 Message → HandleMessage → HandleInvoke → AMF decode
→ HandlePublish → 按 /app/name 获取 Session → AddSink

主播发视频：
recv → BufferReader → Chunk::Parse → 完整 Message
→ HandleVideo → Session::SendMediaData
→ 观众 Connection::SendMediaData → 构造 Message
→ CreateChunk → TcpConnection::Send → BufferWriter → send

观众发 play：
走自己的握手/建流 → HandlePlay → 同路径 Session::AddSink
→ 等待上述媒体转发路径把数据送到自己
```

### 12.16 看源码时的关键限定

- 主流程可以帮助理解架构，不表示当前实现具备完整 RTMP 兼容性。§10 中的扩展 CSID、时间戳、元数据转发、关键帧筛选问题仍存在。
- TcpConnection 的读取代码没有完整区分非阻塞读取暂时无数据、对端关闭等情况；不能把这份错误处理直接当作成熟网络库范例。
- BufferWriter::Append 可能因队列满而失败，但当前 TcpConnection::Send 未检查其返回值，背压处理不完整。
- SendMetaData 的参数与成员使用不一致，导致“代码设计要转发元数据”和“实际能否转发成功”需区分。
- 此项目没有显示完整视频解码、转码、播放器渲染链路；这里只负责 RTMP 接收与分发。

复习时先说清楚“这一层输入是什么、输出是什么、状态保存在谁那里”，再研究各字段和语法。

