/* iig(DriverKit-456.120.3) generated from IOUserAudioControl.iig */

/* IOUserAudioControl.iig:1-40 */
/*
 * Copyright (c) 2020-2021 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#ifndef IOUserAudioControl_h
#define IOUserAudioControl_h

#include <DriverKit/IOService.h>  /* .iig include */
#include <AudioDriverKit/IOUserAudioObject.h>  /* .iig include */
#include <AudioDriverKit/AudioDriverKitTypes.h>

using namespace AudioDriverKit;

class IOUserAudioDriver;
class IODispatchQueue;

/* source class IOUserAudioControl IOUserAudioControl.iig:41-151 */

#if __DOCUMENTATION__
#define KERNEL IIG_KERNEL

/*!
 * @class IOUserAudioControl
 *
 * @brief
 * IOUserAudioControl is a subclass of IOUserAudioObject and base class for control objects.
 *
 * @discussion
 * IOUserAudioControl should not be subclassed or allocated directly.
 */
class LOCALONLY IOUserAudioControl: public IOUserAudioObject
{
public:
	/*!
	 * @function init
	 *
	 * @abstract
	 * Initializes a IOUserAudioControl.
	 *
	 * @param in_driver
	 * The IOUserAudioDriver that owns this object.
	 *
	 * @param in_is_settable
	 * A bool value indicating if the control value can be set
	 *
	 *@param in_control_element
	 * A IOUserAudioObjectPropertyElement for the control
	 *
	 * @param in_control_scope
	 * A IOUserAudioObjectPropertyScope for the control
	 *
	 * @return
	 * true on success.
	 */
	virtual bool init(IOUserAudioDriver* in_driver,
					  bool in_is_settable,
					  IOUserAudioObjectPropertyElement in_control_element,
					  IOUserAudioObjectPropertyScope in_control_scope);
	
	/*!
	 * @function free
	 *
	 * @abstract
	 * frees the IOUserAudioObject.
	 */
	virtual void free() override;

#pragma mark IOUserAudioObject overrides
	/*!
	 *@function GetClassID
	 *
	 * @abstract
	 * Get the IOUserAudioClassID of the object
	 *
	 * @discussion
	 * Overrides the base class IOUserAudioObject
	 *
	 * @return
	 * Returns IOUserAudioClassID
	 */
	virtual IOUserAudioClassID GetClassID() override;

	/*!
	 *@function GetBaseClassID
	 *
	 * @abstract
	 * Get the IOUserAudioClassID of the base class object
	 *
	 * @discussion
	 * Overrides the base class IOUserAudioObject
	 *
	 * @return
	 * Returns IOUserAudioClassID
	 */
	virtual IOUserAudioClassID GetBaseClassID() override;
	
#pragma mark Control configuration
	/*!
	 *@function GetIsSettable
	 *
	 * @abstract
	 * Bool value to check if the control value can be set
	 *
	 * @discussion
	 * True if the control value can be set
	 *
	 * @return
	 * Returns bool
	 */
	bool GetIsSettable();

	/*!
	 *@function GetControlElement
	 *
	 * @abstract
	 * Returns a IOUserAudioObjectPropertyElement for the control
	 *
	 * @return
	 * Returns IOUserAudioObjectPropertyElement
	 */
	IOUserAudioObjectPropertyElement GetControlElement();

	/*!
	 *@function GetControlScope
	 *
	 * @abstract
	 * Returns a IOUserAudioObjectPropertyScope for the control
	 *
	 * @return
	 * Returns IOUserAudioObjectPropertyScope
	 */
	IOUserAudioObjectPropertyScope GetControlScope();
};

#undef KERNEL
#else /* __DOCUMENTATION__ */

/* generated class IOUserAudioControl IOUserAudioControl.iig:41-151 */


#define IOUserAudioControl_Methods \
\
public:\
\
    bool\
    GetIsSettable(\
);\
\
    IOUserAudioObjectPropertyElement\
    GetControlElement(\
);\
\
    IOUserAudioObjectPropertyScope\
    GetControlScope(\
);\
\
\
protected:\
    /* _Impl methods */\
\
\
public:\
    /* _Invoke methods */\
\


#define IOUserAudioControl_KernelMethods \
\
protected:\
    /* _Impl methods */\
\


#define IOUserAudioControl_VirtualMethods \
\
public:\
\
    virtual kern_return_t\
    _SetPropertyData(\
        const IOUserAudioObjectPropertyAddress * in_prop_addr,\
        OSData * in_qualifier_data,\
        OSData * in_data) APPLE_KEXT_OVERRIDE;\
\
    virtual kern_return_t\
    _GetPropertyData(\
        const IOUserAudioObjectPropertyAddress * in_prop_addr,\
        OSData * in_qualifier_data,\
        OSData ** out_data) APPLE_KEXT_OVERRIDE;\
\
    virtual kern_return_t\
    _GetPropertySize(\
        const IOUserAudioObjectPropertyAddress * in_prop_addr,\
        OSData * in_qualifier_data,\
        size_t * out_size) APPLE_KEXT_OVERRIDE;\
\
    virtual kern_return_t\
    _IsPropertySettable(\
        const IOUserAudioObjectPropertyAddress * in_prop_addr,\
        bool * out_is_settable) APPLE_KEXT_OVERRIDE;\
\
    virtual kern_return_t\
    _HasProperty(\
        const IOUserAudioObjectPropertyAddress * in_prop_addr,\
        bool * out_has_property) APPLE_KEXT_OVERRIDE;\
\
    virtual bool\
    init(\
        IOUserAudioDriver * in_driver,\
        bool in_is_settable,\
        IOUserAudioObjectPropertyElement in_control_element,\
        IOUserAudioObjectPropertyScope in_control_scope) APPLE_KEXT_OVERRIDE;\
\
    virtual void\
    free(\
) APPLE_KEXT_OVERRIDE;\
\
    virtual IOUserAudioClassID\
    GetClassID(\
) APPLE_KEXT_OVERRIDE;\
\
    virtual IOUserAudioClassID\
    GetBaseClassID(\
) APPLE_KEXT_OVERRIDE;\
\


#if !KERNEL

extern OSMetaClass          * gIOUserAudioControlMetaClass;
extern const OSClassLoadInformation IOUserAudioControl_Class;

class IOUserAudioControlMetaClass : public OSMetaClass
{
public:
    virtual kern_return_t
    New(OSObject * instance) override;
};

#endif /* !KERNEL */

#if !KERNEL

class  IOUserAudioControlInterface : public OSInterface
{
public:
    virtual bool
    init(IOUserAudioDriver * in_driver,
        bool in_is_settable,
        IOUserAudioObjectPropertyElement in_control_element,
        IOUserAudioObjectPropertyScope in_control_scope) = 0;

    bool
    init_Call(IOUserAudioDriver * in_driver,
        bool in_is_settable,
        IOUserAudioObjectPropertyElement in_control_element,
        IOUserAudioObjectPropertyScope in_control_scope)  { return init(in_driver, in_is_settable, in_control_element, in_control_scope); };\

};

struct IOUserAudioControl_IVars;
struct IOUserAudioControl_LocalIVars;

class IOUserAudioControl : public IOUserAudioObject, public IOUserAudioControlInterface
{
#if !KERNEL
    friend class IOUserAudioControlMetaClass;
#endif /* !KERNEL */

#if !KERNEL
public:
#ifdef IOUserAudioControl_DECLARE_IVARS
IOUserAudioControl_DECLARE_IVARS
#else /* IOUserAudioControl_DECLARE_IVARS */
    union
    {
        IOUserAudioControl_IVars * ivars;
        IOUserAudioControl_LocalIVars * lvars;
    };
#endif /* IOUserAudioControl_DECLARE_IVARS */
#endif /* !KERNEL */

#if !KERNEL
    static OSMetaClass *
    sGetMetaClass() { return gIOUserAudioControlMetaClass; };
#endif /* KERNEL */

    using super = IOUserAudioObject;

#if !KERNEL
    IOUserAudioControl_Methods
    IOUserAudioControl_VirtualMethods
#endif /* !KERNEL */

};
#endif /* !KERNEL */


#endif /* !__DOCUMENTATION__ */

/* IOUserAudioControl.iig:153-154 */

#pragma mark Private Class Extension
/* IOUserAudioControl.iig:176- */

#endif /* IOUserAudioControl_h */
