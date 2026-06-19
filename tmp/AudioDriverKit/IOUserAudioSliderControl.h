/* iig(DriverKit-456.120.3) generated from IOUserAudioSliderControl.iig */

/* IOUserAudioSliderControl.iig:1-51 */
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

#ifndef IOUserAudioSliderControl_h
#define IOUserAudioSliderControl_h

#include <DriverKit/IOService.h>  /* .iig include */
#include <AudioDriverKit/IOUserAudioControl.h>  /* .iig include */
#include <AudioDriverKit/AudioDriverKitTypes.h>

using namespace AudioDriverKit;

class IOUserAudioDriver;
class IODispatchQueue;

/*!
 * @struct IOUserAudioSliderRange
 *
 * @brief
 * IOUserAudioSliderRange set the minimum and maximum range for the slider value
 */
struct IOUserAudioSliderRange {
	uint32_t m_min;
	uint32_t m_max;
};

/* source class IOUserAudioSliderControl IOUserAudioSliderControl.iig:52-264 */

#if __DOCUMENTATION__
#define KERNEL IIG_KERNEL

/*!
 * @class IOUserAudioSliderControl
 *
 * @brief
 * IOUserAudioSliderControl is a subclass of IOUserAudioControl
 *
 * @discussion
 * Control object that supports a uint32_t value slider
 */
class LOCALONLY IOUserAudioSliderControl: public IOUserAudioControl
{
public:
    /*!
     * @function Create
     *
     * @abstract
     * static factory method to allocate and initialize an IOUserAudioSliderControl.
     *
     * @discussion
     * If IOUserAudioSliderControl is subclassed to override behavior, Create should not be
     * used to allocate/initialize the custom subclass.
     *
     * @param in_driver
     * The IOUserAudioDriver that owns this object.
     *
     * @param in_is_settable
     * A bool value indicating if the control value can be set
     *
     * @param in_control_value
     * A uint32_t for the control's current slider value
     *
     * @param in_range
     * The IOUserAudioSliderRange for control
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
     * OSSharedPtr to an IOUserAudioSliderControl if it was successfully allocated and initialized
     */
    static OSSharedPtr<IOUserAudioSliderControl> Create(IOUserAudioDriver* in_driver,
                                                        bool in_is_settable,
                                                        uint32_t in_control_value,
                                                        IOUserAudioSliderRange in_range,
                                                        IOUserAudioObjectPropertyElement in_control_element,
                                                        IOUserAudioObjectPropertyScope in_control_scope,
                                                        IOUserAudioClassID in_control_class_id);

	/*!
	 * @function init
	 *
	 * @abstract
	 * Initializes a IOUserAudioSliderControl.
	 *
	 * @param in_driver
	 * The IOUserAudioDriver that owns this object.
	 *
	 * @param in_is_settable
	 * A bool value indicating if the control value can be set
	 *
	 * @param in_control_value
	 * A uint32_t for the control's current slider value
	 *
	 * @param in_range
	 * The IOUserAudioSliderRange for control
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
					  uint32_t in_control_value,
					  IOUserAudioSliderRange in_range,
					  IOUserAudioObjectPropertyElement in_control_element,
					  IOUserAudioObjectPropertyScope in_control_scope,
					  IOUserAudioClassID in_control_class_id);

	/*!
	 * @function free
	 *
	 * @abstract
	 * frees the IOUserAudioSliderControl.
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
	 * The uint32_t value attempting to be set on the control.
	 *
	 * @return
	 * Returns kIOReturnSuccess on sucess. Upon sucess the control's value should be updated.
	 */
	virtual kern_return_t HandleChangeControlValue(uint32_t in_control_value);

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
	 * Returns uint32_t
	 */
	uint32_t GetControlValue();

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
	 * uint32_t slider control value
	 *
	 * @return
	 * Returns kern_return_t.
	 */
	kern_return_t SetControlValue(uint32_t in_control_value);
	
	/*!
	 * @function GetRange
	 *
	 * @abstract
	 * Get the current range of the slider control.
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns IOUserAudioSliderRange
	 */
	IOUserAudioSliderRange GetRange();
	
	/*!
	 * @function SetRange
	 *
	 * @abstract
	 * Set the current range of the slider control.
	 *
	 * @discussion
	 * Changing the range will send a notification to the host to update the object state if successful.
	 * Setting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_range
	 * IOUserAudioSliderRange slider control range
	 *
	 * @return
	 * Returns kern_return_t.
	 */
	kern_return_t SetRange(IOUserAudioSliderRange in_range);
};

#undef KERNEL
#else /* __DOCUMENTATION__ */

/* generated class IOUserAudioSliderControl IOUserAudioSliderControl.iig:52-264 */


#define IOUserAudioSliderControl_Methods \
\
public:\
\
    static OSSharedPtr<IOUserAudioSliderControl>\
    Create(\
        IOUserAudioDriver * in_driver,\
        bool in_is_settable,\
        uint32_t in_control_value,\
        IOUserAudioSliderRange in_range,\
        IOUserAudioObjectPropertyElement in_control_element,\
        IOUserAudioObjectPropertyScope in_control_scope,\
        IOUserAudioClassID in_control_class_id);\
\
    uint32_t\
    GetControlValue(\
);\
\
    kern_return_t\
    SetControlValue(\
        uint32_t in_control_value);\
\
    IOUserAudioSliderRange\
    GetRange(\
);\
\
    kern_return_t\
    SetRange(\
        IOUserAudioSliderRange in_range);\
\
\
protected:\
    /* _Impl methods */\
\
\
public:\
    /* _Invoke methods */\
\


#define IOUserAudioSliderControl_KernelMethods \
\
protected:\
    /* _Impl methods */\
\


#define IOUserAudioSliderControl_VirtualMethods \
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
        uint32_t in_control_value,\
        IOUserAudioSliderRange in_range,\
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
        uint32_t in_control_value) APPLE_KEXT_OVERRIDE;\
\


#if !KERNEL

extern OSMetaClass          * gIOUserAudioSliderControlMetaClass;
extern const OSClassLoadInformation IOUserAudioSliderControl_Class;

class IOUserAudioSliderControlMetaClass : public OSMetaClass
{
public:
    virtual kern_return_t
    New(OSObject * instance) override;
};

#endif /* !KERNEL */

#if !KERNEL

class  IOUserAudioSliderControlInterface : public OSInterface
{
public:
    virtual bool
    init(IOUserAudioDriver * in_driver,
        bool in_is_settable,
        uint32_t in_control_value,
        IOUserAudioSliderRange in_range,
        IOUserAudioObjectPropertyElement in_control_element,
        IOUserAudioObjectPropertyScope in_control_scope,
        IOUserAudioClassID in_control_class_id) = 0;

    virtual kern_return_t
    HandleChangeControlValue(uint32_t in_control_value) = 0;

    bool
    init_Call(IOUserAudioDriver * in_driver,
        bool in_is_settable,
        uint32_t in_control_value,
        IOUserAudioSliderRange in_range,
        IOUserAudioObjectPropertyElement in_control_element,
        IOUserAudioObjectPropertyScope in_control_scope,
        IOUserAudioClassID in_control_class_id)  { return init(in_driver, in_is_settable, in_control_value, in_range, in_control_element, in_control_scope, in_control_class_id); };\

    kern_return_t
    HandleChangeControlValue_Call(uint32_t in_control_value)  { return HandleChangeControlValue(in_control_value); };\

};

struct IOUserAudioSliderControl_IVars;
struct IOUserAudioSliderControl_LocalIVars;

class IOUserAudioSliderControl : public IOUserAudioControl, public IOUserAudioSliderControlInterface
{
#if !KERNEL
    friend class IOUserAudioSliderControlMetaClass;
#endif /* !KERNEL */

#if !KERNEL
public:
#ifdef IOUserAudioSliderControl_DECLARE_IVARS
IOUserAudioSliderControl_DECLARE_IVARS
#else /* IOUserAudioSliderControl_DECLARE_IVARS */
    union
    {
        IOUserAudioSliderControl_IVars * ivars;
        IOUserAudioSliderControl_LocalIVars * lvars;
    };
#endif /* IOUserAudioSliderControl_DECLARE_IVARS */
#endif /* !KERNEL */

#if !KERNEL
    static OSMetaClass *
    sGetMetaClass() { return gIOUserAudioSliderControlMetaClass; };
#endif /* KERNEL */

    using super = IOUserAudioControl;

#if !KERNEL
    IOUserAudioSliderControl_Methods
    IOUserAudioSliderControl_VirtualMethods
#endif /* !KERNEL */

};
#endif /* !KERNEL */


#endif /* !__DOCUMENTATION__ */

/* IOUserAudioSliderControl.iig:266-267 */

#pragma mark Private Class Extension
/* IOUserAudioSliderControl.iig:289- */

#endif /* IOUserAudioSliderControl_h */
