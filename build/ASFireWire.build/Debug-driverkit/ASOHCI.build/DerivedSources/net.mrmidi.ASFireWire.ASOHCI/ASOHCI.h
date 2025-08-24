/* iig(DriverKit-440) generated from ASOHCI.iig */

/* ASOHCI.iig:1-14 */
//
//  ASOHCI.iig
//  ASOHCI
//
//  Created by Aleksandr Shabelnikov on 23.08.2025.
//

#ifndef ASOHCI_h
#define ASOHCI_h

#include <Availability.h>
#include <DriverKit/IOService.h>  /* .iig include */
#include <PCIDriverKit/IOPCIDevice.h>  /* .iig include */

/* source class ASOHCI ASOHCI.iig:15-22 */

#if __DOCUMENTATION__
#define KERNEL IIG_KERNEL

class ASOHCI: public IOService
{
public:
    virtual kern_return_t
    Start(IOService * provider) override;
    
    virtual kern_return_t
    Stop(IOService * provider) override;
};

#undef KERNEL
#else /* __DOCUMENTATION__ */

/* generated class ASOHCI ASOHCI.iig:15-22 */


#define ASOHCI_Start_Args \
        IOService * provider

#define ASOHCI_Stop_Args \
        IOService * provider

#define ASOHCI_Methods \
\
public:\
\
    virtual kern_return_t\
    Dispatch(const IORPC rpc) APPLE_KEXT_OVERRIDE;\
\
    static kern_return_t\
    _Dispatch(ASOHCI * self, const IORPC rpc);\
\
\
protected:\
    /* _Impl methods */\
\
    kern_return_t\
    Start_Impl(IOService_Start_Args);\
\
    kern_return_t\
    Stop_Impl(IOService_Stop_Args);\
\
\
public:\
    /* _Invoke methods */\
\


#define ASOHCI_KernelMethods \
\
protected:\
    /* _Impl methods */\
\


#define ASOHCI_VirtualMethods \
\
public:\
\


#if !KERNEL

extern OSMetaClass          * gASOHCIMetaClass;
extern const OSClassLoadInformation ASOHCI_Class;

class ASOHCIMetaClass : public OSMetaClass
{
public:
    virtual kern_return_t
    New(OSObject * instance) override;
    virtual kern_return_t
    Dispatch(const IORPC rpc) override;
};

#endif /* !KERNEL */

#if !KERNEL

class  ASOHCIInterface : public OSInterface
{
public:
};

struct ASOHCI_IVars;
struct ASOHCI_LocalIVars;

class ASOHCI : public IOService, public ASOHCIInterface
{
#if !KERNEL
    friend class ASOHCIMetaClass;
#endif /* !KERNEL */

#if !KERNEL
public:
#ifdef ASOHCI_DECLARE_IVARS
ASOHCI_DECLARE_IVARS
#else /* ASOHCI_DECLARE_IVARS */
    union
    {
        ASOHCI_IVars * ivars;
        ASOHCI_LocalIVars * lvars;
    };
#endif /* ASOHCI_DECLARE_IVARS */
#endif /* !KERNEL */

#if !KERNEL
    static OSMetaClass *
    sGetMetaClass() { return gASOHCIMetaClass; };
#endif /* KERNEL */

    using super = IOService;

#if !KERNEL
    ASOHCI_Methods
    ASOHCI_VirtualMethods
#endif /* !KERNEL */

};
#endif /* !KERNEL */


#endif /* !__DOCUMENTATION__ */

/* ASOHCI.iig:24- */

#endif /* ASOHCI_h */
