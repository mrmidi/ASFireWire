/* iig(DriverKit-456.120.3) generated from IOUserAudioDriver.iig */

/* IOUserAudioDriver.iig:1-41 */
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

#ifndef IOUserAudioDriver_h
#define IOUserAudioDriver_h

#include <DriverKit/IOService.h>  /* .iig include */
#include <DriverKit/OSSharedPtr.h>
#include <AudioDriverKit/AudioDriverKitTypes.h>

using namespace AudioDriverKit;

class IOUserAudioObject;
class IOUserAudioDevice;
class IOUserAudioCustomProperty;

/* source class IOUserAudioDriver IOUserAudioDriver.iig:42-342 */

#if __DOCUMENTATION__
#define KERNEL IIG_KERNEL

/*!
 * @class IOUserAudioDriver
 *
 * @discussion
 * An IOUserAudioDriver is a subclass of IOService.
 *
 * For the CoreAudio host to match against this IOService, keys must be added to the driver's plist IOKitOPersonalities.
 *
 *	<key>IOUserAudioDriverUserClientProperties</key>
 *	<dict>
 *		<key>IOClass</key>
 *		<string>IOUserUserClient</string>
 *		<key>IOUserClass</key>
 *		<string>IOUserAudioDriverUserClient</string>
 *	</dict>
 *
 * See constants in AudioDriverKitTypes.h
*
 * AudioDriverKit framework will create the IOAudioDriverUserClient when NewUserClient is called in the IOService.
 * The driver extension must have the following audio family entitlement: "com.apple.developer.driverkit.family.audio"
 *
 * When the state of an IOUserAudioObject implemented by the driver changes, it notifies the host to update its state.
 * For changes to an IOUserAudioDevice's or IOUserAudioClockDevice's state that will affect IO or its structure,
 * the client should  trigger a request to the host using RequestDeviceConfigurationChange(), so the host it has an oppurtunity to
 * stop any outstanding  IO and otherwise return the device to its ground state. The host will inform the driver
 * that it is safe to make the change by calling PerformDeviceConfigurationChange() on the object. It is only at this point that
 * the device can make the state change. When PerformDeviceConfigurationChange() returns, the host
 * will figure out what changed and restart any outstanding IO.
 *
 * The host is in control of IO. It tells the drivers's IOUserAudioDevice when to start and when to stop
 * the hardware. The host drives its timing using the timestamps provided by the IOUserAudioClockDevice's
 * implementation of UpdateCurrentZeroTimestamp() and GetCurrentZeroTimestamp(). The series of timestamps provides a
 * mapping between the device's sample time and mach_absolute_time().
 *
 */
class IOUserAudioDriver : public IOService
{
#pragma mark IOService Overrides
public:
	virtual bool					init() override;
	virtual void					free() override;

	virtual kern_return_t 			Start(IOService * provider) override;
	virtual kern_return_t 			Stop(IOService * provider) override;

	virtual kern_return_t			NewUserClient(uint32_t in_type, IOUserClient** out_user_client) override;

#pragma mark IOUserAudioDriver Configuration
	/*!
	 *@function GetClassID
	 *
	 * @abstract
	 * Get the IOUserAudioClassID of the object
	 *
	 * @return
	 * Returns IOUserAudioClassID
	 */
	IOUserAudioClassID GetClassID() LOCALONLY;

	/*!
	 *@function GetBaseClassID
	 *
	 * @abstract
	 * Get the IOUserAudioClassID of the base class object
	 *
	 * @return
	 * Returns IOUserAudioClassID
	 */
	IOUserAudioClassID GetBaseClassID() LOCALONLY;

	/*!
	 * @function GetWorkQueue
	 *
	 * @abstract
	 * Gets the work queue created by the IOUserAudioObject in an OSSharedPtr.
	 *
	 * @discussion
	 * The work queue is used to synchronize access to the driver's state.  Setters and Getters
	 * for the driver will be done on the work queue.
	 *
	 * @return
	 * Returns an OSSharedPtr to an IODispatchQueue on success
	 */
	OSSharedPtr<IODispatchQueue> GetWorkQueue() LOCALONLY;

	/*!
	 * @function SetTransportType
	 *
	 * @abstract
	 * Set the transport type of the IOUserAudioDriver
	 *
	 * @discussion
	 * Transport type can be changed dynamically.  A notification will be sent
	 * to the host to update the object state if successful.
	 *
	 * @param in_transport_type
	 * IOUserAudioTransportType to set.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	kern_return_t SetTransportType(IOUserAudioTransportType in_transport_type) LOCALONLY;

	/*!
	 * @function GetTransportType
	 *
	 * @abstract
	 * Get the transport type of the IOUserAudioDriver.
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns IOUserAudioTransportType
	 */
	IOUserAudioTransportType GetTransportType() LOCALONLY;

	/*!
	 * @function SetName
	 *
	 * @abstract
	 * Set the name of the IOUserAudioDriver
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
	kern_return_t SetName(OSString* in_name) LOCALONLY;
	
	/*!
	 * @function GetName
	 *
	 * @abstract
	 * Get the name of the IOUserAudioDriver.
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns an OSSharedPtr to an OSString
	 */
	OSSharedPtr<OSString> GetName() LOCALONLY;

#pragma mark Overridable IO Methods
	/*!
	 * @function StartDevice
	 *
	 * @abstract
	 * Tells the driver to start IO on a IOUserAudioDevice.
	 *
	 * @discussion
	 * Default implementation will always return kIOReturnSuccess.
	 * Subclass and override this method to handle any hardware specific things when IO
	 * is starting on the device, then call super class to update IO state.
	 * This call is expected to always succeed or fail. The hardware can take as long
	 * as necessary in this call such that it always either succeeds (and kIOReturnSuccess) or fails.
	 * StartIO will be called on the audio device.
	 *
	 * @param in_object_id
	 * IOUserAudioObjectID of the device to start IO.
	 *
	 * @param in_flags
	 * IOUserAudioStartStopFlags to indicate how IO is starting.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	virtual kern_return_t StartDevice(IOUserAudioObjectID in_object_id,
									  IOUserAudioStartStopFlags in_flags) LOCALONLY;
	
	/*!
	 * @function StopDevice
	 *
	 * @abstract
	 * Tells the driver to stop IO on a IOUserAudioDevice.
	 *
	 * @discussion
	 * Default implementation will always return kIOReturnSuccess.
	 * Subclass and override this method to handle any hardware specific things when IO is stopping, then
	 * call super class to update IO state.
	 * StopIO will be called on the audio device.
	 *
	 * @param in_object_id
	 * IOUserAudioObjectID of the device to stop IO.
	 *
	 * @param in_flags
	 * IOUserAudioStartStopFlags to indicate how IO is stopping.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	virtual kern_return_t StopDevice(IOUserAudioObjectID in_object_id,
									 IOUserAudioStartStopFlags in_flags) LOCALONLY;

#pragma mark IOUserAudioObject Related Methods
	/*!
	 * @function GetAudioObjectForObjectID
	 *
	 * @abstract
	 * Get a IOUserAudioObject OSSharedPtr that corresponds to a IOUserAudioObjectID
	 *
	 * @param in_object_id
	 * IOUserAudioObjectID of an object that was previously added to the driver.
	 *
	 * @return
	 * Returns OSSharedPtr to an IOUserAudioObject if in_object_id was found.
	 */
	OSSharedPtr<IOUserAudioObject> GetAudioObjectForObjectID(IOUserAudioObjectID in_object_id) LOCALONLY;

	/*!
	 * @function AddObject
	 *
	 * @abstract
	 * Add a IOUserAudioObject to the driver
	 *
	 * @discussion
	 * All objects that need to be managed by the host needs to be added to the driver.
	 * The objects's reference count will be incremented if it was successfully added.
	 * Caller should also call PropertiesChanged() as necessary to notify host of any changes.
	 *
	 * @param in_object
	 * IOUserAudioObject to be added to the driver.
	 *
	 * @return
	 * Returns kIOReturnSuccess if object was successfully added.
	 */
	kern_return_t AddObject(IOUserAudioObject* in_object) LOCALONLY;
	
	/*!
	 * @function RemoveObject
	 *
	 * @abstract
	 * Remove a IOUserAudioObject from the driver
	 *
	 * @discussion
	 * The objects's reference count will be decremented if it was successfully removed.
	 * Caller should also call PropertiesChanged() as necessary to notify host of any changes.
	 *
	 * @param in_object
	 * IOUserAudioObject to be removed from the driver.
	 *
	 * @return
	 * Returns kIOReturnSuccess if object was successfully removed.
	 */
	kern_return_t RemoveObject(IOUserAudioObject* in_object) LOCALONLY;
	
#pragma mark Asynchronous Change Callback To Host
	/*!
	 * @function PropertiesChanged
	 *
	 * @abstract
	 * This method informs the Host when the state of an driver's object changes.
	 *
	 * @discussion
	 * Note that for device objects, this method is only used for state changes that don't
	 * affect IO or the structure of the device.
	 *
	 * @param in_properties
	 * An array of IOUserAudioObjectPropertySelectors for the changed properties.
	 *
	 * @param in_num_properties
	 * The number of elements in the in_properties array.
	 *
	 * @return
	 * A kern_return_t indicating success or failure.
	 */
	kern_return_t PropertiesChanged(IOUserAudioObjectID in_object_id,
									IOUserAudioObjectPropertySelector* in_properties,
									uint32_t in_num_properties) LOCALONLY;
		
#pragma mark Custom Properties
	/*!
	 * @function AddCustomProperty
	 *
	 * @abstract
	 * Adds a IOUserAudioCustomProperty object to the IOUserAudioDriver.
	 *
	 * @param in_custom_property
	 * A IOUserAudioCustomProperty object that should be added to the IOUserAudioDriver
	 *
	 * @return
	 * Returns kIOReturnSuccess on success
	 */
	kern_return_t AddCustomProperty(IOUserAudioCustomProperty* in_custom_property) LOCALONLY;

	/*!
	 * @function RemoveCustomProperty
	 *
	 * @abstract
	 * Removes a IOUserAudioCustomProperty object that was previously added to the IOUserAudioDriver.
	 *
	 * @param in_custom_property
	 * A IOUserAudioCustomProperty object that should be removed from the IOUserAudioDriver
	 *
	 * @return
	 * Returns kIOReturnSuccess on success
	 */
	kern_return_t RemoveCustomProperty(IOUserAudioCustomProperty* in_custom_property) LOCALONLY;
};

#undef KERNEL
#else /* __DOCUMENTATION__ */

/* generated class IOUserAudioDriver IOUserAudioDriver.iig:42-342 */


#define IOUserAudioDriver_Start_Args \
        IOService * provider

#define IOUserAudioDriver_Stop_Args \
        IOService * provider

#define IOUserAudioDriver_NewUserClient_Args \
        uint32_t in_type, \
        IOUserClient ** out_user_client

#define IOUserAudioDriver_Methods \
\
public:\
\
    virtual kern_return_t\
    Dispatch(const IORPC rpc) APPLE_KEXT_OVERRIDE;\
\
    static kern_return_t\
    _Dispatch(IOUserAudioDriver * self, const IORPC rpc);\
\
    kern_return_t\
    _StartIOThread(\
        IOUserAudioObjectID in_device_id,\
        IOUserAudioObjectID in_iocontext_id,\
        double in_nominal_sample_rate,\
        uint32_t in_io_buffer_frame_size);\
\
    kern_return_t\
    _UnregisterIOThread(\
        IOUserAudioObjectID in_device_id,\
        IOUserAudioObjectID in_iocontext_id);\
\
    kern_return_t\
    _RegisterIOThread(\
        IOUserAudioObjectID in_device_id,\
        IOUserAudioObjectID in_iocontext_id,\
        double in_nominal_sample_rate,\
        uint32_t in_io_buffer_frame_size);\
\
    OSSharedPtr<IOUserClient>\
    _GetIOUserClient(\
);\
\
    kern_return_t\
    _RequestDeviceConfigurationChange(\
        IOUserAudioObjectID in_device_id,\
        uint64_t in_change_action,\
        OSObject * in_change_info);\
\
    kern_return_t\
    _AbortDeviceConfigurationChange(\
        IOUserAudioObjectID in_device_id,\
        uint64_t in_change_action,\
        OSObject * in_change_info);\
\
    kern_return_t\
    _PerformDeviceConfigurationChange(\
        IOUserAudioObjectID in_device_id,\
        uint64_t in_change_action,\
        OSObject * in_change_info);\
\
    kern_return_t\
    _HandleAbortDeviceConfigurationChange(\
        IOUserAudioObjectID in_device_id,\
        uint64_t in_change_action,\
        uint64_t in_change_info_token);\
\
    kern_return_t\
    _HandlePerformDeviceConfigurationChange(\
        IOUserAudioObjectID in_device_id,\
        uint64_t in_change_action,\
        uint64_t in_change_info_token);\
\
    kern_return_t\
    _HandleSetPropertyData(\
        IOUserAudioObjectID in_object_id,\
        const IOUserAudioObjectPropertyAddress * in_prop_addr,\
        OSData * in_qualifier_data,\
        OSData * in_data);\
\
    kern_return_t\
    _HandleGetPropertyData(\
        IOUserAudioObjectID in_object_id,\
        const IOUserAudioObjectPropertyAddress * in_prop_addr,\
        OSData * in_qualifier_data,\
        OSData ** out_data);\
\
    kern_return_t\
    _HandleGetPropertyDataSize(\
        IOUserAudioObjectID in_object_id,\
        const IOUserAudioObjectPropertyAddress * in_prop_addr,\
        OSData * in_qualifier_data,\
        size_t * out_size);\
\
    kern_return_t\
    _HandleIsPropertySettable(\
        IOUserAudioObjectID in_object_id,\
        const IOUserAudioObjectPropertyAddress * in_prop_addr,\
        bool * out_is_settable);\
\
    kern_return_t\
    _HandleHasProperty(\
        IOUserAudioObjectID in_object_id,\
        const IOUserAudioObjectPropertyAddress * in_prop_addr,\
        bool * out_has_property);\
\
    kern_return_t\
    _SetPropertyData(\
        const IOUserAudioObjectPropertyAddress * in_prop_addr,\
        OSData * in_qualifier_data,\
        OSData * in_data);\
\
    kern_return_t\
    _GetPropertyData(\
        const IOUserAudioObjectPropertyAddress * in_prop_addr,\
        OSData * in_qualifier_data,\
        OSData ** out_data);\
\
    kern_return_t\
    _GetPropertySize(\
        const IOUserAudioObjectPropertyAddress * in_prop_addr,\
        OSData * in_qualifier_data,\
        size_t * out_size);\
\
    kern_return_t\
    _IsPropertySettable(\
        const IOUserAudioObjectPropertyAddress * in_prop_addr,\
        bool * out_is_settable);\
\
    kern_return_t\
    _HasProperty(\
        const IOUserAudioObjectPropertyAddress * in_prop_addr,\
        bool * out_has_property);\
\
    void\
    _UnregisterAndStopDevice(\
        IOUserAudioObject * in_audio_object);\
\
    void\
    _UserClientDisconnected(\
);\
\
    OSSharedPtr<IOMemoryDescriptor>\
    _GetMemoryDescriptorFromID(\
        IOUserAudioObjectID in_buffer_id);\
\
    IOUserAudioObjectID\
    _GetNextObjectID(\
);\
\
    IOUserAudioClassID\
    GetClassID(\
);\
\
    IOUserAudioClassID\
    GetBaseClassID(\
);\
\
    OSSharedPtr<IODispatchQueue>\
    GetWorkQueue(\
);\
\
    kern_return_t\
    SetTransportType(\
        IOUserAudioTransportType in_transport_type);\
\
    IOUserAudioTransportType\
    GetTransportType(\
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
    OSSharedPtr<IOUserAudioObject>\
    GetAudioObjectForObjectID(\
        IOUserAudioObjectID in_object_id);\
\
    kern_return_t\
    AddObject(\
        IOUserAudioObject * in_object);\
\
    kern_return_t\
    RemoveObject(\
        IOUserAudioObject * in_object);\
\
    kern_return_t\
    PropertiesChanged(\
        IOUserAudioObjectID in_object_id,\
        IOUserAudioObjectPropertySelector * in_properties,\
        uint32_t in_num_properties);\
\
    kern_return_t\
    AddCustomProperty(\
        IOUserAudioCustomProperty * in_custom_property);\
\
    kern_return_t\
    RemoveCustomProperty(\
        IOUserAudioCustomProperty * in_custom_property);\
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
    kern_return_t\
    NewUserClient_Impl(IOService_NewUserClient_Args);\
\
\
public:\
    /* _Invoke methods */\
\


#define IOUserAudioDriver_KernelMethods \
\
protected:\
    /* _Impl methods */\
\


#define IOUserAudioDriver_VirtualMethods \
\
public:\
\
    virtual bool\
    init(\
) APPLE_KEXT_OVERRIDE;\
\
    virtual void\
    free(\
) APPLE_KEXT_OVERRIDE;\
\
    virtual kern_return_t\
    StartDevice(\
        IOUserAudioObjectID in_object_id,\
        IOUserAudioStartStopFlags in_flags) APPLE_KEXT_OVERRIDE;\
\
    virtual kern_return_t\
    StopDevice(\
        IOUserAudioObjectID in_object_id,\
        IOUserAudioStartStopFlags in_flags) APPLE_KEXT_OVERRIDE;\
\


#if !KERNEL

extern OSMetaClass          * gIOUserAudioDriverMetaClass;
extern const OSClassLoadInformation IOUserAudioDriver_Class;

class IOUserAudioDriverMetaClass : public OSMetaClass
{
public:
    virtual kern_return_t
    New(OSObject * instance) override;
    virtual kern_return_t
    Dispatch(const IORPC rpc) override;
};

#endif /* !KERNEL */

#if !KERNEL

class  IOUserAudioDriverInterface : public OSInterface
{
public:
    virtual kern_return_t
    StartDevice(IOUserAudioObjectID in_object_id,
        IOUserAudioStartStopFlags in_flags) = 0;

    virtual kern_return_t
    StopDevice(IOUserAudioObjectID in_object_id,
        IOUserAudioStartStopFlags in_flags) = 0;

    kern_return_t
    StartDevice_Call(IOUserAudioObjectID in_object_id,
        IOUserAudioStartStopFlags in_flags)  { return StartDevice(in_object_id, in_flags); };\

    kern_return_t
    StopDevice_Call(IOUserAudioObjectID in_object_id,
        IOUserAudioStartStopFlags in_flags)  { return StopDevice(in_object_id, in_flags); };\

};

struct IOUserAudioDriver_IVars;
struct IOUserAudioDriver_LocalIVars;

class IOUserAudioDriver : public IOService, public IOUserAudioDriverInterface
{
#if !KERNEL
    friend class IOUserAudioDriverMetaClass;
#endif /* !KERNEL */

#if !KERNEL
public:
#ifdef IOUserAudioDriver_DECLARE_IVARS
IOUserAudioDriver_DECLARE_IVARS
#else /* IOUserAudioDriver_DECLARE_IVARS */
    union
    {
        IOUserAudioDriver_IVars * ivars;
        IOUserAudioDriver_LocalIVars * lvars;
    };
#endif /* IOUserAudioDriver_DECLARE_IVARS */
#endif /* !KERNEL */

#if !KERNEL
    static OSMetaClass *
    sGetMetaClass() { return gIOUserAudioDriverMetaClass; };
#endif /* KERNEL */

    using super = IOService;

#if !KERNEL
    IOUserAudioDriver_Methods
    IOUserAudioDriver_VirtualMethods
#endif /* !KERNEL */

};
#endif /* !KERNEL */


#endif /* !__DOCUMENTATION__ */

/* IOUserAudioDriver.iig:344-345 */

#pragma mark Private Class Extension
/* IOUserAudioDriver.iig:434- */

#endif /* IOUserAudioDriver_h */
