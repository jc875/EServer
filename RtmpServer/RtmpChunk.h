#include "../EdoyunNet/BufferReader.h"
#include "../EdoyunNet/BufferWriter.h"
#include "RtmpMessage.h"
#include <map>

// 每个连接使用的分块编解码器，不是一个单独 Chunk 的数据对象。
// 接收：字节流 -> 多个 Chunk -> 完整 RtmpMessage。
// 发送：RtmpMessage -> 带头的多个 Chunk -> 连续输出字节。
class RtmpChunk
{
public:
    enum State
    {
        PARSE_HEADER, // 下一步等待当前块的头
        PARSE_BODY, // 头已解析，下一步等待当前块的数据
    };
    RtmpChunk();
    virtual ~RtmpChunk();
    // 每次只推进头或体一个阶段。返回 >0 消费字节数，0 等更多输入，<0 错误。
    // 只有完整消息才写 out_rtmp_msg；返回 >0 不代表消息已完成。
    int Parse(BufferReader& in_buffer,RtmpMessage& out_rtmp_msg);
    // 把一条消息的所有块写入 buf，返回含头的总字节数；不直接发送 socket。
    // in_msg 是引用：当前实现会把 in_msg.lenght 递减到 0。
    int CreateChunk(uint32_t csid,RtmpMessage& in_msg,char* buf,uint32_t buf_size);
    // 两个方向独立：对端通知的块大小影响接收，本端通知的影响发送。
    void SetInChunkSize(uint32_t in_chunk_size)
    {
        in_chunk_size_ = in_chunk_size;
    }
    void SetOutChunkSize(uint32_t out_chunk_size)
    {
        out_chunk_size_ = out_chunk_size;
    }
    // 只清除各 CSID 的消息缓存，不会复位 state_ 和 chunk_stream_id_。
    void Clear()
    {
        rtmp_message_.clear();
    }
    int GetStreamId()const{
        return stream_id_;
    }
protected:
    int ParseChunkHeader(BufferReader& buffer);
    int ParseChunkBody(BufferReader& buffer);
    int CreateBasicHeader(uint8_t fmt,uint32_t csid,char* buf);
    int CreateMessageHeader(uint8_t fmt,RtmpMessage& rtmp_msg,char* buf);
private:
    State state_;
    int chunk_stream_id_ = 0; // 当前正要读取块体的 CSID；-1 表示未选择
    static int stream_id_; // 项目用于生成 MSID 的共享计数，不是 CSID
    uint32_t in_chunk_size_ = 128;
    uint32_t out_chunk_size_ = 128;
    // 每个 CSID 各自保留历史头和拼装进度，允许不同通道的块交错到达。
    std::map<int,RtmpMessage> rtmp_message_;
    // 只计 Message Header，不包含 Basic Header 和可选扩展时间戳。
    const int KChunkMessageHeaderLenght[4] = {11,7,3,0};
};