#include <cstdint>
#include <memory>

// 线上 fmt=0 的 Message Header：11 字节。其他 fmt 只携带其前缀字段。
// 它不含 Basic Header、扩展时间戳或消息体；不能把整个 RtmpMessage 直接发到网络。
    struct RtmpMessageHeader
    {
        uint8_t timestamp[3]; // 大端：fmt=0 为绝对时间，fmt=1/2 为增量
        uint8_t lenght[3]; // 大端：整条消息体长度，不是当前块长度；lenght 是原有拼写
        uint8_t type_id; // 消息类型：8 音频、9 视频、20 AMF0 命令
        uint8_t stream_id[4]; //小端存储
    };

    // 内存中的完整消息及重组状态；接收时可处于尚未拼完整的状态。
    struct RtmpMessage
    {
        uint32_t timestamp = 0; // 解析器保存的时间字段/状态；具体累加见 Parse
        uint32_t lenght = 0; // 消息体总长度；CreateChunk 会把它当剩余长度递减
        uint8_t type_id = 0; // 决定完整消息交给音频、视频还是命令处理器
        uint32_t stream_id =0; // MSID：当前连接中的逻辑消息流，不是全局房间号
        uint32_t extend_timestamp = 0; // 扩展时间字段；普通时间为 0xFFFFFF 时使用

        uint64_t _timestamp = 0; // 本实现累计的时间/发送时间；不能与 timestamp 无条件互换
        uint8_t codeId = 0; // 预留编码信息，不属于 RTMP Chunk Header 的字段

        uint8_t csid = 0; // 分块通道编号；注意 uint8_t 无法覆盖协议的扩展 CSID
        uint32_t index = 0; // 已经拼入 playload 的消息体字节数
        std::shared_ptr<char> playload = nullptr; // 消息体内存；playload 为原有拼写

        // 准备接收后续消息：保留长度、类型、MSID 等，重置部分拼装状态。
        // 重新分配消息体；先前复制出去的 shared_ptr 仍持有旧数据。
        // 注意它也清除了时间字段，不能认为实现了所有 fmt 的完整继承语义。
        // 隐患：timestamp 被清零后，同 CSID 通道下一条 fmt=1/2（增量时间戳）
        // 消息的继承基数丢失、时间戳从 0 重算；fmt=0（绝对时间）不受影响，
        // 主流推流端都用 fmt=0，所以当前不触发。
        void Clear()
        {
            index = 0;
            timestamp = 0;
            extend_timestamp = 0;
            if(lenght > 0)
            {
                playload.reset(new char[lenght],std::default_delete<char[]>());
            }
        }

        // 完整消息判定：不是“收到一个 Chunk”，而是累计收到全部消息体。
        bool IsCompleted() const 
        {
            if(index == lenght && lenght > 0 && playload != nullptr)
            {
                return true;
            }
            return false;
        }
    };
    