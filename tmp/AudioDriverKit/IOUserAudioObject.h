/* iig(DriverKit-456.120.3) generated from IOUserAudioObject.iig */

/* IOUserAudioObject.iig:1-41 */
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

#ifndef IOUserAudioObject_h
#define IOUserAudioObject_h

#include <DriverKit/OSObject.h>  /* .iig include */
#include <DriverKit/IOService.h>  /* .iig include */
#include <DriverKit/OSSharedPtr.h>
#include <AudioDriverKit/AudioDriverKitTypes.h>

using namespace AudioDriverKit;

class IOUserAudioDriver;
class IOUserAudioCustomProperty;

/* source class IOUserAudioObject IOUserAudioObject.iig:42-336 */

#if __DOCUMENTATION__
#define KERNEL IIG_KERNEL

/*!
 * @class IOUserAudioObject
 *
 * @brief
 * Base class for all IOUserAudio* based objects.
 *
 * @discussion
 * IOUserAudioObject should not be subclassed or allocated directly.
 */
class LOCALONLY IOUserAudioObject: public OSObject
{
public:

	/*!
	 * @function init
	 *
	 * @abstract
	 * Initializes a IOUserAudioObject.
	 *
	 * @discussion
	 * Always pass in the IOUserAudioDriver.  init() will always return false;
	 *
	 * @param in_audio_driver
	 * The IOUserAudioDriver that owns this object.
	 *
	 * @return
	 * true on success.
	 */
	virtual bool init(IOUserAudioDriver* in_audio_driver);

	virtual bool init() final;
	
	/*!
	 * @function free
	 *
	 * @abstract
	 * frees the IOUserAudioObject.
	 */
	virtual void free() override;

#pragma mark IOUserAudioObject Class Info
	/*!
	 * @function GetClassID
	 *
	 * @abstract
	 * Get the IOUserAudioClassID of the object
	 *
	 * @return
	 * Returns IOUserAudioClassID
	 */
	virtual IOUserAudioClassID GetClassID();
	
	/*!
	 * @function GetBaseClassID
	 *
	 * @abstract
	 * Get the IOUserAudioClassID of the object's base class
	 *
	 * @return
	 * Returns IOUserAudioClassID
	 */
	virtual IOUserAudioClassID GetBaseClassID();
	
	/*!
	 * @function GetObjectID
	 *
	 * @abstract
	 * Get the IOUserAudioObjectID of the object, which can be used for object lookup
	 * with IOUserAudioDriver
	 *
	 * @return
	 * Returns IOUserAudioObjectID
	 */
	IOUserAudioObjectID	GetObjectID();
	
	/*!
	 * @function GetWorkQueue
	 *
	 * @abstract
	 * Gets the work queue created by the IOUserAudioObject in an OSSharedPtr.
	 *
	 * @discussion
	 * The work queue is used to synchronize access to the object's state.  Setters and Getters
	 * for the object will be done on the work queue.
	 *
	 * @return
	 * Returns an OSSharedPtr to an IODispatchQueue on success
	 */
	OSSharedPtr<IODispatchQueue> GetWorkQueue();
	
#pragma mark Object Setters and Getters
	/*!
	 * @function SetName
	 *
	 * @abstract
	 * Set the name of the IOUserAudioObject
	 *
	 * @discussion
	 * If object can change the name dynamically, a notification will be sent
	 * to the host to update the object state if successful.
	 * Setting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_name
	 * OSString name to set.
	 *
	 * @return
	 * Returns kern_return_t.
	 */
	kern_return_t SetName(OSString* in_name);
	
	/*!
	 * @function GetName
	 *
	 * @abstract
	 * Get the name of the IOUserAudioObject.
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns an OSSharedPtr to an OSString
	 */
	OSSharedPtr<OSString> GetName();

	/*!
	 * @function SetElementName
	 *
	 * @abstract
	 * Set the name for the given element and scope of the IOUserAudioObject
	 *
	 * @discussion
	 * If object can change the name dynamically, a notification will be sent
	 * to the host to update the object state if successful.
	 * Setting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_element
	 * The IOUserAudioObjectPropertyElement
	 *
	 * @param in_scope
	 * The IOUserAudioObjectPropertyScope
	 *
	 * @param in_name
	 * OSString name to set.
	 * If the OSString is set to NULL, then the name will be removed.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	kern_return_t SetElementName(IOUserAudioObjectPropertyElement in_element, IOUserAudioObjectPropertyScope in_scope, OSString* in_name);
	
	/*!
	 * @function GetElementName
	 *
	 * @abstract
	 * Get the name for the given element and scope of the IOUserAudioObject.
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_element
	 * The IOUserAudioObjectPropertyElement
	 *
	 * @param in_scope
	 * The IOUserAudioObjectPropertyScope
	 *
	 * @return
	 * Returns an OSSharedPtr to an OSString
	 */
	OSSharedPtr<OSString> GetElementName(IOUserAudioObjectPropertyElement in_element, IOUserAudioObjectPropertyScope in_scope);
	
	/*!
	 * @function SetElementCategoryName
	 *
	 * @abstract
	 * Set the category name for the given element and scope of the IOUserAudioObject
	 *
	 * @discussion
	 * If object can change the name dynamically, a notification will be sent
	 * to the host to update the object state if successful.
	 * Setting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_element
	 * The IOUserAudioObjectPropertyElement
	 *
	 * @param in_scope
	 * The IOUserAudioObjectPropertyScope
	 *
	 * @param in_category_name
	 * OSString category name to set.
	 * If the OSString is NULL, then the element category name will be removed.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	kern_return_t SetElementCategoryName(IOUserAudioObjectPropertyElement in_element, IOUserAudioObjectPropertyScope in_scope, OSString* in_category_name);

	/*!
	 * @function GetElementCategoryName
	 *
	 * @abstract
	 * Get the category name for the given element and scope of the IOUserAudioObject.
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_element
	 * The IOUserAudioObjectPropertyElement
	 *
	 * @param in_scope
	 * The IOUserAudioObjectPropertyScope
	 *
	 * @return
	 * Returns an OSSharedPtr to an OSString
	 */
	OSSharedPtr<OSString> GetElementCategoryName(IOUserAudioObjectPropertyElement in_element, IOUserAudioObjectPropertyScope in_scope);

	/*!
	 * @function SetElementNumberName
	 *
	 * @abstract
	 * Set the number name for the given element of the IOUserAudioObject
	 *
	 * @discussion
	 * If object can change the name dynamically, a notification will be sent
	 * to the host to update the object state if successful.
	 * Setting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_element
	 * The IOUserAudioObjectPropertyElement
	 *
	 * @param in_scope
	 * The IOUserAudioObjectPropertyScope
	 *
	 * @param in_number_name
	 * OSString number name to set.
	 * If the OSString is NULL, then the element number name will be removed.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	kern_return_t SetElementNumberName(IOUserAudioObjectPropertyElement in_element, IOUserAudioObjectPropertyScope in_scope, OSString* in_number_name);

	/*!
	 * @function GetElementNumberName
	 *
	 * @abstract
	 * Get the number name for the given element and scope of the IOUserAudioObject.
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_element
	 * The IOUserAudioObjectPropertyElement
	 *
	 * @param in_scope
	 * The IOUserAudioObjectPropertyScope
	 *
	 * @return
	 * Returns an OSSharedPtr to an OSString
	 */
	OSSharedPtr<OSString> GetElementNumberName(IOUserAudioObjectPropertyElement in_element, IOUserAudioObjectPropertyScope in_scope);


#pragma mark Custom Properties
	/*!
	 * @function AddCustomProperty
	 *
	 * @abstract
	 * Adds a IOUserAudioCustomProperty object to this IOUserAudioObject.
	 *
	 * @param in_custom_property
	 * A IOUserAudioCustomProperty object that should be added to the IOUserAudioObject
	 *
	 * @return
	 * Returns kIOReturnSuccess on success
	 */
	virtual kern_return_t AddCustomProperty(IOUserAudioCustomProperty* in_custom_property);

	/*!
	 * @function RemoveCustomProperty
	 *
	 * @abstract
	 * Removes a IOUserAudioCustomProperty object that was previously added to the IOUserAudioObject.
	 *
	 * @param in_custom_property
	 * A IOUserAudioCustomProperty object that should be removed from the IOUserAudioObject
	 *
	 * @return
	 * Returns kIOReturnSuccess on success
	 */
	virtual kern_return_t RemoveCustomProperty(IOUserAudioCustomProperty* in_custom_property);

#pragma mark Owning Object
    /*!
     * @function GetOwnerObjectID
     *
     * @abstract
     * Get the IOUserAudioObjectID of the object that owns the object.
     *
     * @return
     * Returns IOUserAudioObjectID of the owning object
     */
    IOUserAudioObjectID GetOwnerObjectID();
};

#undef KERNEL
#else /* __DOCUMENTATION__ */

/* generated class IOUserAudioObject IOUserAudioObject.iig:42-336 */


#define IOUserAudioObject_Methods \
\
public:\
\
    void\
    _SetOwningObjectID(\
        IOUserAudioObjectID _param0);\
\
    void\
    _DriverServiceStopped(\
);\
\
    static OSObject *\
    _CreateObjectFromSerializedData(\
        OSData * in_data);\
\
    static OSData *\
    _SerializeObject(\
        OSObject * in_object);\
\
    static size_t\
    _GetSerializedObjectLength(\
        OSObject * in_object);\
\
    kern_return_t\
    _AllocateBufferDescriptor(\
        uint64_t in_options,\
        uint64_t in_capacity,\
        uint64_t in_alignment,\
        IOBufferMemoryDescriptor ** out_descriptor,\
        void ** out_buffer);\
\
    IOUserAudioObjectID\
    GetObjectID(\
);\
\
    OSSharedPtr<IODispatchQueue>\
    GetWorkQueue(\
);\
\
    kern_return_t\
    SetName(\
        OSString * in_name);\
\
    OSSharedPtr<OSString>\
    GetName(\
);\
\
    kern_return_t\
    SetElementName(\
        IOUserAudioObjectPropertyElement in_element,\
        IOUserAudioObjectPropertyScope in_scope,\
        OSString * in_name);\
\
    OSSharedPtr<OSString>\
    GetElementName(\
        IOUserAudioObjectPropertyElement in_element,\
        IOUserAudioObjectPropertyScope in_scope);\
\
    kern_return_t\
    SetElementCategoryName(\
        IOUserAudioObjectPropertyElement in_element,\
        IOUserAudioObjectPropertyScope in_scope,\
        OSString * in_category_name);\
\
    OSSharedPtr<OSString>\
    GetElementCategoryName(\
        IOUserAudioObjectPropertyElement in_element,\
        IOUserAudioObjectPropertyScope in_scope);\
\
    kern_return_t\
    SetElementNumberName(\
        IOUserAudioObjectPropertyElement in_element,\
        IOUserAudioObjectPropertyScope in_scope,\
        OSString * in_number_name);\
\
    OSSharedPtr<OSString>\
    GetElementNumberName(\
        IOUserAudioObjectPropertyElement in_element,\
        IOUserAudioObjectPropertyScope in_scope);\
\
    IOUserAudioObjectID\
    GetOwnerObjectID(\
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


#define IOUserAudioObject_KernelMethods \
\
protected:\
    /* _Impl methods */\
\


#define IOUserAudioObject_VirtualMethods \
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
        IOUserAudioDriver * in_audio_driver) APPLE_KEXT_OVERRIDE;\
\
    virtual bool\
    init(\
) APPLE_KEXT_OVERRIDE;\
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
    AddCustomProperty(\
        IOUserAudioCustomProperty * in_custom_property) APPLE_KEXT_OVERRIDE;\
\
    virtual kern_return_t\
    RemoveCustomProperty(\
        IOUserAudioCustomProperty * in_custom_property) APPLE_KEXT_OVERRIDE;\
\


#if !KERNEL

extern OSMetaClass          * gIOUserAudioObjectMetaClass;
extern const OSClassLoadInformation IOUserAudioObject_Class;

class IOUserAudioObjectMetaClass : public OSMetaClass
{
public:
    virtual kern_return_t
    New(OSObject * instance) override;
};

#endif /* !KERNEL */

#if !KERNEL

class  IOUserAudioObjectInterface : public OSInterface
{
public:
    virtual kern_return_t
    _SetPropertyData(const IOUserAudioObjectPropertyAddress * in_prop_addr,
        OSData * in_qualifier_data,
        OSData * in_data) = 0;

    virtual kern_return_t
    _GetPropertyData(const IOUserAudioObjectPropertyAddress * in_prop_addr,
        OSData * in_qualifier_data,
        OSData ** out_data) = 0;

    virtual kern_return_t
    _GetPropertySize(const IOUserAudioObjectPropertyAddress * in_prop_addr,
        OSData * in_qualifier_data,
        size_t * out_size) = 0;

    virtual kern_return_t
    _IsPropertySettable(const IOUserAudioObjectPropertyAddress * in_prop_addr,
        bool * out_is_settable) = 0;

    virtual kern_return_t
    _HasProperty(const IOUserAudioObjectPropertyAddress * in_prop_addr,
        bool * out_has_property) = 0;

    virtual bool
    init(IOUserAudioDriver * in_audio_driver) = 0;

    virtual IOUserAudioClassID
    GetClassID() = 0;

    virtual IOUserAudioClassID
    GetBaseClassID() = 0;

    virtual kern_return_t
    AddCustomProperty(IOUserAudioCustomProperty * in_custom_property) = 0;

    virtual kern_return_t
    RemoveCustomProperty(IOUserAudioCustomProperty * in_custom_property) = 0;

    kern_return_t
    _SetPropertyData_Call(const IOUserAudioObjectPropertyAddress * in_prop_addr,
        OSData * in_qualifier_data,
        OSData * in_data)  { return _SetPropertyData(in_prop_addr, in_qualifier_data, in_data); };\

    kern_return_t
    _GetPropertyData_Call(const IOUserAudioObjectPropertyAddress * in_prop_addr,
        OSData * in_qualifier_data,
        OSData ** out_data)  { return _GetPropertyData(in_prop_addr, in_qualifier_data, out_data); };\

    kern_return_t
    _GetPropertySize_Call(const IOUserAudioObjectPropertyAddress * in_prop_addr,
        OSData * in_qualifier_data,
        size_t * out_size)  { return _GetPropertySize(in_prop_addr, in_qualifier_data, out_size); };\

    kern_return_t
    _IsPropertySettable_Call(const IOUserAudioObjectPropertyAddress * in_prop_addr,
        bool * out_is_settable)  { return _IsPropertySettable(in_prop_addr, out_is_settable); };\

    kern_return_t
    _HasProperty_Call(const IOUserAudioObjectPropertyAddress * in_prop_addr,
        bool * out_has_property)  { return _HasProperty(in_prop_addr, out_has_property); };\

    bool
    init_Call(IOUserAudioDriver * in_audio_driver)  { return init(in_audio_driver); };\

    IOUserAudioClassID
    GetClassID_Call()  { return GetClassID(); };\

    IOUserAudioClassID
    GetBaseClassID_Call()  { return GetBaseClassID(); };\

    kern_return_t
    AddCustomProperty_Call(IOUserAudioCustomProperty * in_custom_property)  { return AddCustomProperty(in_custom_property); };\

    kern_return_t
    RemoveCustomProperty_Call(IOUserAudioCustomProperty * in_custom_property)  { return RemoveCustomProperty(in_custom_property); };\

};

struct IOUserAudioObject_IVars;
struct IOUserAudioObject_LocalIVars;

class IOUserAudioObject : public OSObject, public IOUserAudioObjectInterface
{
#if !KERNEL
    friend class IOUserAudioObjectMetaClass;
#endif /* !KERNEL */

#if !KERNEL
public:
#ifdef IOUserAudioObject_DECLARE_IVARS
IOUserAudioObject_DECLARE_IVARS
#else /* IOUserAudioObject_DECLARE_IVARS */
    union
    {
        IOUserAudioObject_IVars * ivars;
        IOUserAudioObject_LocalIVars * lvars;
    };
#endif /* IOUserAudioObject_DECLARE_IVARS */
#endif /* !KERNEL */

#if !KERNEL
    static OSMetaClass *
    sGetMetaClass() { return gIOUserAudioObjectMetaClass; };
#endif /* KERNEL */

    using super = OSObject;

#if !KERNEL
    IOUserAudioObject_Methods
    IOUserAudioObject_VirtualMethods
#endif /* !KERNEL */

};
#endif /* !KERNEL */


#endif /* !__DOCUMENTATION__ */

/* IOUserAudioObject.iig:338-339 */

#pragma mark Private Class Extension
/* IOUserAudioObject.iig:378- */

#endif /* IOUserAudioObject_h */
