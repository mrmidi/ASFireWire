/* iig(DriverKit-456.120.3) generated from IOUserAudioStereoPanControl.iig */

/* IOUserAudioStereoPanControl.iig:1-40 */
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

#ifndef IOUserAudioStereoPanControl_h
#define IOUserAudioStereoPanControl_h

#include <DriverKit/IOService.h>  /* .iig include */
#include <AudioDriverKit/IOUserAudioControl.h>  /* .iig include */
#include <AudioDriverKit/AudioDriverKitTypes.h>

using namespace AudioDriverKit;

class IOUserAudioDriver;
class IODispatchQueue;

/* source class IOUserAudioStereoPanControl IOUserAudioStereoPanControl.iig:41-269 */

#if __DOCUMENTATION__
#define KERNEL IIG_KERNEL

/*!
 * @class IOUserAudioStereoPanControl
 *
 * @brief
 * IOUserAudioStereoPanControl is a subclass of IOUserAudioControl
 *
 * @discussion
 * Control object that supports panning between stereo channels
 */
class LOCALONLY IOUserAudioStereoPanControl: public IOUserAudioControl
{
public:
    /*!
     * @function Create
     *
     * @abstract
     * static factory method to allocate and initialize an IOUserAudioStereoPanControl.
     *
     * @discussion
     * If IOUserAudioStereoPanControl is subclassed to override behavior, Create should not be
     * used to allocate/initialize the custom subclass.
     *
     * @param in_driver
     * The IOUserAudioDriver that owns this object.
     *
     * @param in_is_settable
     * A bool value indicating if the control value can be set
     *
     * @param in_control_value
     * A float for the control's current stereo pan value
     *
     * @param in_left_element
     * The IOUserAudioObjectPropertyElement for the left channel
     *
     * @param in_right_element
     * The IOUserAudioObjectPropertyElement for the right channel
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
     * OSSharedPtr to an IOUserAudioStereoPanControl if it was successfully allocated and initialized
     */
    static OSSharedPtr<IOUserAudioStereoPanControl> Create(IOUserAudioDriver* in_driver,
                                                           bool in_is_settable,
                                                           float in_control_value,
                                                           IOUserAudioObjectPropertyElement in_left_channel,
                                                           IOUserAudioObjectPropertyElement in_right_channel,
                                                           IOUserAudioObjectPropertyElement in_control_element,
                                                           IOUserAudioObjectPropertyScope in_control_scope,
                                                           IOUserAudioClassID in_control_class_id);

	/*!
	 * @function init
	 *
	 * @abstract
	 * Initializes a IOUserAudioStereoPanControl.
	 *
	 * @param in_driver
	 * The IOUserAudioDriver that owns this object.
	 *
	 * @param in_is_settable
	 * A bool value indicating if the control value can be set
	 *
	 * @param in_control_value
	 * A float for the control's current stereo pan value
	 *
	 * @param in_left_element
	 * The IOUserAudioObjectPropertyElement for the left channel
	 *
	 * @param in_right_element
	 * The IOUserAudioObjectPropertyElement for the right channel
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
					  float in_control_value,
					  IOUserAudioObjectPropertyElement in_left_channel,
					  IOUserAudioObjectPropertyElement in_right_channel,
					  IOUserAudioObjectPropertyElement in_control_element,
					  IOUserAudioObjectPropertyScope in_control_scope,
					  IOUserAudioClassID in_control_class_id);

	/*!
	 * @function free
	 *
	 * @abstract
	 * frees the IOUserAudioStereoPanControl.
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
	 * The float value attempting to be set on the control.
	 *
	 * @return
	 * Returns kIOReturnSuccess on sucess. Upon sucess the controls value should be updated.
	 */
	virtual kern_return_t HandleChangeControlValue(float in_control_value);

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
	 * Returns float
	 */
	float GetControlValue();

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
	 * float stereo pan value.
	 *
	 * @return
	 * Returns kern_return_t.
	 */
	kern_return_t SetControlValue(float in_control_value);
	
	/*!
	 * @function SetPanningChannels
	 *
	 * @abstract
	 * Set the current stereo panning channels.
	 *
	 * @discussion
	 * Changing the panning channels will send a notification to the host to update the object state if successful.
	 * Setting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_left_channel
	 * IOUserAudioObjectPropertyElement for the left channel
	 *
	 * @param in_right_channel
	 * IOUserAudioObjectPropertyElement for the right channel
	 *
	 * @return
	 * Returns kern_return_t.
	 */
	kern_return_t SetPanningChannels(IOUserAudioObjectPropertyElement in_left_channel,
									 IOUserAudioObjectPropertyElement in_right_channel);
	
	/*!
	 * @function GetPanningChannels
	 *
	 * @abstract
	 * Get the current stereo panning channels.
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @param out_left_channel
	 * IOUserAudioObjectPropertyElement for the left channel
	 *
	 * @param out_right_channel
	 * IOUserAudioObjectPropertyElement for the right channel
	 */
	void GetPanningChannels(IOUserAudioObjectPropertyElement* out_left_channel,
							IOUserAudioObjectPropertyElement* out_right_channel);
};

#undef KERNEL
#else /* __DOCUMENTATION__ */

/* generated class IOUserAudioStereoPanControl IOUserAudioStereoPanControl.iig:41-269 */


#define IOUserAudioStereoPanControl_Methods \
\
public:\
\
    static OSSharedPtr<IOUserAudioStereoPanControl>\
    Create(\
        IOUserAudioDriver * in_driver,\
        bool in_is_settable,\
        float in_control_value,\
        IOUserAudioObjectPropertyElement in_left_channel,\
        IOUserAudioObjectPropertyElement in_right_channel,\
        IOUserAudioObjectPropertyElement in_control_element,\
        IOUserAudioObjectPropertyScope in_control_scope,\
        IOUserAudioClassID in_control_class_id);\
\
    float\
    GetControlValue(\
);\
\
    kern_return_t\
    SetControlValue(\
        float in_control_value);\
\
    kern_return_t\
    SetPanningChannels(\
        IOUserAudioObjectPropertyElement in_left_channel,\
        IOUserAudioObjectPropertyElement in_right_channel);\
\
    void\
    GetPanningChannels(\
        IOUserAudioObjectPropertyElement * out_left_channel,\
        IOUserAudioObjectPropertyElement * out_right_channel);\
\
\
protected:\
    /* _Impl methods */\
\
\
public:\
    /* _Invoke methods */\
\


#define IOUserAudioStereoPanControl_KernelMethods \
\
protected:\
    /* _Impl methods */\
\


#define IOUserAudioStereoPanControl_VirtualMethods \
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
        float in_control_value,\
        IOUserAudioObjectPropertyElement in_left_channel,\
        IOUserAudioObjectPropertyElement in_right_channel,\
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
        float in_control_value) APPLE_KEXT_OVERRIDE;\
\


#if !KERNEL

extern OSMetaClass          * gIOUserAudioStereoPanControlMetaClass;
extern const OSClassLoadInformation IOUserAudioStereoPanControl_Class;

class IOUserAudioStereoPanControlMetaClass : public OSMetaClass
{
public:
    virtual kern_return_t
    New(OSObject * instance) override;
};

#endif /* !KERNEL */

#if !KERNEL

class  IOUserAudioStereoPanControlInterface : public OSInterface
{
public:
    virtual bool
    init(IOUserAudioDriver * in_driver,
        bool in_is_settable,
        float in_control_value,
        IOUserAudioObjectPropertyElement in_left_channel,
        IOUserAudioObjectPropertyElement in_right_channel,
        IOUserAudioObjectPropertyElement in_control_element,
        IOUserAudioObjectPropertyScope in_control_scope,
        IOUserAudioClassID in_control_class_id) = 0;

    virtual kern_return_t
    HandleChangeControlValue(float in_control_value) = 0;

    bool
    init_Call(IOUserAudioDriver * in_driver,
        bool in_is_settable,
        float in_control_value,
        IOUserAudioObjectPropertyElement in_left_channel,
        IOUserAudioObjectPropertyElement in_right_channel,
        IOUserAudioObjectPropertyElement in_control_element,
        IOUserAudioObjectPropertyScope in_control_scope,
        IOUserAudioClassID in_control_class_id)  { return init(in_driver, in_is_settable, in_control_value, in_left_channel, in_right_channel, in_control_element, in_control_scope, in_control_class_id); };\

    kern_return_t
    HandleChangeControlValue_Call(float in_control_value)  { return HandleChangeControlValue(in_control_value); };\

};

struct IOUserAudioStereoPanControl_IVars;
struct IOUserAudioStereoPanControl_LocalIVars;

class IOUserAudioStereoPanControl : public IOUserAudioControl, public IOUserAudioStereoPanControlInterface
{
#if !KERNEL
    friend class IOUserAudioStereoPanControlMetaClass;
#endif /* !KERNEL */

#if !KERNEL
public:
#ifdef IOUserAudioStereoPanControl_DECLARE_IVARS
IOUserAudioStereoPanControl_DECLARE_IVARS
#else /* IOUserAudioStereoPanControl_DECLARE_IVARS */
    union
    {
        IOUserAudioStereoPanControl_IVars * ivars;
        IOUserAudioStereoPanControl_LocalIVars * lvars;
    };
#endif /* IOUserAudioStereoPanControl_DECLARE_IVARS */
#endif /* !KERNEL */

#if !KERNEL
    static OSMetaClass *
    sGetMetaClass() { return gIOUserAudioStereoPanControlMetaClass; };
#endif /* KERNEL */

    using super = IOUserAudioControl;

#if !KERNEL
    IOUserAudioStereoPanControl_Methods
    IOUserAudioStereoPanControl_VirtualMethods
#endif /* !KERNEL */

};
#endif /* !KERNEL */


#endif /* !__DOCUMENTATION__ */

/* IOUserAudioStereoPanControl.iig:271-272 */

#pragma mark Private Class Extension
/* IOUserAudioStereoPanControl.iig:294- */

#endif /* IOUserAudioStereoPanControl_h */
