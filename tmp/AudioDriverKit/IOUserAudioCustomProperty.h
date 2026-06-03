/* iig(DriverKit-456.120.3) generated from IOUserAudioCustomProperty.iig */

/* IOUserAudioCustomProperty.iig:1-39 */
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

#ifndef IOUserAudioCustomProperty_h
#define IOUserAudioCustomProperty_h

#include <DriverKit/OSObject.h>  /* .iig include */
#include <AudioDriverKit/IOUserAudioObject.h>  /* .iig include */
#include <AudioDriverKit/AudioDriverKitTypes.h>

using namespace AudioDriverKit;

class IOUserAudioDriver;

/* source class IOUserAudioCustomProperty IOUserAudioCustomProperty.iig:40-249 */

#if __DOCUMENTATION__
#define KERNEL IIG_KERNEL

/*!
 * @class IOUserAudioCustomProperty
 *
 * @brief
 * Custom property object that can be added/associated to IOUserAudio objects.
 *
 * @discussion
 * Custom properties can be added to the following objects: IOUserAudioControl, IOUserAudioBox, IOUserAudioStream,
 * IOUserAudioClockDevice, IOUserAudioDevice, IOUserAudioDriver.
 * Custom properites have qualifier and data types of OSString, OSDictionary, or OSData.
 */

class LOCALONLY IOUserAudioCustomProperty: public IOUserAudioObject
{
public:
    
    /*!
     * @function Create
     *
     * @abstract
     * static factory method to allocate and initialize an IOUserAudioCustomProperty.
     *
     * @discussion
     * If IOUserAudioCustomProperty is subclassed to override behavior, Create should not be
     * used to allocate/initialize the custom subclass.
     *
     * @param in_audio_driver
     * The IOUserAudioDriver that owns this object.
     *
     * @param in_prop_addr
     * The IOUserAudioObjectPropertyAddress of the custom property.
     *
     * @param in_is_property_settable
     * bool value that indicates if the property can be set.
     *
     * @param in_qualifier_data_type
     * The IOUserAudioCustomPropertyDataType for custom property's qualifier data value
     *
     * @param in_data_type
     * The IOUserAudioCustomPropertyDataType for custom property's data value. Value cannot be
	 * IOUserAudioCustomPropertyDataType::None
     *
     * @return
     * OSSharedPtr to an IOUserAudioBooleanControl if it was successfully allocated and initialized
     */
    static OSSharedPtr<IOUserAudioCustomProperty> Create(IOUserAudioDriver* in_audio_driver,
                                                         IOUserAudioObjectPropertyAddress in_prop_addr,
                                                         bool in_is_property_settable,
                                                         IOUserAudioCustomPropertyDataType in_qualifier_data_type,
                                                         IOUserAudioCustomPropertyDataType in_data_type);

	/*!
	 * @function init
	 *
	 * @abstract
	 * Initializes a IOUserAudioCustomProperty.
	 *
	 * @param in_audio_driver
	 * The IOUserAudioDriver that owns this object.
	 *
	 * @param in_prop_addr
	 * The IOUserAudioObjectPropertyAddress of the custom property.
	 *
	 * @param in_is_property_settable
	 * bool value that indicates if the property can be set.
	 *
	 * @param in_qualifier_data_type
	 * The IOUserAudioCustomPropertyDataType for custom property's qualifier data value
	 *
	 * @param in_data_type
	 * The IOUserAudioCustomPropertyDataType for custom property's data value
	 *
	 * @return
	 * true on success.
	 */
	virtual bool init(IOUserAudioDriver* in_audio_driver,
					  IOUserAudioObjectPropertyAddress in_prop_addr,
					  bool in_is_property_settable,
					  IOUserAudioCustomPropertyDataType in_qualifier_data_type,
					  IOUserAudioCustomPropertyDataType in_data_type);
	
	/*!
	 * @function free
	 *
	 * @abstract
	 * frees the IOUserAudioCustomProperty.
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
	 * @function HandleChangeCustomPropertyDataValueWithQualifier
	 *
	 * @abstract
	 * Virtual Method will be called when the custom property's data value will be changed.
	 *
	 * @discussion
	 * Default implementation will always return kIOReturnSuccess and update the custom
	 * property data value without checking qualifier contents.
	 * Subclass and override this method to handle changes to this custom property value and
	 * return kIOReturnSucess upon success.
	 *
	 * @param in_qualifier_data
	 * The qualifier data OSObject associated with setting the property data value.
	 * Can be a nullptr, OSString, or OSDictionary.
	 *
	 * @param in_data
	 * The data OSObject that is getting set for the custom property.
	 * Can be a OSString or OSDictionary.
	 *
	 * @return
	 * Returns kIOReturnSuccess on sucess. Upon sucess the custom property's data value should be updated.
	 */
	virtual	kern_return_t HandleChangeCustomPropertyDataValueWithQualifier(OSObject* in_qualifier_data,
																		   OSObject* in_data);

#pragma mark Custom Property Data Setters/Getters
	/*!
	 * @function SetCustomPropertyValue
	 *
	 * @abstract
	 * Set the custom propertie's data value.
	 *
	 * @param in_qualifier_data
	 * The qualifier data OSObject for the custom property that corresponds to the data value.
	 * Must be nullptr if qualifier data type is CustomPropertyDataTypeNone.
	 * Must be an OSString if qualifier data type is CustomPropertyDataTypeOSString.
	 * Must be an OSDictionary if qualifier data type is CustomPropertyDataTypeOSDictionary.
	 *
	 * @param in_data
	 * The data OSObject for the custom property that corresponds to the qualifier.
	 * Must be an OSString if data type is CustomPropertyDataTypeOSString.
	 * Must be an OSDictionary if data type is CustomPropertyDataTypeOSDictionary.
	 * Value cannot be a nullptr.
	 *
	 * @return
	 * Returns kIOReturnSuccess on sucess.
	 */
	kern_return_t SetQualifierAndDataValue(OSObject* in_qualifier_data, OSObject* in_data);
	
	/*!
	 * @function GetCustomPropertyValueWithQualifier
	 *
	 * @abstract
	 * Get the custom property value for a given qualifier
	 *
	 * @discussion
	 * Base class will return the custom property value set on the object without looking at contents of
	 * the qualifier data.  If the value returned is dependent on qualfier, IOUserAudioCustomProperty should
	 * be subclassed and derived class should override this method.
	 *
	 * @param in_qualifier_data
	 * The OSObject that is used to qualify the custom property data value.  in_qualifier_data can be a nullptr
	 * if custom property value does not require qualifier data.
	 *
	 * @param out_data
	 * Returned OSObject that is retained and to be released by the caller.
	 *
	 * @return
	 * Returns kIOReturnSuccess on sucess.
	 */
	virtual kern_return_t GetCustomPropertyValueWithQualifier(OSObject* in_qualifier_data,
															  OSObject** out_data);
	
	/*!
	 * @function GetCustomPropertyInfo
	 *
	 * @abstract
	 * Get the custom property information IOUserAudioCustomPropertyInfo.
	 *
	 * @return
	 * Returns IOUserAudioCustomPropertyInfo for the custom property.
	 */
	IOUserAudioCustomPropertyInfo GetCustomPropertyInfo();
	
	/*!
	 * @function AddCustomProperty
	 *
	 * @abstract
	 * Will always return kIOReturnError since a custom property cannot have a custom property
	 *
	 * @return
	 * Returns kIOReturnError
	 */
	virtual kern_return_t AddCustomProperty(IOUserAudioCustomProperty* in_custom_property) final;
	
	/*!
	 * @function AddCustomProperty
	 *
	 * @abstract
	 * Will always return kIOReturnError since a custom property cannot have a custom property
	 *
	 * @return
	 * Returns kIOReturnError
	 */
	virtual kern_return_t RemoveCustomProperty(IOUserAudioCustomProperty* in_custom_property) final;
};

#undef KERNEL
#else /* __DOCUMENTATION__ */

/* generated class IOUserAudioCustomProperty IOUserAudioCustomProperty.iig:40-249 */


#define IOUserAudioCustomProperty_Methods \
\
public:\
\
    kern_return_t\
    _VerifyData(\
        IOUserAudioCustomPropertyDataType in_data_type,\
        OSObject * in_data_to_check);\
\
    static OSSharedPtr<IOUserAudioCustomProperty>\
    Create(\
        IOUserAudioDriver * in_audio_driver,\
        IOUserAudioObjectPropertyAddress in_prop_addr,\
        bool in_is_property_settable,\
        IOUserAudioCustomPropertyDataType in_qualifier_data_type,\
        IOUserAudioCustomPropertyDataType in_data_type);\
\
    kern_return_t\
    SetQualifierAndDataValue(\
        OSObject * in_qualifier_data,\
        OSObject * in_data);\
\
    IOUserAudioCustomPropertyInfo\
    GetCustomPropertyInfo(\
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


#define IOUserAudioCustomProperty_KernelMethods \
\
protected:\
    /* _Impl methods */\
\


#define IOUserAudioCustomProperty_VirtualMethods \
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
        IOUserAudioDriver * in_audio_driver,\
        IOUserAudioObjectPropertyAddress in_prop_addr,\
        bool in_is_property_settable,\
        IOUserAudioCustomPropertyDataType in_qualifier_data_type,\
        IOUserAudioCustomPropertyDataType in_data_type) APPLE_KEXT_OVERRIDE;\
\
    virtual void\
    free(\
) APPLE_KEXT_OVERRIDE;\
\
    virtual IOUserAudioClassID\
    GetClassID(\
) APPLE_KEXT_OVERRIDE;\
\
    virtual kern_return_t\
    HandleChangeCustomPropertyDataValueWithQualifier(\
        OSObject * in_qualifier_data,\
        OSObject * in_data) APPLE_KEXT_OVERRIDE;\
\
    virtual kern_return_t\
    GetCustomPropertyValueWithQualifier(\
        OSObject * in_qualifier_data,\
        __attribute__((os_returns_retained)) OSObject ** out_data) APPLE_KEXT_OVERRIDE;\
\
    virtual kern_return_t\
    AddCustomProperty(\
        IOUserAudioCustomProperty * in_custom_property) APPLE_KEXT_OVERRIDE;\
\
    virtual kern_return_t\
    RemoveCustomProperty(\
        IOUserAudioCustomProperty * in_custom_property) APPLE_KEXT_OVERRIDE;\
\


#if !KERNEL

extern OSMetaClass          * gIOUserAudioCustomPropertyMetaClass;
extern const OSClassLoadInformation IOUserAudioCustomProperty_Class;

class IOUserAudioCustomPropertyMetaClass : public OSMetaClass
{
public:
    virtual kern_return_t
    New(OSObject * instance) override;
};

#endif /* !KERNEL */

#if !KERNEL

class  IOUserAudioCustomPropertyInterface : public OSInterface
{
public:
    virtual bool
    init(IOUserAudioDriver * in_audio_driver,
        IOUserAudioObjectPropertyAddress in_prop_addr,
        bool in_is_property_settable,
        IOUserAudioCustomPropertyDataType in_qualifier_data_type,
        IOUserAudioCustomPropertyDataType in_data_type) = 0;

    virtual kern_return_t
    HandleChangeCustomPropertyDataValueWithQualifier(OSObject * in_qualifier_data,
        OSObject * in_data) = 0;

    virtual kern_return_t
    GetCustomPropertyValueWithQualifier(OSObject * in_qualifier_data,
        OSObject ** out_data) = 0;

    bool
    init_Call(IOUserAudioDriver * in_audio_driver,
        IOUserAudioObjectPropertyAddress in_prop_addr,
        bool in_is_property_settable,
        IOUserAudioCustomPropertyDataType in_qualifier_data_type,
        IOUserAudioCustomPropertyDataType in_data_type)  { return init(in_audio_driver, in_prop_addr, in_is_property_settable, in_qualifier_data_type, in_data_type); };\

    kern_return_t
    HandleChangeCustomPropertyDataValueWithQualifier_Call(OSObject * in_qualifier_data,
        OSObject * in_data)  { return HandleChangeCustomPropertyDataValueWithQualifier(in_qualifier_data, in_data); };\

    kern_return_t
    GetCustomPropertyValueWithQualifier_Call(OSObject * in_qualifier_data,
        OSObject ** out_data)  { return GetCustomPropertyValueWithQualifier(in_qualifier_data, out_data); };\

};

struct IOUserAudioCustomProperty_IVars;
struct IOUserAudioCustomProperty_LocalIVars;

class IOUserAudioCustomProperty : public IOUserAudioObject, public IOUserAudioCustomPropertyInterface
{
#if !KERNEL
    friend class IOUserAudioCustomPropertyMetaClass;
#endif /* !KERNEL */

#if !KERNEL
public:
#ifdef IOUserAudioCustomProperty_DECLARE_IVARS
IOUserAudioCustomProperty_DECLARE_IVARS
#else /* IOUserAudioCustomProperty_DECLARE_IVARS */
    union
    {
        IOUserAudioCustomProperty_IVars * ivars;
        IOUserAudioCustomProperty_LocalIVars * lvars;
    };
#endif /* IOUserAudioCustomProperty_DECLARE_IVARS */
#endif /* !KERNEL */

#if !KERNEL
    static OSMetaClass *
    sGetMetaClass() { return gIOUserAudioCustomPropertyMetaClass; };
#endif /* KERNEL */

    using super = IOUserAudioObject;

#if !KERNEL
    IOUserAudioCustomProperty_Methods
    IOUserAudioCustomProperty_VirtualMethods
#endif /* !KERNEL */

};
#endif /* !KERNEL */


#endif /* !__DOCUMENTATION__ */

/* IOUserAudioCustomProperty.iig:251-252 */

#pragma mark Private Class Extension
/* IOUserAudioCustomProperty.iig:277- */

#endif /* IOUserAudioCustomProperty_h */
