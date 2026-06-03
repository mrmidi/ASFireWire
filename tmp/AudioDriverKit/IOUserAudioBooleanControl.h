/* iig(DriverKit-456.120.3) generated from IOUserAudioBooleanControl.iig */

/* IOUserAudioBooleanControl.iig:1-40 */
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

#ifndef IOUserAudioBooleanControl_h
#define IOUserAudioBooleanControl_h

#include <DriverKit/IOService.h>  /* .iig include */
#include <AudioDriverKit/IOUserAudioControl.h>  /* .iig include */
#include <AudioDriverKit/AudioDriverKitTypes.h>

using namespace AudioDriverKit;

class IOUserAudioDriver;
class IODispatchQueue;

/* source class IOUserAudioBooleanControl IOUserAudioBooleanControl.iig:41-213 */

#if __DOCUMENTATION__
#define KERNEL IIG_KERNEL

/*!
 * @class IOUserAudioBooleanControl
 *
 * @brief
 * IOUserAudioBooleanControl is a subclass of IOUserAudioControl
 *
 * @discussion
 * Control object that supports boolean value
 */
class LOCALONLY IOUserAudioBooleanControl: public IOUserAudioControl
{
public:
    /*!
     * @function Create
     *
     * @abstract
     * static factory method to allocate and initialize an IOUserAudioBooleanControl.
     *
     * @discussion
     * If IOUserAudioBooleanControl is subclassed to override behavior, Create should not be
     * used to allocate/initialize the custom subclass.
     *
     * @param in_driver
     * The IOUserAudioDriver that owns this object.
     *
     * @param in_is_settable
     * A bool value indicating if the control value can be set
     *
     * @param in_control_value
     * A bool for the control's current value
     *
     * @param in_control_element
     * The IOUserAudioObjectPropertyElement for the control
     *
     * @param in_control_scope
     * The IOUserAudioObjectPropertyScope for the control
     *
     * @param in_control_class_id
     * The IOUserAudioClassID of the control
     *
     * @return
     * OSSharedPtr to an IOUserAudioBooleanControl if it was successfully allocated and initialized
     */
    static OSSharedPtr<IOUserAudioBooleanControl> Create(IOUserAudioDriver* in_driver,
                                                         bool in_is_settable,
                                                         bool in_control_value,
                                                         IOUserAudioObjectPropertyElement in_control_element,
                                                         IOUserAudioObjectPropertyScope in_control_scope,
                                                         IOUserAudioClassID in_control_class_id);

	/*!
	 * @function init
	 *
	 * @abstract
	 * Initializes a IOUserAudioBooleanControl.
	 *
	 * @param in_driver
	 * The IOUserAudioDriver that owns this object.
	 *
	 * @param in_is_settable
	 * A bool value indicating if the control value can be set
	 *
	 * @param in_control_value
	 * A bool for the control's current value
	 *
	 * @param in_control_element
	 * The IOUserAudioObjectPropertyElement for the control
	 *
	 * @param in_control_scope
	 * The IOUserAudioObjectPropertyScope for the control
	 *
	 * @param in_control_class_id
	 * The IOUserAudioClassID of the control
	 *
	 * @return
	 * true on success.
	 */
	virtual bool init(IOUserAudioDriver* in_driver,
					  bool in_is_settable,
					  bool in_control_value,
					  IOUserAudioObjectPropertyElement in_control_element,
					  IOUserAudioObjectPropertyScope in_control_scope,
					  IOUserAudioClassID in_control_class_id);

	/*!
	 * @function free
	 *
	 * @abstract
	 * frees the IOUserAudioBooleanControl.
	 */
	virtual void free() override;

#pragma mark IOUserAudioObject overrides
	/*!
	 * @function GetClassID
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

#pragma mark Overridable Change methods
	/*!
	 * @function HandleChangeControlValue
	 *
	 * @abstract
	 * Virtual method will be called when the controls value will be changed.
	 *
	 * @discussion
	 * Default implementation will call SetControlValue() and return kIOReturnSuccess.
	 * Subclass and override this method to handle changes to this control value and
	 * return kIOReturnSucess upon success.
	 *
	 * @param in_control_value
	 * The bool value attempting to be set on the control.
	 *
	 * @return
	 * Returns kIOReturnSuccess on sucess. Upon sucess the control's value should be updated.
	 */
	virtual kern_return_t HandleChangeControlValue(bool in_control_value);

#pragma mark Setters/Getters
	/*!
	 * @function GetControlValue
	 *
	 * @abstract
	 * Get the current value of the control.
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns bool
	 */
	bool GetControlValue();
	
	/*!
	 * @function SetControlValue
	 *
	 * @abstract
	 * Set the current control value.
	 *
	 * @discussion
	 * Changing the control value will send a notification to the host to update the object state if successful.
	 * Setting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_control_value
	 * bool control value.
	 *
	 * @return
	 * Returns kern_return_t.
	 */
	kern_return_t SetControlValue(bool in_control_value);
};

#undef KERNEL
#else /* __DOCUMENTATION__ */

/* generated class IOUserAudioBooleanControl IOUserAudioBooleanControl.iig:41-213 */


#define IOUserAudioBooleanControl_Methods \
\
public:\
\
    static OSSharedPtr<IOUserAudioBooleanControl>\
    Create(\
        IOUserAudioDriver * in_driver,\
        bool in_is_settable,\
        bool in_control_value,\
        IOUserAudioObjectPropertyElement in_control_element,\
        IOUserAudioObjectPropertyScope in_control_scope,\
        IOUserAudioClassID in_control_class_id);\
\
    bool\
    GetControlValue(\
);\
\
    kern_return_t\
    SetControlValue(\
        bool in_control_value);\
\
\
protected:\
    /* _Impl methods */\
\
\
public:\
    /* _Invoke methods */\
\


#define IOUserAudioBooleanControl_KernelMethods \
\
protected:\
    /* _Impl methods */\
\


#define IOUserAudioBooleanControl_VirtualMethods \
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
        bool in_control_value,\
        IOUserAudioObjectPropertyElement in_control_element,\
        IOUserAudioObjectPropertyScope in_control_scope,\
        IOUserAudioClassID in_control_class_id) APPLE_KEXT_OVERRIDE;\
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
    virtual kern_return_t\
    HandleChangeControlValue(\
        bool in_control_value) APPLE_KEXT_OVERRIDE;\
\


#if !KERNEL

extern OSMetaClass          * gIOUserAudioBooleanControlMetaClass;
extern const OSClassLoadInformation IOUserAudioBooleanControl_Class;

class IOUserAudioBooleanControlMetaClass : public OSMetaClass
{
public:
    virtual kern_return_t
    New(OSObject * instance) override;
};

#endif /* !KERNEL */

#if !KERNEL

class  IOUserAudioBooleanControlInterface : public OSInterface
{
public:
    virtual bool
    init(IOUserAudioDriver * in_driver,
        bool in_is_settable,
        bool in_control_value,
        IOUserAudioObjectPropertyElement in_control_element,
        IOUserAudioObjectPropertyScope in_control_scope,
        IOUserAudioClassID in_control_class_id) = 0;

    virtual kern_return_t
    HandleChangeControlValue(bool in_control_value) = 0;

    bool
    init_Call(IOUserAudioDriver * in_driver,
        bool in_is_settable,
        bool in_control_value,
        IOUserAudioObjectPropertyElement in_control_element,
        IOUserAudioObjectPropertyScope in_control_scope,
        IOUserAudioClassID in_control_class_id)  { return init(in_driver, in_is_settable, in_control_value, in_control_element, in_control_scope, in_control_class_id); };\

    kern_return_t
    HandleChangeControlValue_Call(bool in_control_value)  { return HandleChangeControlValue(in_control_value); };\

};

struct IOUserAudioBooleanControl_IVars;
struct IOUserAudioBooleanControl_LocalIVars;

class IOUserAudioBooleanControl : public IOUserAudioControl, public IOUserAudioBooleanControlInterface
{
#if !KERNEL
    friend class IOUserAudioBooleanControlMetaClass;
#endif /* !KERNEL */

#if !KERNEL
public:
#ifdef IOUserAudioBooleanControl_DECLARE_IVARS
IOUserAudioBooleanControl_DECLARE_IVARS
#else /* IOUserAudioBooleanControl_DECLARE_IVARS */
    union
    {
        IOUserAudioBooleanControl_IVars * ivars;
        IOUserAudioBooleanControl_LocalIVars * lvars;
    };
#endif /* IOUserAudioBooleanControl_DECLARE_IVARS */
#endif /* !KERNEL */

#if !KERNEL
    static OSMetaClass *
    sGetMetaClass() { return gIOUserAudioBooleanControlMetaClass; };
#endif /* KERNEL */

    using super = IOUserAudioControl;

#if !KERNEL
    IOUserAudioBooleanControl_Methods
    IOUserAudioBooleanControl_VirtualMethods
#endif /* !KERNEL */

};
#endif /* !KERNEL */


#endif /* !__DOCUMENTATION__ */

/* IOUserAudioBooleanControl.iig:215-216 */

#pragma mark Private Class Extension
/* IOUserAudioBooleanControl.iig:238- */

#endif /* IOUserAudioBooleanControl_h */
