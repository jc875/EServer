# 01 基础概念与 WASAPI：从你已经会的 FFmpeg 讲起

> 讲解对象：
>
> `C:\Users\金\Desktop\ECloudAssistant`
>
> （Qt + 自研 RTMP + FFmpeg 的云助教 / 远程控制项目）
> 你的起点：会用 FFmpeg 做解封装、编解码。本文假设你完全没碰过 Windows 音频 API。



***

## 一、先建立一张总图：声音和画面在电脑里到底是什么

### 1. 声音 = 一串数字（PCM）

生活类比：你用手机拍桌子，每秒拍 48000 次，每次记下 "此刻桌面振动幅度是多少"，记成一个整数。这一串整数就是 **PCM（脉冲编码调制）**，也就是未经压缩的原始音频。

三个必备参数（看真实代码 `AudioCapture.h` 第 28-30 行的默认值）：



| 参数                   | 含义         | 代码里的默认值 | 生活类比             |
| -------------------- | ---------- | ------- | ---------------- |
| 采样率 `samplerate`     | 每秒记多少次     | 48000   | 每秒拍 48000 张照片    |
| 声道数 `channels`       | 几路声音       | 2（立体声）  | 左右两只耳朵各一本账       |
| 位深 `bits_per_sample` | 每次用几个比特记幅度 | 16      | 记账本的格子大小，格子越大越精细 |

一帧 16bit 双声道音频占多少字节，代码里反复出现这个公式：



```
// AudioBuffer.h / AudioCapture.cpp 里的真实公式
字节数 = 采样帧数 * 位深/8 * 声道数;
// 例：1024 帧 = 1024 * 2 * 2 = 4096 字节
```

### 2. 画面 = 一张像素矩阵



* 一张 2560×1440 的屏幕，每个像素用 **BGRA** 四个字节表示（蓝、绿、红、透明度），一帧就是 `2560*1440*4 ≈ 14.7 MB`。

* 30 帧 / 秒意味着每秒原始数据 **442 MB**，根本没法直接走网络。所以必须压缩：H.264 把它压到每秒几百 KB。

### 3. 为什么需要 "编码"——FFmpeg 干的活你已经知道了



* PCM → AAC（音频压缩）

* BGRA/YUV → H.264（视频压缩）

* 压缩后的数据还要按协议 "打包" 才能上网，这就是 RTMP 干的事。

**关键认知**：FFmpeg 是 "加工厂"，但它不负责从你电脑的声卡 / 屏幕里 "取货"。取货要靠操作系统自己的 API：



* 音频取货（Windows）：**WASAPI**（本项目用的）

* 屏幕取货（Windows）：**GDI**（本项目用 `gdigrab`，FFmpeg 帮你封装了一层）



***

## 二、WASAPI 是什么？为什么你以前没听过

### 1. 全称与定位

**WASAPI = Windows Audio Session API（Windows 音频会话 API）**，从 Windows Vista 开始引入，是微软官方的 "底层音频接口"。

生活类比：



* 你以前用 FFmpeg 打开一个 mp3 文件，像在**外卖平台点已经做好的菜**；

* WASAPI 像直接走进**后厨的传菜口**：系统声卡混音后的所有声音（浏览器、游戏、QQ 提示音）都从这个传菜口经过，你可以直接把它们端走。

### 2. 本项目要它做什么 ——Loopback（环回采集）

看真实代码 `WASAPICapture.cpp` 第 63 行：



```
// 初始化音频客户端：共享模式 + LOOPBACK 标志
hr = m_audioClient->Initialize(
    AUDCLNT_SHAREMODE_SHARED,          // 共享模式：和其他程序共用声卡
    AUDCLNT_STREAMFLAGS_LOOPBACK,      // 环回：采集"扬声器正在播放的声音"
    REFTIMES_PER_SEC,                  // 缓冲时长 = 1 秒（单位是 100ns）
    0,
    m_mixFormat,
    NULL);
```

注意两个点：



* `eRender`（第 35 行 `GetDefaultAudioEndpoint(eRender, ...)`）选的是**渲染设备（扬声器）**，不是麦克风。

* 配合 `LOOPBACK` 标志，意思是 "我不录音，我要偷听系统正要播给扬声器的那份声音"。这就是直播软件能把电脑内部声音播出去的原理，不需要对着麦克风外放。

### 3. WASAPI 的四个核心 COM 对象（对应真实头文件成员）

WASAPI 是 **COM 接口**技术，所以代码里全是 `IXxx` 接口指针和 `HRESULT` 返回值。按初始化顺序看：



```
IMMDeviceEnumerator   （设备枚举器：声卡名单）
        │ GetDefaultAudioEndpoint()
        ▼
IMMDevice             （一块具体声卡，这里是默认扬声器）
        │ Activate()
        ▼
IAudioClient          （音频客户端：管理模式、格式、缓冲、启停）
        │ GetService()
        ▼
IAudioCaptureClient   （采集客户端：真正一包包取数据的接口）
```

对应 `WASAPICapture.h` 第 50-53 行的真实成员：



```
Microsoft::WRL::ComPtr<IMMDeviceEnumerator> m_enumerator;   // 枚举器
Microsoft::WRL::ComPtr<IMMDevice>           m_device;       // 声卡设备
Microsoft::WRL::ComPtr<IAudioClient>        m_audioClient;  // 音频客户端
Microsoft::WRL::ComPtr<IAudioCaptureClient> m_audioCaptureClient; // 取包接口
```

`ComPtr` 是微软的智能指针，作用类似 `std::unique_ptr`，出作用域自动 `Release()`，不用手动管 COM 引用计数 —— 这是现代写法，理由是**防止忘记 Release 导致对象永远不释放**。

### 4. 先把 COM 地基打牢（不然代码看不懂）

#### （1）组件 vs 接口：带 CLSID 的是组件，带 IID 的是接口

COM 里有两种"身份证"，都是 GUID（128 位、全球唯一的编号），用途完全不同：

| 前缀 | 全称 | 标识谁 | 代码里的例子 |
| --- | --- | --- | --- |
| `CLSID_` | Class ID | **组件类**（能被创建出来的具体实现） | `CLSID_MMDeviceEnumerator` |
| `IID_` | Interface ID | **接口**（一组方法的契约） | `IID_IAudioClient`、`IID_IAudioCaptureClient` |

- **组件（对象）**：真正存在、真正干活的实体，内部有代码和数据，但你不能直接摸它内部。
- **接口**：组件对外暴露的一组方法（功能菜单）。你只能通过接口指针调它的方法。
- 类型名以字母 **I** 开头的都是接口：`IMMDeviceEnumerator`、`IMMDevice`、`IAudioClient`、`IAudioCaptureClient`。
- 一个组件可以实现**多个接口**，你报哪个 IID，它就给你哪个接口。

生活类比：组件是**一个具体的人**，接口是他身上的**岗位**。同一个人可能既是前台又是财务；你说"找前台"（报 IID），他只能办接待，说"找财务"（另一个 IID）才能报销。人还是同一个，岗位能做的事不同。这就是"**面向接口编程，不面向实现**"。

看 `CoCreateInstance` 一行里两者同时出现：

```
CoCreateInstance(CLSID_MMDeviceEnumerator, ...,   // CLSID：创建哪个组件（造人）
                 IID_IMMDeviceEnumerator, ...);   // IID：要它的哪个接口（按岗位找人）
```

后面的 `Activate(IID_IAudioClient)`、`GetService(IID_IAudioCaptureClient)` 因为对象已经存在，只报 IID 要新接口，不再需要 CLSID。

#### （2）COM 的固定规矩

* `CoInitialize(NULL)`：在当前线程初始化 COM 库（第 29 行）。**规矩：用 COM 的线程必须先初始化，结束时 `CoUninitialize()` 配对。**

* `CoCreateInstance(...)`：像工厂订货，创建枚举器对象。

* 所有 COM 方法返回 `HRESULT`，`FAILED(hr)` 为真就是出错，代码里每个调用后都检查，这是正确的防御式写法。

* 生活类比：COM 像一套严格的"前台登记制度"——进门先登记（CoInitialize），找任何部门都通过前台（CoCreateInstance / Activate），拿到的东西用完要还（ComPtr 自动还）。

#### （3）`Initialize` 和 `Start` 是两个阶段，别混

这是最容易误会的地方：名字都像"启动"，其实分工不同。

* **`Initialize`（在 `init()` 里）= 装管道、签合同，此时不流水。** 它只做静态准备：分配内部环形缓冲、约定共享/环回、记录格式。这时候音频引擎还没往缓冲写数据，缓冲是空的。
* **`Start`（在 `start()` 里）= 拧开水阀。** 让这条流进入运行状态，音频引擎从此开始把扬声器混音真正搬进缓冲、时间戳开始走。只有 Start 之后，采集线程 `GetBuffer` 才拿得到东西。
* **Start 不是"去取数据"**，取数据是采集线程里 `GetBuffer` 干的；Start 只是"让系统开始供货"。

类比：Initialize 是把水管接好、水表装好，Start 是拧开阀门，水（混音流）才开始流，之后你拿杯子（GetBuffer）才接得到。

#### （4）那为什么改了"身份证"它不按 16bit 给你？

因为 `WAVEFORMATEX` 是**协商申请，不是命令**。共享模式环回下，数据格式由系统混音器说了算：

* 环回是"分一份扬声器正在播放的混音"，那股混音引擎内部就是 32bit float，**管道里真实跑的字节是 float**，这是事实。
* 你在结构体里写"我要 16bit"，引擎不会因此重做混音器；它要么 `Initialize` 直接报不支持，要么收下标签却照旧写 float 字节。
* `GetBuffer` 取到的永远是引擎实际写入的东西，跟你贴的标签无关。

所以不能靠改申请单改变水管里的液体，要 16bit 只能在取到 float 后**自己做转换**（见第四节）。



***

## 三、逐行走一遍 WASAPI 初始化（`init()` 第 15-91 行）



```
int WASAPICapture::init()
{
    std::lock_guard<std::mutex> lock(m_mutex);   // 加锁：防止多线程同时 init
    if(m_initialized) return 0;                  // 幂等：初始化过就直接返回

    CoInitialize(NULL);                          // ① 初始化 COM

    // ② 创建"设备枚举器"——声卡名单管理器
    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL,
         CLSCTX_ALL, IID_IMMDeviceEnumerator,
         (void**)m_enumerator.GetAddressOf());
    if(FAILED(hr)) return -1;

    // ③ 拿到默认的"渲染"端点（扬声器）。eMultimedia 表示多媒体用途
    hr = m_enumerator->GetDefaultAudioEndpoint(
         eRender, eMultimedia, m_device.GetAddressOf());
    if(FAILED(hr)) return -1;

    // ④ 激活设备，取出 IAudioClient 接口
    hr = m_device->Activate(IID_IAudioClient, CLSCTX_ALL, NULL,
         (void**)m_audioClient.GetAddressOf());
    if(FAILED(hr)) return -1;

    // ⑤ 问系统：你的混音格式是什么？（通常是 48kHz 双声道 IEEE 浮点 32bit）
    hr = m_audioClient->GetMixFormat(&m_mixFormat);
    if(FAILED(hr)) return -1;

    adjustFormatTo16Bits(m_mixFormat);           // ⑥ 想改成 16bit（这里有 bug，见下）

    // ⑦ 共享模式 + 环回，缓冲 1 秒
    hr = m_audioClient->Initialize(
         AUDCLNT_SHAREMODE_SHARED,
         AUDCLNT_STREAMFLAGS_LOOPBACK,
         REFTIMES_PER_SEC, 0, m_mixFormat, NULL);
    if(FAILED(hr)) return -1;

    // ⑧ 问内部缓冲能装多少帧
    hr = m_audioClient->GetBufferSize(&m_bufferFrameCount);

    // ⑨ 取出真正的"取包"接口
    hr = m_audioClient->GetService(IID_IAudioCaptureClient,
         (void**)m_audioCaptureClient.GetAddressOf());

    // ⑩ 算一下整个缓冲对应的实际时长（100ns 为单位）
    m_hnsActualDuration = REFERENCE_TIME(
        REFTIMES_PER_SEC * m_bufferFrameCount / m_mixFormat->nSamplesPerSec);
    m_initialized = true;
    return 0;
}
```

两个时间单位常量（头文件第 35-36 行）：



```
const int REFTIMES_PER_SEC = 10000000;      // 1 秒 = 10^7 个 100ns 单位
const int REFTIMES_PER_MILLISEC = 10000;    // 1 毫秒 = 10000 个 100ns 单位
```

### 为什么系统给的是浮点格式？

`GetMixFormat()` 在绝大多数 Windows 机器上返回的是 **IEEE 浮点 32bit（-1.0 \~ 1.0 的小数）**，因为 Windows 内部音频引擎用浮点做混音，精度高、不容易在多路声音叠加时削顶失真。

但 AAC 编码器和网络传输更希望拿到整数 PCM（S16），于是有了第 59 行的 `adjustFormatTo16Bits`。



***

## 四、如实报 bug：`adjustFormatTo16Bits` 是这份代码最大的硬伤

看它做了什么（第 162-186 行）：



```
int WASAPICapture::adjustFormatTo16Bits(WAVEFORMATEX *pwfx)
{
    if(pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        pwfx->wFormatTag = WAVE_FORMAT_PCM;   // 只是把"标签"改成 PCM！
    ...
    pwfx->wBitsPerSample = 16;                // 只是把"位深数字"改成 16！
    pwfx->nBlockAlign = ...;                   // 重算对齐字节
    pwfx->nAvgBytesPerSec = ...;
    return 0;
}
```

### 问题本质：改说明书不会改变货物

生活类比：一箱箱子上贴着 "苹果"，你把标签撕了换成 "橘子"，里面装的还是苹果。



* `WAVEFORMATEX` 只是一张**格式说明书**，告诉别人 "数据长什么样"。

* 真正的数据字节是 Windows 音频引擎按 **float32 位模式**填进缓冲的。改结构体字段不会让 Windows 帮你做 float→int 的数学转换。

* 接下来第 63 行拿着改坏的说明书去 `Initialize`，共享模式下系统可能直接拒绝（`AUDCLNT_E_UNSUPPORTED_FORMAT`，0x88890008）；就算某些驱动容忍了，后面按 S16 去读 float 的字节，听到的是噪声。

### 正确做法（两种）

**做法 A（推荐，工业界常规）：不动系统格式，拿到 float32 后自己转换。**



```
// float32 [-1.0, 1.0] -> int16 [-32768, 32767] 的真实转换
// 每个采样点做一次缩放+截断，这才是真正的"换货物"
static int16_t float_to_s16(float sample) {
    if (sample >= 1.0f)  return 32767;        // 钳位：超范围就封顶，防止爆音
    if (sample <= -1.0f) return -32768;
    return static_cast<int16_t>(sample * 32767.0f);
}
```

然后用项目里已有的 `AudioResampler`（swresample）或自己循环转换，再喂给 AAC 编码器。

**做法 B：先问系统能不能直接给 S16。**



```
// 询问系统：我想要这个格式，你支持吗？
WAVEFORMATEX* closest = nullptr;
HRESULT hr = m_audioClient->IsFormatSupported(
    AUDCLNT_SHAREMODE_SHARED, wanted_s16_format, &closest);
// 只有返回 S_OK 才说明系统原生支持，不能自己假设
```

### 还有一个连带 bug：静音分支传错了指针

`capture()` 第 225-239 行：



```
if(flags & AUDCLNT_BUFFERFLAGS_SILENT)
    memset(m_pcmBuf.get(), 0, m_pcmBufSize);   // 静音：把自己的缓冲清零
else
    memcpy(m_pcmBuf.get(), pData, ...);         // 正常：拷到自己的缓冲

if(m_callback)
    m_callback(m_mixFormat, pData, numFrameAvailabel);  // 但回调传的是 pData！
```

静音时明明清零了 `m_pcmBuf`，回调却把**原始指针** `pData` 传出去，静音标志等于白处理。合理写法是统一传 `m_pcmBuf.get()`。

#### 静音也要照常发，不能干脆不发

`SILENT` 标志的意思是"这段有数据、但振幅全是 0"，不是"这段没数据、跳过"。全 0 的字节就是"没声音"的数学表示。静音帧仍然要照常编码、照常发 RTMP，原因有三：

1. **音频按固定时间格子走，不能有空洞**：AAC 一帧固定 1024 采样，这帧不发，对端这段时间没东西播，听感是断流/卡顿，而不是"安静"。
2. **时间戳要连续**：少发一段，时间戳出现缺口，音画同步会错乱。
3. **对端分不清"静音"和"掉线"**：不发会被当成网络卡了；发一帧全 0，它知道连接正常、只是没声音。

生活类比：地铁按时刻表发车，哪怕这趟一个乘客都没有，也得准点开一趟**空车**，否则整条时刻表全乱。全 0 的音频帧就是那趟空车，维持时间节奏。正确链路：静音 → 清零缓冲 → 回调 `m_pcmBuf.get()` → AAC 照常编码 → RTMP 照常发，时间戳不断。



***

## 五、采集循环：数据是怎么一包包取出来的（`capture()` 第 188-258 行）

`start()` 里起了一个独立线程（第 125-133 行），循环调用 `capture()`：



```
int WASAPICapture::capture()
{
    uint32_t packetLenght = 0;
    // ① 先问：现在有没有攒好一个包？
    hr = m_audioCaptureClient->GetNextPacketSize(&packetLenght);
    if(FAILED(hr)) return -1;

    if(packetLenght == 0) {
        // 没数据睡 1ms，避免空转把 CPU 吃满
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return 0;
    }

    while (packetLenght > 0) {
        BYTE* pData; DWORD flags;
        // ② 锁定并取出这一包数据
        hr = m_audioCaptureClient->GetBuffer(&pData, &numFrameAvailabel,
                                             &flags, NULL, NULL);
        // ③ 自己的缓冲不够大就扩容（new 一块新的）
        if(m_pcmBufSize < numFrameAvailabel * m_mixFormat->nBlockAlign) {
            m_pcmBufSize = numFrameAvailabel * m_mixFormat->nBlockAlign;
            m_pcmBuf.reset(new uint8_t[m_pcmBufSize],
                           std::default_delete<uint8_t[]>());
        }
        // ④ 静音清零 / 正常拷贝（见上面的 bug）
        // ⑤ 通过回调把数据交给上层 AudioCapture
        if(m_callback) m_callback(m_mixFormat, pData, numFrameAvailabel);
        // ⑥ 释放缓冲，告诉系统"这包我用完了"——不释放，下一包永远拿不到
        hr = m_audioCaptureClient->ReleaseBuffer(numFrameAvailabel);
        // ⑦ 继续问还有没有包
        hr = m_audioCaptureClient->GetNextPacketSize(&packetLenght);
    }
    return 0;
}
```

### 这个 `GetBuffer → 处理 → ReleaseBuffer` 模式非常重要

生活类比：图书馆借书 ——`GetBuffer` 是把书借走，`ReleaseBuffer` 是还书。**借了不还，图书馆的书架很快全空，后面的人（新数据）进不来。** WASAPI 是固定大小的环形缓冲，系统写、你读，你不 Release，它写满就只能丢数据或卡住。

### 停止流程（`stop()` 第 137-155 行）



```
m_isEnabeld = false;        // 先通知线程退出
m_threadPtr->join();        // 等线程真正结束（join = 等它收工）
m_audioClient->Stop();      // 再停音频客户端
```

顺序是对的：**先停消费线程，再停设备**，反过来会让线程访问已关闭的接口而报错。



***

## 六、WASAPI 在整个项目里的位置（承上启下）



```
WASAPICapture        系统声卡 → 一包包 PCM（回调）
   │ 回调
AudioCapture         把 PCM 存进 AudioBuffer，对外暴露 Read()
   │ Read
RtmpPushManager      按 AAC 要求的帧数取出 → AAC 编码 → RTMP 推走
```

下一份文档 `02_推流流水线.md` 会接着讲：PCM 怎么进缓冲、怎么和 H.264 画面一起被打包推上网络。



***

## 七、本文件要点自测（先答不上来再回看）



1. 为什么直播软件不开麦克风也能推电脑内部声音？（提示：哪两个标志组合）

2. WASAPI 四个 COM 对象的调用顺序是什么？各自管什么？

3. 为什么直接改 `WAVEFORMATEX` 字段不能把 float 变成 16bit PCM？正确做法是什么？

4. `GetBuffer` 之后为什么必须 `ReleaseBuffer`？不 Release 会怎样？

5. `stop()` 里为什么先 `join()` 线程再 `Stop()` 设备？

6. CLSID 和 IID 分别标识什么？为什么说"一个组件可以有多个接口"？

7. `Initialize` 和 `Start` 各做什么？为什么 Start 之后 `GetBuffer` 才有数据？

8. 静音时为什么不能干脆不发 RTMP，而要发一帧全 0 的数据？