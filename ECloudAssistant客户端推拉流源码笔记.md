# ECloudAssistant 客户端推拉流源码笔记

本文沿着一次「开始远程」阅读桌面上的 `ECloudAssistant` 工程。被控端采集并推送桌面与系统声音，控制端收到地址后拉流，在窗口里显示画面并播放声音。笔记以源码静态阅读为依据，环境相关的表现需要实际运行确认。

## 1. 先认清手里的数据

| 概念 | 在这个项目里是什么 |
| --- | --- |
| 像素格式 | 规定一张图的字节怎么排。BGRA 每像素 4 字节。8 位 YUV420P 有 Y、U、V 三个平面，U/V 各在横纵方向减半。读某一行还要看 stride，也就是内存里相邻两行的起始地址相差多少字节。 |
| PCM | 未压缩的声音采样。48 kHz 表示每声道每秒 48000 个采样。双声道 S16 每个采样时刻有左、右各 2 字节，共 4 字节。 |
| S16 与 FLTP | S16 表示 16 位有符号整数，当前采集链路按左右声道交错处理。FLTP 是分平面的浮点格式，每个声道有独立缓冲。格式转换要处理实际样本值和排布。 |
| H.264 / AAC | 视频和音频的压缩编码。2560×1440 的 BGRA 桌面以 30 fps 传原始像素约需 422 MiB/s，编码后才进入网络发送链。 |
| `AVFrame` / `AVPacket` | 在远端媒体编解码链路中，`AVFrame` 装待编码或解码后的像素/PCM，`AVPacket` 装编码数据。抓屏设备的 `av_read_frame()` 也用 `AVPacket` 承载设备数据，所以判断里面是什么还要看输入来源。 |
| 封装、RTMP Chunk、TCP | 媒体封装给编码数据加类型、时间等信息。RTMP 把消息切成 Chunk 传输，TCP 提供连续字节流。一次 TCP 读可能只有半个 Chunk，也可能包含多条消息。 |
| PTS / DTS / time_base | PTS 表示何时展示，DTS 表示何时解码，二者都是各自时间基下的刻度。比如时间基为 1/30 秒时，PTS 90 对应 3 秒。播放器还需要一套播放时钟才会按这些时间展示。 |

视频里几个容易混淆的词。**NAL 单元**是 H.264 的数据结构，一张图可能对应多个 NAL。**SPS/PPS**是解码参数，例如 SPS 描述画面尺寸相关信息。**IDR**是可从这里重新开始解码的一类图像，后续图像无须引用 IDR 之前的图像。**Annex B** 用 `00 00 01` 或 `00 00 00 01` 标识 NAL 边界，RTMP 的 AVC 媒体负载通常按长度逐个标识 NAL 边界。

AAC 的 **AudioSpecificConfig** 告诉解码器音频对象类型、采样率和声道配置等信息。FFmpeg 的 `extradata` 可保存这类解码所需参数。项目把 AVC/AAC 配置包装成各自的 **sequence header** 随流发送。配置和第一张可解码图是两个条件，只收到配置不会出画。

## 2. 点击「开始远程」后，谁通知谁？

```text
被控端登录成功 → RemoteWgt::handleLogined → RemoteManager::Init → 加入信令
控制端点开始远程 → RemoteManager::StartRemote → PullerWgt::Connect
                → AVPlayer::Connect → 控制端 SigConnection → 请求流

信令通知被控端创建流 → SigConnection::doCtreatStream
    → RemoteManager::HandleStartStream → RtmpPushManager::Open(URL)
    → 被控端把 URL 回给信令服务器
信令给控制端播放地址 → SigConnection::doPlayStream
    → AVPlayer::HandleStartStream → AVDEMuxer::Open(URL)

媒体链路：被控端 → RtmpSvr → 控制端
```

程序入口是 `main()` 创建 Qt 窗口 `ECloudAssistant`。同一文件里直接打开推流地址的代码在注释中。真实入口在 `RemoteWgt` 的按钮回调，被控端则由登录信号接入。控制端的 `SigConnection` 加入成功后调用 `obtainStream()`，开流回调从信令消息处理路径触发。被控端在 `doCtreatStream()` 中用写死的 RTMP 主机和本地递增的流编号拼出地址，推流入口返回成功后才回复地址。键鼠事件走信令连接，音视频走 RTMP 连接。

源码入口 · [main.cpp](../ECloudAssistant/main.cpp#L7) · [RemoteWgt.cpp](../ECloudAssistant/UI/center/RemoteWgt.cpp) · [RemoteManager.cpp](../ECloudAssistant/UI/center/RemoteManager.cpp) · [SigConnection.cpp](../ECloudAssistant/Net/SigConnection.cpp#L123) · [AVPlayer::Connect](../ECloudAssistant/Puller/UI/AVPlayer.cpp#L69)

## 3. 推流端，从桌面和声卡到 RTMP

### 3.1 初始化时准备什么

`RtmpPushManager::Open()` 先调用 `Init()`，依次准备发布器、抓屏器、H.264 编码器、音频采集器和 AAC 编码器。抓屏尺寸与编码配置都写成 2560×1440。视频编码配置是 30 fps、80000 kbps，GOP 配置为 30 帧。AAC 编码器从采集器获取采样率和声道，输入声明为 S16，目标码率 64 kbps。

编码器打开后，管理器取视频 `extradata`，用 `H264Paraser::findNal()` 找 SPS 和 PPS，再取 AAC 的 AudioSpecificConfig，交给 `SetMediaInfo()` 预先构造媒体序列头。`OpenUrl()` 建 TCP 并启动 RTMP 握手，握手和后续 `connect → createStream → publish` 随网络回包推进。随后管理器启动两个编码工作线程。`Open()` 返回 true 时，服务器的 `NetStream.Publish.Start` 还可能在路上。客户端 RTMP 连接收到这个状态才设 `is_publishing_`，而媒体发送入口检查的是连接对象是否存在、是否关闭，未用这个发布标志做门控。

发送方向还有一层。客户端的 SendVideoData() / SendAudioData() 分别构造视频和音频 RTMP 消息，再走 SendRtmpChunks() → RtmpChunk::CreateChunk() → TcpConnection::Send()。这里的发送结果只代表本地发送链路推进，播放端还需经服务端转发后才能收到。

源码 · [RtmpPushManager::Open/Init](../ECloudAssistant/Pusher/RtmpPushManager.cpp#L24) · [RtmpPublisher::OpenUrl](../ECloudAssistant/Pusher/rtmp/RtmpPublisher.cpp#L102) · [客户端 RTMP 命令和媒体发送](../ECloudAssistant/Pusher/rtmp/RtmpConnection.cpp#L208)

### 3.2 跟一张桌面图

1. **采集线程**在 `GDIScreenCapture::Init()` 打开 FFmpeg 的 `gdigrab` 输入 `desktop`。它的 `run()` 循环读设备包，经设备输入对应的解码器取出图像，在锁保护下更新一份当前画面缓存。
2. **视频编码线程**在 `EncodeVideo()` 调 `CaptureFrame()` 拷出当前图像。这个接口读取最新缓存，所以采集和编码频率不一致时，可能跳过旧画面或再次取到同一张。循环按约 30 ms 间隔尝试取图，编码器配置的 30 fps 则对应约 33.3 ms 的时间基，两者是独立的设置。
3. **编码器**经 `H264Encoder::Encode() → VideoEncoder::Encode()` 把 BGRA 转为 YUV420P，再调用 `avcodec_send_frame()` 和 `avcodec_receive_packet()`。前者提交一张图，后者取压缩输出。编码器可能先缓冲，所以一次提交后未必立刻拿到包。包装层遇到关键包还会把 `extradata` 拼在输出前面。
4. **发布器**经 `PushVideo() → PushVideoFrame()` 构造视频负载。普通 AVC 包的负载依次是帧类型和 codec 字节、AVC packet type、3 字节组合时间、4 字节 NAL 长度和编码数据。这个版本将组合时间写为 0，并用发布器计时器的毫秒读数作为 RTMP 时间戳。FFmpeg 包的 PTS/DTS 没有一路传到这里。

这里有个具体的字节边界问题。`PushVideo()` 固定去掉前 4 字节，发布器随后把整个剩余缓冲当成一条带长度的 NAL。如果输入有 3 字节起始码，或者里面是 SPS、PPS、IDR 等多个以起始码分隔的 NAL，按这个写法得到的负载可能无法被对端正确解析。

源码 · [GDIScreenCapture::Init](../ECloudAssistant/Pusher/capture/GDISreenScapture.cpp#L50) · [抓屏线程与缓存](../ECloudAssistant/Pusher/capture/GDISreenScapture.cpp#L159) · [EncodeVideo](../ECloudAssistant/Pusher/RtmpPushManager.cpp#L140) · [H264Encoder::Encode](../ECloudAssistant/Codec/H264Encoder.cpp#L33) · [VideoEncoder::Encode](../ECloudAssistant/Codec/VideoEncoder.cpp#L104) · [PushVideo](../ECloudAssistant/Pusher/RtmpPushManager.cpp#L291) · [PushVideoFrame](../ECloudAssistant/Pusher/rtmp/RtmpPublisher.cpp#L134)

### 3.3 跟一小段声音

1. **WASAPI 采集线程**使用默认播放设备和 `AUDCLNT_STREAMFLAGS_LOOPBACK`。环回采集拿的是该播放设备正在输出的系统声音。采集循环用 `GetBuffer()` 借一段设备数据，回调在 `ReleaseBuffer()` 前把数据拷进应用缓冲。
2. **AudioCapture** 是设备和编码器之间的适配层。`StartCapture()` 注册回调，用 `nBlockAlign × samples` 算写入 `AudioBuffer` 的字节数。`nBlockAlign` 是同一采样时刻所有声道所占的字节。设备每次给多少帧取决于采集节奏，AAC 编码器有自己的输入帧长。
3. **音频编码线程**在 `EncodeAudio()` 等 `GetSamples() >= GetFrames()`，然后读足一批 PCM 交给 `AACEncoder`。以每声道 1024 个采样、48 kHz 双声道 S16 为例，一批是 4096 字节，对应约 21.3 ms 声音。底层 `AudioEncoder` 将 S16 转成编码器使用的 FLTP，然后提交帧、读取 AAC 包。
4. **AAC 普通包**由 `PushAudioFrame()` 加两字节媒体头后交给 `SendAudioData()`。当前实现让普通音频等到首个视频关键帧通过发布器的判断才开始发。因此如果关键帧一直没通过，抓屏问题也可能表现为一直没声音。

两个值得对照代码的地方。WASAPI 在初始化前改写设备 mix format 的格式结构，采集循环里看不到显式的 float→S16 转换。设备是否接受申请格式要看 `Initialize()` 的返回结果。静音分支把自己的 `m_pcmBuf` 清零，回调却传原始 `pData`。另外，发布器在首个关键帧分支通过 `SendVideoData()` 发送 **AAC** 序列头，所以该配置会进入视频消息类型，播放器可能拿不到正确的音频配置。

源码 · [WASAPI 初始化](../ECloudAssistant/Pusher/capture/WASAPICapture.cpp#L18) · [WASAPI 取包与释放](../ECloudAssistant/Pusher/capture/WASAPICapture.cpp#L204) · [AudioCapture::StartCapture](../ECloudAssistant/Pusher/capture/AudioCapture.cpp#L79) · [EncodeAudio](../ECloudAssistant/Pusher/RtmpPushManager.cpp#L186) · [AudioEncoder::Encode](../ECloudAssistant/Codec/AudioEncoder.cpp#L95) · [发布器的序列头和 AAC 数据](../ECloudAssistant/Pusher/rtmp/RtmpPublisher.cpp#L134)

## 4. 拉流端，从地址到屏幕和扬声器

### 4.1 打开输入并识别轨道

`AVPlayer::Init()` 准备共享的 `AVContext`、解封装器和音频输出。控制端拿到 URL 时，`HandleStartStream()` 调 `AVDEMuxer::Open()` 启动读流线程，同时启动音频播放和视频播放两个消费者。此时播放器线程已启动，队列可以仍为空。

读流线程在 `FetchStreamInfo()` 中调用 `avformat_open_input()` 与 `avformat_find_stream_info()`。这里的 RTMP 接收由 FFmpeg 负责。代码记录音频、视频的 `stream_index`，用对应 `codecpar` 初始化解码器。随后循环 `av_read_frame()`，根据包的 `stream_index` 投进不同解码器。当前 `streamCb_` 默认是一个空操作函数，所以条件成立，会正常执行探测。不过它未把探测失败传到播放界面。读包返回错误时，这个循环退出。

源码 · [AVPlayer::Init](../ECloudAssistant/Puller/UI/AVPlayer.cpp#L33) · [AVPlayer::HandleStartStream](../ECloudAssistant/Puller/UI/AVPlayer.cpp#L221) · [AVDEMuxer::FetchStream](../ECloudAssistant/Codec/AVDEMuxer.cpp#L77) · [AVDEMuxer::FetchStreamInfo](../ECloudAssistant/Codec/AVDEMuxer.cpp#L125) · [默认 streamCb_](../ECloudAssistant/Codec/AVDEMuxer.h#L31)

### 4.2 为什么每路都要经过两级队列？

```text
读流线程
  ├─ 压缩视频包队列 → 视频解码线程 → YUV 帧队列 → 视频播放线程 → Qt UI / OpenGL
  └─ 压缩音频包队列 → 音频解码线程 → PCM 帧队列 → 音频播放线程 → QAudioOutput
```

第一级队列解耦网络读取和解码。一次读流只要分拣包，不用等某个解码器把它处理完。第二级队列解耦解码与显示/声卡消费。解码器用 `avcodec_send_packet()` 提交压缩包，再反复 `avcodec_receive_frame()` 取可用帧，直到暂时没有输出或遇到错误。视频解码后转换为渲染使用的 YUV420P，音频解码后经重采样转换为 44.1 kHz、双声道、交错 S16。两条链通过共享指针传递包或帧，队列和消费者持有的引用让数据跨线程存活。

`AVQueue` 底层是链表。入队、出队有互斥保护，`size()`、`empty()` 直接读共享计数，无同样的加锁。队列没有容量上限，消费者在空队列上短暂休眠再检查。如果解码持续快于输出，排队时间和内存占用都会增长。

源码 · [视频解码循环](../ECloudAssistant/Codec/H264_Decoder.cpp#L77) · [音频解码循环](../ECloudAssistant/Codec/AAC_Decoder.cpp#L73) · [AV_Queue.h](../ECloudAssistant/Codec/AV_Queue.h)

### 4.3 画面最后在哪里出现，声音最后在哪里播放？

- **视频**经 `videoPlay() → sig_repaint(frame)` 发往 `OpenGLRender::Repaint()`。连接方式是 `Qt::QueuedConnection`，因此渲染槽在窗口所属的 UI 线程执行。`Repaint()` 更新当前帧并调用 `update()` 请求重绘。随后 `paintGL()` 使用 Y、U、V 三个纹理平面绘制。看到 `sig_repaint` 被发出只说明帧已交给 Qt 事件队列。
- **音频**经 `audioPlay() → AudioRender::Write()` 写入 `QAudioOutput::start()` 返回的 `QIODevice`。PCM 写入量按 `nb_samples × channels × 每采样字节数` 计算。当前条件 `!AvailableBytes()` 只识别返回值恰好为 0 的情况，而函数可能给出负数。`Write()` 也未处理只写入一部分数据的情况。
- **同步**目前由两路各自消费队列。音频受设备缓冲影响，视频有帧就发重绘信号。这两段播放循环没有用帧 PTS 和共同播放时钟调度，所以延迟不同或一侧积压时，可能看到音画错位。

源码 · [AVPlayer 播放循环](../ECloudAssistant/Puller/UI/AVPlayer.cpp#L93) · [OpenGLRender::Repaint](../ECloudAssistant/Puller/Render/OpenGLRender.cpp#L56) · [paintGL](../ECloudAssistant/Puller/Render/OpenGLRender.cpp#L201) · [AudioRender.cpp](../ECloudAssistant/Puller/Render/AudioRender.cpp#L16)

## 5. 沿失败路径读代码

| 现象 | 先查哪一站 | 对照的代码行为 |
| --- | --- | --- |
| 点远程后连地址都没有 | 控制端 `doJoin()/obtainStream()`、被控端 `doCtreatStream()` | 被控端只有在开流回调成功后才把 URL 回给信令服务器。检查是否走到了回调，以及抓屏、音频或 RTMP TCP 初始化是否提前返回。 |
| 收到地址，窗口一直空着 | `FetchStreamInfo()` → `av_read_frame()` → 解码器输入/输出队列 | 播放线程和探测并发启动。区分打开输入失败、压缩包没来、包来了但没有解码帧这三个阶段。 |
| 第一张图一直不来，音频也不发 | `PushVideo()` → `PushVideoFrame()` → `SendVideoData()` | 分别确认 NAL 边界、关键帧判断和发布状态。普通 AAC 还受视频关键帧门控。 |
| 还没收到流就关闭窗口 | `AVPlayer::Close()` | 停流直接对两个线程指针调用 `joinable()`。没创建线程时指针可能为空。即使正常停过一次，线程也会被清空，再走 Close 仍要看这个分支。 |
| 推过一次后重开，线程立即退出 | `RtmpPushManager::Close()/Open()` | `Close()` 置 `exit_ = true`，`Open()` 未复位。停流还会先关闭并重置发布器，再等待编码线程退出，需留意编码线程仍可能访问发布器。 |

正常停流意图是让生产/消费线程先退出，再回收它们访问的资源。拉流端 `AVDEMuxer::Close()` 置 `quit_`，FFmpeg 的中断回调读取该标志以便退出阻塞操作，随后回收读流线程和输入上下文。上面的提前关闭场景可能在到达这一步之前就出问题。

源码 · [RtmpPushManager::Close](../ECloudAssistant/Pusher/RtmpPushManager.cpp#L123) · [AVPlayer::Close](../ECloudAssistant/Puller/UI/AVPlayer.cpp#L45) · [AVDEMuxer::Close](../ECloudAssistant/Codec/AVDEMuxer.cpp#L49) · [InterruptFouction](../ECloudAssistant/Codec/AVDEMuxer.cpp#L191)

## 6. 读完后自己走两遍

1. **从被控端移动鼠标到控制端看到画面变化。** 依次找抓屏缓存更新、编码线程复制图像、视频负载形成、RTMP 服务器转发、FFmpeg 分出视频包、解码器输出 YUV、UI 上传纹理。标出每一步拿到的是像素、压缩数据还是网络字节。
2. **从被控端播放一声提示音到控制端听见。** 依次找 WASAPI `GetBuffer`、`AudioCapture` 写入、攒够 AAC 输入帧、发送、解码后重采样、`QIODevice::write`。如果网络上已有普通 AAC 数据但没有声音，再向前找 AAC 序列头是按哪种 RTMP 消息发送的。
