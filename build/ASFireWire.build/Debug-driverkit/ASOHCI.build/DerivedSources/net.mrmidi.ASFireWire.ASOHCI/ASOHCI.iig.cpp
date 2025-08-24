/* iig(DriverKit-440 Aug 13 2025 15:49:28) generated from ASOHCI.iig */

#undef	IIG_IMPLEMENTATION
#define	IIG_IMPLEMENTATION 	ASOHCI.iig

#if KERNEL
#include <libkern/c++/OSString.h>
#else
#include <DriverKit/DriverKit.h>
#endif /* KERNEL */
#include <DriverKit/IOReturn.h>
#include "ASOHCI.h"


#if __has_builtin(__builtin_load_member_function_pointer)
#define SimpleMemberFunctionCast(cfnty, self, func) (cfnty)__builtin_load_member_function_pointer(self, func)
#else
#define SimpleMemberFunctionCast(cfnty, self, func) ({ union { typeof(func) memfun; cfnty cfun; } pair; pair.memfun = func; pair.cfun; })
#endif


#if !KERNEL
extern OSMetaClass * gOSContainerMetaClass;
extern OSMetaClass * gOSDataMetaClass;
extern OSMetaClass * gOSNumberMetaClass;
extern OSMetaClass * gOSStringMetaClass;
extern OSMetaClass * gOSBooleanMetaClass;
extern OSMetaClass * gOSDictionaryMetaClass;
extern OSMetaClass * gOSArrayMetaClass;
extern OSMetaClass * gOSSetMetaClass;
extern OSMetaClass * gOSOrderedSetMetaClass;
extern OSMetaClass * gIODispatchQueueMetaClass;
extern OSMetaClass * gIOBufferMemoryDescriptorMetaClass;
extern OSMetaClass * gIOUserClientMetaClass;
extern OSMetaClass * gOSActionMetaClass;
extern OSMetaClass * gIOServiceStateNotificationDispatchSourceMetaClass;
extern OSMetaClass * gIOMemoryMapMetaClass;
#endif /* !KERNEL */

#if !KERNEL

#define ASOHCI_QueueNames  ""

#define ASOHCI_MethodNames  ""

#define ASOHCIMetaClass_MethodNames  ""

struct OSClassDescription_ASOHCI_t
{
    OSClassDescription base;
    uint64_t           methodOptions[2 * 0];
    uint64_t           metaMethodOptions[2 * 0];
    char               queueNames[sizeof(ASOHCI_QueueNames)];
    char               methodNames[sizeof(ASOHCI_MethodNames)];
    char               metaMethodNames[sizeof(ASOHCIMetaClass_MethodNames)];
};

const struct OSClassDescription_ASOHCI_t
OSClassDescription_ASOHCI =
{
    .base =
    {
        .descriptionSize         = sizeof(OSClassDescription_ASOHCI_t),
        .name                    = "ASOHCI",
        .superName               = "IOService",
        .methodOptionsSize       = 2 * sizeof(uint64_t) * 0,
        .methodOptionsOffset     = __builtin_offsetof(struct OSClassDescription_ASOHCI_t, methodOptions),
        .metaMethodOptionsSize   = 2 * sizeof(uint64_t) * 0,
        .metaMethodOptionsOffset = __builtin_offsetof(struct OSClassDescription_ASOHCI_t, metaMethodOptions),
        .queueNamesSize       = sizeof(ASOHCI_QueueNames),
        .queueNamesOffset     = __builtin_offsetof(struct OSClassDescription_ASOHCI_t, queueNames),
        .methodNamesSize         = sizeof(ASOHCI_MethodNames),
        .methodNamesOffset       = __builtin_offsetof(struct OSClassDescription_ASOHCI_t, methodNames),
        .metaMethodNamesSize     = sizeof(ASOHCIMetaClass_MethodNames),
        .metaMethodNamesOffset   = __builtin_offsetof(struct OSClassDescription_ASOHCI_t, metaMethodNames),
        .flags                   = 0*kOSClassCanRemote,
        .resv1                   = {0},
    },
    .methodOptions =
    {
    },
    .metaMethodOptions =
    {
    },
    .queueNames      = ASOHCI_QueueNames,
    .methodNames     = ASOHCI_MethodNames,
    .metaMethodNames = ASOHCIMetaClass_MethodNames,
};

OSMetaClass * gASOHCIMetaClass;

static kern_return_t
ASOHCI_New(OSMetaClass * instance);

const OSClassLoadInformation
ASOHCI_Class = 
{
    .description       = &OSClassDescription_ASOHCI.base,
    .metaPointer       = &gASOHCIMetaClass,
    .version           = 1,
    .instanceSize      = sizeof(ASOHCI),

    .resv2             = {0},

    .New               = &ASOHCI_New,
    .resv3             = {0},

};

extern const void * const
gASOHCI_Declaration;
const void * const
gASOHCI_Declaration
__attribute__((used,visibility("hidden"),section("__DATA_CONST,__osclassinfo,regular,no_dead_strip"),no_sanitize("address")))
    = &ASOHCI_Class;

static kern_return_t
ASOHCI_New(OSMetaClass * instance)
{
    if (!new(instance) ASOHCIMetaClass) return (kIOReturnNoMemory);
    return (kIOReturnSuccess);
}

kern_return_t
ASOHCIMetaClass::New(OSObject * instance)
{
    if (!new(instance) ASOHCI) return (kIOReturnNoMemory);
    return (kIOReturnSuccess);
}

#endif /* !KERNEL */

#ifdef KERNEL
#define MESSAGE_CONTENT(__field) (messageContent->__field)
#else /* KERNEL */
#define MESSAGE_CONTENT(__field) (message->content.__field)
#endif /* KERNEL */

kern_return_t
ASOHCI::Dispatch(const IORPC rpc)
{
    return _Dispatch(this, rpc);
}

kern_return_t
ASOHCI::_Dispatch(ASOHCI * self, const IORPC rpc)
{
    kern_return_t ret = kIOReturnUnsupported;
#ifdef KERNEL
    IORPCMessage * msg = rpc.kernelContent;
#else /* KERNEL */
    IORPCMessage * msg = IORPCMessageFromMach(rpc.message, false);
#endif /* KERNEL */

    switch (msg->msgid)
    {
        case IOService_Start_ID:
        {
            ret = IOService::Start_Invoke(rpc, self, SimpleMemberFunctionCast(IOService::Start_Handler, *self, &ASOHCI::Start_Impl));
            break;
        }
        case IOService_Stop_ID:
        {
            ret = IOService::Stop_Invoke(rpc, self, SimpleMemberFunctionCast(IOService::Stop_Handler, *self, &ASOHCI::Stop_Impl));
            break;
        }

        default:
            ret = IOService::_Dispatch(self, rpc);
            break;
    }

    return (ret);
}

#if KERNEL
kern_return_t
ASOHCI::MetaClass::Dispatch(const IORPC rpc)
{
#else /* KERNEL */
kern_return_t
ASOHCIMetaClass::Dispatch(const IORPC rpc)
{
#endif /* !KERNEL */

    kern_return_t ret = kIOReturnUnsupported;
#ifdef KERNEL
    IORPCMessage * msg = rpc.kernelContent;
#else /* KERNEL */
    IORPCMessage * msg = IORPCMessageFromMach(rpc.message, false);
#endif /* KERNEL */

    switch (msg->msgid)
    {

        default:
            ret = OSMetaClassBase::Dispatch(rpc);
            break;
    }

    return (ret);
}



