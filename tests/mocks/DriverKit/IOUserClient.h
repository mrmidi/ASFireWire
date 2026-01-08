#pragma once
#include <DriverKit/OSObject.h>
#include <DriverKit/IOReturn.h>

struct IOUserClientMethodArguments {
    uint64_t* scalarInput;
    uint32_t scalarInputCount;
    void* structureInput;
    uint64_t structureInputSize;
    
    uint64_t* scalarOutput;
    uint32_t scalarOutputCount;
    class OSData* structureOutput;
    class IOBufferMemoryDescriptor* structureOutputDescriptor;
};

class IOUserClient : public OSObject {
public:
    virtual bool init() override { return true; }
    virtual void free() override { OSObject::free(); }
};
