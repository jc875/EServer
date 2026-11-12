#include "../EdoyunNet/BufferReader.h"

/**
 * RTMP 握手状态机。
 *
 * RTMP 握手由固定长度的数据包组成：
 *   - C0 / S0：1 字节版本号；
 *   - C1 / S1：1536 字节；
 *   - C2 / S2：1536 字节。
 *
 * 同一个类通过构造时传入的初始状态，分别支持客户端和服务端：
 *   - 客户端从 HANDSHAKE_S0S1S2 开始，等待服务端的 S0S1S2；
 *   - 服务端从 HANDSHAKE_C0C1 开始，等待客户端的 C0C1。
 *
 * Parse() 每次只消费一个当前状态所需的完整握手阶段；如果数据不完整，
 * 则保留 BufferReader 中的数据并返回 0，等待下次读取继续处理。
 */
class RtmpHandshake
{
public:
	/// 握手处理阶段。状态值也表示当前期望从对端读取的数据。
	enum State
	{
		HANDSHAKE_C0C1,     ///< 服务端：等待客户端发送 C0+C1。
		HANDSHAKE_S0S1S2,   ///< 客户端：等待服务端发送 S0+S1+S2。
		HANDSHAKE_C2,       ///< 服务端：等待客户端发送 C2。
		HANDSHAKE_COMPLETE  ///< 握手完成，可以进入 RTMP 消息处理阶段。
	};

	/// 以指定的角色和阶段创建握手状态机。
	RtmpHandshake(State state);
	virtual ~RtmpHandshake();

	/**
	 * 解析输入缓冲区中的握手数据，并生成需要发送给对端的响应。
	 *
	 * @param in_buffer     输入缓冲区。成功解析后会移除已消费的数据；
	 *                      数据不完整时不会消费任何数据。
	 * @param res_buf       输出缓冲区，用于保存握手响应。
	 * @param res_buf_size  输出缓冲区容量。当前实现由调用方保证容量足够。
	 * @return >= 0：需要发送的响应字节数；0 表示数据尚未收完整；
	 *         -1 表示 RTMP 版本号不支持。
	 */
	int Parse(BufferReader& in_buffer, char* res_buf, uint32_t res_buf_size);

	/// 判断握手是否已经完成。
	bool IsCompleted() const
	{
		return handshake_state_ == HANDSHAKE_COMPLETE;
	}
	
	/**
	 * 构造客户端发送的 C0+C1 握手包。
	 *
	 * C0 的版本号为 3，C1 的时间戳和版本字段当前置零，
	 * 其余字节使用随机数据填充。
	 *
	 * @return 实际生成的字节数；正常情况下为 1537。
	 */
	int BuildC0C1(char* buf,uint32_t buf_size);

private:
	/// 当前握手阶段，决定 Parse() 期待的数据格式及下一步动作。
	State handshake_state_;
};