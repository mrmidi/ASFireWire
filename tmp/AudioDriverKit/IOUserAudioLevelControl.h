/* iig(DriverKit-456.120.3) generated from IOUserAudioLevelControl.iig */

/* IOUserAudioLevelControl.iig:1-56 */
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

#ifndef IOUserAudioLevelControl_h
#define IOUserAudioLevelControl_h

#include <DriverKit/IOService.h>  /* .iig include */
#include <AudioDriverKit/IOUserAudioControl.h>  /* .iig include */
#include <AudioDriverKit/AudioDriverKitTypes.h>

using namespace AudioDriverKit;

class IOUserAudioDriver;
class IODispatchQueue;

/*!
 * @struct IOUserAudioLevelControlRange
 *
 * @brief
 * IOUserAudioLevelControlRange is a subclass of IOUserAudioControl
 *
 * @discussion
 * m_min is the minimum float value for the level control range
 * m_max is the maximum float value for the level control range
 */
struct IOUserAudioLevelControlRange
{
	float m_min;
	float m_max;
};

/* source class IOUserAudioLevelControl IOUserAudioLevelControl.iig:57-289 */

#if __DOCUMENTATION__
#define KERNEL IIG_KERNEL

/*!
 * @class IOUserAudioLevelControl
 *
 * @brief
 * IOUserAudioLevelControl is a subclass of IOUserAudioControl
 *
 * @discussion
 * Control object that supports a float value level.  Getting/Setting control values can be done
 * with scalar or decibel level values.
 */
class LOCALONLY IOUserAudioLevelControl: public IOUserAudioControl
{
public:
    /*!
     * @function Create
     *
     * @abstract
     * static factory method to allocate and initialize an IOUserAudioLevelControl.
     *
     * @discussion
     * If IOUserAudioLevelControl is subclassed to override behavior, Create should not be
     * used to allocate/initialize the custom subclass.
     *
     * @param in_driver
     * The IOUserAudioDriver that owns this object.
     *
     * @param in_is_settable
     * A bool value indicating if the control value can be set
     *
     * @param in_decibel_value
     * A float for the controls current decibel level value
     *
     * @param in_decibel_range
     * A IOUserAudioLevelControlRange for the controls decibe minimum and maximum range
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
     * OSSharedPtr to an IOUserAudioLevelControl if it was successfully allocated and initialized
     */
    static OSSharedPtr<IOUserAudioLevelControl> Create(IOUserAudioDriver* in_driver,
                                                       bool in_is_settable,
                                                       float in_decibel_value,
                                                       IOUserAudioLevelControlRange in_decibel_range,
                                                       IOUserAudioObjectPropertyElement in_control_element,
                                                       IOUserAudioObjectPropertyScope in_control_scope,
                                                       IOUserAudioClassID in_control_class_id);

	/*!
	 * @function init
	 *
	 * @abstract
	 * Initializes a IOUserAudioLevelControl.
	 *
	 * @param in_driver
	 * The IOUserAudioDriver that owns this object.
	 *
	 * @param in_is_settable
	 * A bool value indicating if the control value can be set
	 *
	 * @param in_decibel_value
	 * A float for the controls current decibel level value
	 *
	 * @param in_decibel_range
	 * A IOUserAudioLevelControlRange for the controls decibe minimum and maximum range
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
					  float in_decibel_value,
					  IOUserAudioLevelControlRange in_decibel_range,
					  IOUserAudioObjectPropertyElement in_control_element,
					  IOUserAudioObjectPropertyScope in_control_scope,
					  IOUserAudioClassID in_control_class_id);

	/*!
	 * @function free
	 *
	 * @abstract
	 * frees the IOUserAudioLevelControl.
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
	 * Default implementation will call SetDecibelValue() and return kIOReturnSuccess
	 * Subclass and override this method to handle changes to this control value and
	 * return kIOReturnSucess upon success.
	 *
	 * @param in_decibel_value
	 * The float decibel level value attempting to be set on the control.
	 *
	 * @return
	 * Returns kIOReturnSuccess on sucess. Upon sucess the control's value should be updated.
	 */
	virtual kern_return_t HandleChangeDecibelValue(float in_decibel_value);

	/*!
	 * @function HandleChangeControlValue
	 *
	 * @abstract
	 * Virtual method will be called when the controls value will be changed.
	 *
	 * @discussion
	 * Default implementation will call SetScalarValue() and return kIOReturnSuccess.
	 * Subclass and override this method to handle changes to this control value and
	 * return kIOReturnSucess upon success.
	 *
	 * @param in_scalar_value
	 * The float scalar level value attempting to be set on the control.
	 *
	 * @return
	 * Returns kIOReturnSuccess on sucess. Upon sucess the control's value should be updated.
	 */
	virtual kern_return_t HandleChangeScalarValue(float in_scalar_value);

#pragma mark Setters/Getters
	/*!
	 * @function GetDecibelValue
	 *
	 * @abstract
	 * Get the decibel level value for the control.
	 *
	 * @discussion
	 * Getting the control value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns float.
	 */
	float GetDecibelValue();
	
	/*!
	 * @function SetDecibelValue
	 *
	 * @abstract
	 * Set the current decibel level value.
	 *
	 * @discussion
	 * Changing the decibel level value will send a notification to the host to update the object state if successful.
	 * Setting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_decibel_value
	 * float decibel level value
	 *
	 * @return
	 * Returns kern_return_t.
	 */
	kern_return_t SetDecibelValue(float in_decibel_value);

	/*!
	 * @function GetScalarValue
	 *
	 * @abstract
	 * Get the scalar level value for the control.
	 *
	 * @discussion
	 * Getting the control value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns float.
	 */
	float GetScalarValue();
	
	/*!
	 * @function SetScalarValue
	 *
	 * @abstract
	 * Set the current scalar level value.
	 *
	 * @discussion
	 * Changing the scalar level value will send a notification to the host to update the object state if successful.
	 * Setting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_decibel_value
	 * float scalar level value
	 *
	 * @return
	 * Returns kern_return_t.
	 */
	kern_return_t SetScalarValue(float in_scalar);
};

#undef KERNEL
#else /* __DOCUMENTATION__ */

/* generated class IOUserAudioLevelControl IOUserAudioLevelControl.iig:57-289 */


#define IOUserAudioLevelControl_Methods \
\
public:\
\
    float\
    _GetDecibelFromScalarValue(\
        float in_scalar_value);\
\
    float\
    _GetScalarFromDecibelValue(\
        float in_decibel_value);\
\
    static OSSharedPtr<IOUserAudioLevelControl>\
    Create(\
        IOUserAudioDriver * in_driver,\
        bool in_is_settable,\
        float in_decibel_value,\
        IOUserAudioLevelControlRange in_decibel_range,\
        IOUserAudioObjectPropertyElement in_control_element,\
        IOUserAudioObjectPropertyScope in_control_scope,\
        IOUserAudioClassID in_control_class_id);\
\
    float\
    GetDecibelValue(\
);\
\
    kern_return_t\
    SetDecibelValue(\
        float in_decibel_value);\
\
    float\
    GetScalarValue(\
);\
\
    kern_return_t\
    SetScalarValue(\
        float in_scalar);\
\
\
protected:\
    /* _Impl methods */\
\
\
public:\
    /* _Invoke methods */\
\


#define IOUserAudioLevelControl_KernelMethods \
\
protected:\
    /* _Impl methods */\
\


#define IOUserAudioLevelControl_VirtualMethods \
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
        float in_decibel_value,\
        IOUserAudioLevelControlRange in_decibel_range,\
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
    HandleChangeDecibelValue(\
        float in_decibel_value) APPLE_KEXT_OVERRIDE;\
\
    virtual kern_return_t\
    HandleChangeScalarValue(\
        float in_scalar_value) APPLE_KEXT_OVERRIDE;\
\


#if !KERNEL

extern OSMetaClass          * gIOUserAudioLevelControlMetaClass;
extern const OSClassLoadInformation IOUserAudioLevelControl_Class;

class IOUserAudioLevelControlMetaClass : public OSMetaClass
{
public:
    virtual kern_return_t
    New(OSObject * instance) override;
};

#endif /* !KERNEL */

#if !KERNEL

class  IOUserAudioLevelControlInterface : public OSInterface
{
public:
    virtual bool
    init(IOUserAudioDriver * in_driver,
        bool in_is_settable,
        float in_decibel_value,
        IOUserAudioLevelControlRange in_decibel_range,
        IOUserAudioObjectPropertyElement in_control_element,
        IOUserAudioObjectPropertyScope in_control_scope,
        IOUserAudioClassID in_control_class_id) = 0;

    virtual kern_return_t
    HandleChangeDecibelValue(float in_decibel_value) = 0;

    virtual kern_return_t
    HandleChangeScalarValue(float in_scalar_value) = 0;

    bool
    init_Call(IOUserAudioDriver * in_driver,
        bool in_is_settable,
        float in_decibel_value,
        IOUserAudioLevelControlRange in_decibel_range,
        IOUserAudioObjectPropertyElement in_control_element,
        IOUserAudioObjectPropertyScope in_control_scope,
        IOUserAudioClassID in_control_class_id)  { return init(in_driver, in_is_settable, in_decibel_value, in_decibel_range, in_control_element, in_control_scope, in_control_class_id); };\

    kern_return_t
    HandleChangeDecibelValue_Call(float in_decibel_value)  { return HandleChangeDecibelValue(in_decibel_value); };\

    kern_return_t
    HandleChangeScalarValue_Call(float in_scalar_value)  { return HandleChangeScalarValue(in_scalar_value); };\

};

struct IOUserAudioLevelControl_IVars;
struct IOUserAudioLevelControl_LocalIVars;

class IOUserAudioLevelControl : public IOUserAudioControl, public IOUserAudioLevelControlInterface
{
#if !KERNEL
    friend class IOUserAudioLevelControlMetaClass;
#endif /* !KERNEL */

#if !KERNEL
public:
#ifdef IOUserAudioLevelControl_DECLARE_IVARS
IOUserAudioLevelControl_DECLARE_IVARS
#else /* IOUserAudioLevelControl_DECLARE_IVARS */
    union
    {
        IOUserAudioLevelControl_IVars * ivars;
        IOUserAudioLevelControl_LocalIVars * lvars;
    };
#endif /* IOUserAudioLevelControl_DECLARE_IVARS */
#endif /* !KERNEL */

#if !KERNEL
    static OSMetaClass *
    sGetMetaClass() { return gIOUserAudioLevelControlMetaClass; };
#endif /* KERNEL */

    using super = IOUserAudioControl;

#if !KERNEL
    IOUserAudioLevelControl_Methods
    IOUserAudioLevelControl_VirtualMethods
#endif /* !KERNEL */

};
#endif /* !KERNEL */


#endif /* !__DOCUMENTATION__ */

/* IOUserAudioLevelControl.iig:291-292 */

#pragma mark Private Class Extension
/* IOUserAudioLevelControl.iig:317- */

#endif /* IOUserAudioLevelControl_h */
