#ifndef _BUFFERREADER_H_
#define _BUFFERREADER_H_
#include <vector>
#include <cstdint>
#include <string>

uint32_t ReadUint32BE(char* data);
uint32_t ReadUint32LE(char* data);
uint32_t ReadUint24BE(char* data);
uint32_t ReadUint24LE(char* data);
uint16_t ReadUint16BE(char* data);
uint16_t ReadUint16LE(char* data);

class BufferReader
{
public:
    BufferReader(uint32_t initial_size = 2048);
    virtual ~BufferReader();
    inline uint32_t ReadableBytes() const{return writer_index_ - reader_index_;}
    inline uint32_t WritableBytes() const{return buffer_.size() - writer_index_;}
    char* Peek(){return Begin() + reader_index_;}
    const char* Peek()const{return Begin() + reader_index_;}
    void RetrieveAll()
    {
        writer_index_ = 0;
        reader_index_ = 0;
    }
    // Copilot

//BufferReader::Retrieve 是一个内联函数，用于从缓冲区中检索指定长度的字节。如果请求的长度小于可读取的字节数，则更新读取索引；如果读取索引与写入索引相等，则调用 RetrieveAll 函数以清空缓冲区。
    void Retrieve(size_t len)
    {
        if(len < ReadableBytes())
        {
            reader_index_ += len;
            if(reader_index_ == writer_index_)
            {
                RetrieveAll();
            }
        }
        else
        {
            RetrieveAll();
        }
    }

    int Read(int fd);
    uint32_t ReadAll(std::string& data);
    uint32_t Size() const{
        return buffer_.size();
    }
private:
    char* Begin()
    {
        return &*buffer_.begin();
    }
    
    const char* Begin() const
    {
        return &*buffer_.begin();
    }

    char* BeginWrite()
    {
        return Begin() + writer_index_;
    }

    const char* BeginWrite() const
    {
        return Begin() + writer_index_;
    }
private:
    std::vector<char> buffer_;
    size_t reader_index_ = 0;
    size_t writer_index_ = 0;
    static const uint32_t MAX_BYTES_PER_READ = 4096;
    static const uint32_t MAX_BUFFER_SIZE = 1024 * 100000; //加大缓冲区
};
#endif