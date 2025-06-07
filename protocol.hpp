#pragma once
#include <cstdint>
#include <vector>
#include <cstring>


struct PDU
{
    uint16_t packetLength;
    uint8_t  msgType;
    uint8_t  flags;
    uint32_t sequenceNumber;
    uint16_t classId;
};


enum class MsgType : uint8_t
{
    HELLO = 1, 
    CONNECTED = 2,  
    STATE_UPDATE = 3,  
    BYE = 4
};


struct GameObject
{
    virtual ~GameObject() = default;
    virtual uint8_t  getClassId() const = 0;
    virtual void     serialize(std::vector<uint8_t>& buf) const = 0;
    virtual void     deserialize(const std::vector<uint8_t>& buf,
        size_t& offset) = 0;
};

