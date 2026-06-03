/* iig(DriverKit-456.120.3) generated from IOUserAudioDevice.iig */

/* IOUserAudioDevice.iig:1-43 */
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

#ifndef IOUserAudioDevice_h
#define IOUserAudioDevice_h

#include <DriverKit/IOService.h>  /* .iig include */
#include <DriverKit/OSSharedPtr.h>
#include <AudioDriverKit/IOUserAudioObject.h>  /* .iig include */
#include <AudioDriverKit/IOUserAudioClockDevice.h>  /* .iig include */

using namespace AudioDriverKit;

class IOUserAudioDriver;
class IODispatchQueue;
class IOUserAudioStream;
class IOUserAudioControl;

/* source class IOUserAudioDevice IOUserAudioDevice.iig:44-608 */

#if __DOCUMENTATION__
#define KERNEL IIG_KERNEL

/*!
 * @class IOUserAudioDevice
 *
 * @discussion
 * The IOUserAudioDevice class is a subclass of the IOUserAudioClockDevice class. The device
 * has IOUserAudioDeviceStreams.
 */
class LOCALONLY IOUserAudioDevice: public IOUserAudioClockDevice
{
public:
    /*!
     * @function Create
     *
     * @abstract
     * static factory method to allocate and initialize an IOUserAudioDevice.
     *
     * @discussion
     * If IOUserAudioDevice is subclassed to override behavior, Create should not be
     * used to allocate/initialize the custom subclass.
     *
     * @param in_driver
     * The IOUserAudioDriver that owns this object.
     *
     * @param in_supports_prewarming
     * A bool that specifies if the device supports prewarming IO.
     *
     * @param in_device_uid
     * OSString pointer for the audio device unique identifier.
     *
     * @param in_model_uid
     * OSString pointer for the audio device model unique identifier.
     *
     * @param in_manufacturer_uid
     * OSString pointer for the audio device manufacturer unique identifier.
     *
     * @param in_zero_timestamp_period
	 * A uint32_t whose value indicates the number of sample frames the host can
	 * expect between successive time stamps returned from GetZeroTimeStamp(). In
	 * other words, if GetZeroTimeStamp() returned a sample time of X, the host can
	 * expect that the next valid time stamp that will be returned will be X plus
	 * the value of this property.
     *
     * @return
     * OSSharedPtr to an IOUserAudioDevice if it was successfully allocated and initialized
     */
    static OSSharedPtr<IOUserAudioDevice> Create(IOUserAudioDriver* in_driver,
                                                 bool in_supports_prewarming,
                                                 OSString* in_device_uid,
                                                 OSString* in_model_uid,
                                                 OSString* in_manufacturer_uid,
                                                 uint32_t in_zero_timestamp_period);

	/*!
	 * @function init
	 *
	 * @abstract
	 * Initializes a IOUserAudioDevice.
	 *
	 * @param in_driver
	 * The IOUserAudioDriver that owns this object.
	 *
	 * @param in_supports_prewarming
	 * A bool that specifies if the device supports prewarming IO.
	 *
	 * @param in_device_uid
	 * OSString pointer for the audio device unique identifier.
	 *
	 * @param in_model_uid
	 * OSString pointer for the audio device model unique identifier.
	 *
	 * @param in_manufacturer_uid
	 * OSString pointer for the audio device manufacturer unique identifier.
	 *
	 * @param in_zero_timestamp_period
	 * A uint32_t whose value indicates the number of sample frames the host can
	 * expect between successive time stamps returned from GetZeroTimeStamp(). In
	 * other words, if GetZeroTimeStamp() returned a sample time of X, the host can
	 * expect that the next valid time stamp that will be returned will be X plus
	 * the value of this property.
	 *
	 * @return
	 * true on success.
	 */
	virtual bool init(IOUserAudioDriver* in_driver,
					  bool in_supports_prewarming,
					  OSString* in_device_uid,
                      OSString* in_model_uid,
                      OSString* in_manufacturer_uid,
					  uint32_t in_zero_timestamp_period) override;
	
	/*!
	 * @function free
	 *
	 * @abstract
	 * frees the IOUserAudioDevice.
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
	virtual IOUserAudioClassID GetClassID() final;

	/*!
	 * @function GetBaseClassID
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
	virtual IOUserAudioClassID GetBaseClassID() final;

#pragma mark IOUserAudioClockDevice overrides: IO Methods
	/*!
	 * @function StartIO
	 *
	 * @abstract
	 * Tells the device to start IO.
	 *
	 * @discussion
	 * Default implementation will always return kIOReturnSuccess.
	 * Subclass and override this method to handle any hardware specific things when IO is starting, then
	 * call super class to update IO state.
	 * This call is expected to always succeed or fail. The hardware can take as long
	 * as necessary in this call such that it always either succeeds (and kIOReturnSuccess) or fails.
	 * StartIO will also be called for all streams that were added to the device.
	 *
	 * @param in_flags
	 * IOUserAudioStartStopFlags to indicate how IO is starting.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	virtual kern_return_t StartIO(IOUserAudioStartStopFlags in_flags) override;
	
	/*!
	 * @function StopIO
	 *
	 * @abstract
	 * Tells the device to stop IO.
	 *
	 * @discussion
	 * Default implementation will always return kIOReturnSuccess.
	 * Subclass and override this method to handle any hardware specific things when IO is stopping, then
	 * call super class to update IO state.
	 * StopIO will also be called for all streams that were added to the device.
	 *
	 * @param in_flags
	 * IOUserAudioStartStopFlags to indicate how IO is stopping.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	virtual kern_return_t StopIO(IOUserAudioStartStopFlags in_flags) override;
	
	/*!
	 * @function PerformDeviceConfigurationChange
	 *
	 * @abstract
	 * This is called by the host to allow the device to perform a configuration
	 * change that had been previously requested via a call to the host via
	 * RequestDeviceConfigChange or a change to an IO state that
	 * requires a configuration change
	 *
	 * @discussion
	 * Subclass and override this method to handle any custom configuration change requests, then
	 * call super class to update state.
	 * IO will be stopped prior to the performing the configuration change.
	 *
	 * @param in_change_action
	 * A uint64_t indicating the action the device object wants to take. This is
	 * the same value that was passed to RequestDeviceConfigurationChange().
	 * Note that this value is purely for the driver's usage. The host does
	 * not look at this value.
	 *
	 * @param in_change_info
	 * A pointer to an OSObject  about the configuration change. This is the
	 * same value that was passed to RequestDeviceConfigurationChange(). Note
	 * that this value is purely for the driver's usage. The Host does not
	 * look at this value.  Object reference should be retained/released as necessary.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	virtual kern_return_t PerformDeviceConfigurationChange(uint64_t in_change_action,
														   OSObject* in_change_info) override;

	/*!
	 * @function AbortDeviceConfigurationChange
	 *
	 * @abstract
	 * This is called by the Host to tell the driver not to perform a
	 * configuration change that had been requested via a call to the Host method,
	 * RequestDeviceConfigurationChange(). Subclass and override this method to handle any
	 * aborted custom configuration change requests, then call super class to update state.
	 *
	 * @param in_change_action
	 * A uint64_t indicating the action the device object wants to take. This is
	 * the same value that was passed to RequestDeviceConfigurationChange().
	 * Note that this value is purely for the driver's usage. The host does
	 * not look at this value.
	 *
	 * @param in_change_info
	 * A pointer to an OSObject  about the configuration change. This is the
	 * same value that was passed to RequestDeviceConfigurationChange(). Note
	 * that this value is purely for the driver's usage. The Host does not
	 * look at this value.  Object reference should be retained/released as necessary.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	virtual kern_return_t AbortDeviceConfigurationChange(uint64_t in_change_action,
														 OSObject* in_change_info) override;

#pragma mark Overridable Audio Device Setters
	/*!
	 * @function HandleChangeSampleRate
	 *
	 * @abstract
	 * Virtual method will be called when the device's sample rate will be changed.
	 *
	 * @discussion
	 * Default implementation will call SetSampleRate() and return kIOReturnSuccess.
	 * Subclass can override this method to handle changes to this value and
	 * should return kIOReturnSucess upon success.
	 *
	 * @param in_sample_rate
	 * The double sample rate attempting to be set on the device.
	 *
	 * @return
	 * Returns kIOReturnSuccess on sucess. Upon sucess the controls value should be updated.
	 */
	virtual kern_return_t HandleChangeSampleRate(double in_sample_rate) override;

#pragma mark Audio Device Setters/Getters
	/*!
	 * @function SetCanBeDefaultInputDevice
	 *
	 * @abstract
	 * Specify if device can be used as default input device.
	 *
	 * @discussion
	 * Setting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_can_be_default
	 * true if device can be used as default input device by the host.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	kern_return_t SetCanBeDefaultInputDevice(bool in_can_be_default);

	/*!
	 * @function CanBeDefaultInputDevice
	 *
	 * @abstract
	 * Get bool value indiciating if device can be used for default input.
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns bool, true if device can be used for default input.
	 */
	uint32_t CanBeDefaultInputDevice();
	
	/*!
	 * @function SetCanBeDefaultOutputDevice
	 *
	 * @abstract
	 * Specify if device can be used as default output device.
	 *
	 * @discussion
	 * Setting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_can_be_default
	 * true if device can be used as default output device by the host.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	kern_return_t SetCanBeDefaultOutputDevice(bool in_can_be_default);
	
	/*!
	 * @function CanBeDefaultOutputDevice
	 *
	 * @abstract
	 * Get bool value indiciating if device can be used for default output.
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns bool, true if device can be used for default output.
	 */
	uint32_t CanBeDefaultOutputDevice();
	
	/*!
	 * @function SetCanBeDefaultSystemOutputDevice
	 *
	 * @abstract
	 * Specify if device can be used as default system output device
	 * Setting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_can_be_default
	 * true if device can be used as default system output device by the host.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	kern_return_t SetCanBeDefaultSystemOutputDevice(bool in_can_be_default);
	
	/*!
	 * @function CanBeDefaultSystemOutputDevice
	 *
	 * @abstract
	 * Get bool value indiciating if device can be used for default system output.
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns bool, true if device can be used for default system output.
	 */
	uint32_t CanBeDefaultSystemOutputDevice();

	/*!
	 * @function SetInputSafetyOffset
	 *
	 * @abstract
	 * Specify the input safety offset of the device.
	 *
	 * @discussion
	 * A uint32_t whose value indicates the number for frames behind
	 * the current hardware position that is safe to do IO.
	 *
	 * @param in_safety_offset
	 * uint32_t input safety offset value.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	kern_return_t SetInputSafetyOffset(uint32_t in_safety_offset);
	
	/*!
	 * @function GetInputSafetyOffset
	 *
	 * @abstract
	 * Get the input safety offset of the device.
	 *
	 * @discussion
	 * A uint32_t whose value indicates the number for frames behind
	 * the current hardware position that is safe to do IO.
	 *
	 * @return
	 * Returns uint32_t input safety offset.
	 */
	uint32_t GetInputSafetyOffset();
	
	/*!
	 * @function SetOutputSafetyOffset
	 *
	 * @abstract
	 * Specify the output safety offset of the device.
	 *
	 * @discussion
	 * A uint32_t whose value indicates the number for frames ahead
	 * the current hardware position that is safe to do IO.
	 *
	 * @param in_safety_offset
	 * uint32_t output safety offset value.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	kern_return_t SetOutputSafetyOffset(uint32_t in_safety_offset);
	
	/*!
	 * @function GetOutputSafetyOffset
	 *
	 * @abstract
	 * Get the output safety offset of the device.
	 *
	 * @discussion
	 * A uint32_t whose value indicates the number for frames ahead
	 * the current hardware position that is safe to do IO.
	 *
	 * @return
	 * Returns uint32_t output safety offset.
	 */
	uint32_t GetOutputSafetyOffset();
    
    /*!
     * @function SetPreferredChannelsForStereo
     *
     * @abstract
     * Set the channel indices for the prefered stereo pair
     *
     * @param in_left_channel
     * uint32_t channel index for the left channel.
     *
     * @param in_right_channel
     * uint32_t channel index for the right channel.
     *
     * @return
     * Returns kern_return_t
     */
    kern_return_t SetPreferredChannelsForStereo(uint32_t in_left_channel, uint32_t in_right_channel);
    
    /*!
     * @function GetPreferredChannelsForStereo
     *
     * @abstract
     * Get the channel indices for the prefered stereo pair
     *
     * @param out_left_channel
     * Pointer to a uint32_t channel index for the preferred stereo left channel.
     *
     * @param out_right_channel
     * Pointer to a uint32_t channel index for the preferred stereo right channel.
     */
    void GetPreferredChannelsForStereo(uint32_t* out_left_channel, uint32_t* out_right_channel);
    
    /*!
     * @function SetPreferredOutputChannelLayout
     *
     * @abstract
     * Set the output channel layout with IOUserAudioChannelLabel values
     *
     * @param in_channel_labels
     * array of IOUserAudioChannelLabel's.
     *
     * @param in_num_channels
     * number of items in in_channel_labels array
     *
     * @return
     * Returns kern_return_t
     */
    kern_return_t SetPreferredOutputChannelLayout(IOUserAudioChannelLabel* in_channel_labels, size_t in_num_channels);
    
    /*!
     * @function SetPreferredInputChannelLayout
     *
     * @abstract
     * Set the input channel layout with IOUserAudioChannelLabel values
     *
     * @param in_channel_labels
     * array of IOUserAudioChannelLabel's.
     *
     * @param in_num_channels
     * number of items in in_channel_labels array
     *
     * @return
     * Returns kern_return_t
     */
    kern_return_t SetPreferredInputChannelLayout(IOUserAudioChannelLabel* in_channel_labels, size_t in_num_channels);
	
#pragma mark Audio Stream
	/*!
	 * @function AddStream
	 *
	 * @abstract
	 * Add a IOUserAudioStream to the device.
	 *
	 * @discussion
	 * The stream's reference count will be incremented if it was successfully added.
	 *
	 * @param in_stream
	 * IOUserAudioStream to add to the device.
	 *
	 * @return
	 * Returns kIOReturnSuccess if stream was successfully added.
	 */
	kern_return_t AddStream(IOUserAudioStream* in_stream);
	
	/*!
	 * @function RemoveStream
	 *
	 * @abstract
	 * Remove a IOUserAudioStream from the device.
	 *
	 * @discussion
	 * The stream's reference count will be decremented if it was successfully removed.
	 *
	 * @param in_stream
	 * IOUserAudioStream to remove from the device.
	 *
	 * @return
	 * Returns kIOReturnSuccess if stream was successfully removed.
	 */
	kern_return_t RemoveStream(IOUserAudioStream* in_stream);

#pragma mark IO Operations
	/*!
	 * @function SetIOOperationHandler
	 *
	 * @abstract
	 * Set the IOOperationHandler block on the device.
	 *
	 * @discussion
	 * The IOOperationHandler will be invoked when a IO operation is performed by the host.
	 * The handler will be called on a real time priority thread, so any work should only call
	 * real-time safe operations and never block. Many of the calls to various IOUserAudioObjects
	 * are syncrhonized against the work queue, so any necessary information to perform IO
	 * should be cached and captured in the block.
	 *
	 * @param in_io_operation_block
	 * The IOOperationHandler block to be called when the host performs an IO operation.
	 *
	 * @return
	 * Returns kIOReturnSuccess if the IOOperationHandler block was successfuly set on the device
	 */
	kern_return_t SetIOOperationHandler(IOOperationHandler in_io_operation_block);

	
	/*!
	 * @function GetCurrentClientIOTime
	 *
	 * @abstract
	 * Get the current sample/host time pair in the ring buffer written to or read from by the client
	 *
	 * @param in_is_input
	 * bool value indicating if client IO time is for input or output.  true for input, false for output
	 *
	 * @param out_input_sample_time
	 * pointer to uint64_t that will be set with the current io sample time of the client.
	 *
	 * @param out_output_sample_time
	 * pointer to uint64_t that will be set with the current io host time of the client.
	 */
	void GetCurrentClientIOTime(bool in_is_input,
								uint64_t* out_sample_time,
								uint64_t* out_host_time);
	
#pragma mark Stream Restoration
	/*!
	 * @function SetWantsStreamFormatsRestored
	 *
	 * @abstract
	 * Setter on the device object that tells the host that the stream formats for the device should or should not be
	 * saved/restored when the device is first published. If the device doesn't implement
	 * this property, it is assumed that the settings should be saved and restored.
	 * Note that this should be set before the device is published to the host.
	 *
	 * @param in_wants_stream_formats_restored
	 * bool value indicating if the host should or should not restore the stream formats for the device
	 * A value of false indicates that the stream formats for the device should NOT be saved/restored
	 * A value of true indicated that the stream formats for the device should be saved/restored
	 */
	void SetWantsStreamFormatsRestored(bool in_wants_stream_formats_restored);
};

#undef KERNEL
#else /* __DOCUMENTATION__ */

/* generated class IOUserAudioDevice IOUserAudioDevice.iig:44-608 */


#define IOUserAudioDevice_Methods \
\
public:\
\
    void\
    _UnregisterAllIOThreads(\
);\
\
    kern_return_t\
    _StartIOThread(\
        IOUserAudioObjectID in_iocontext_id,\
        double in_nominal_sample_rate,\
        uint32_t in_io_buffer_frame_size);\
\
    kern_return_t\
    _UnregisterIOThread(\
        IOUserAudioObjectID in_iocontext_id);\
\
    kern_return_t\
    _RegisterIOThread(\
        IOUserAudioObjectID in_iocontext_id,\
        double in_nominal_sample_rate,\
        uint32_t in_io_buffer_frame_size);\
\
    IOUserAudioObjectID\
    _GetIOMemoryObjectID(\
        IOUserAudioObjectID in_stream_object_id);\
\
    OSSharedPtr<IOMemoryDescriptor>\
    _GetIOMemoryDescriptorFromObjectID(\
        IOUserAudioObjectID in_object_id);\
\
    static OSSharedPtr<IOUserAudioDevice>\
    Create(\
        IOUserAudioDriver * in_driver,\
        bool in_supports_prewarming,\
        OSString * in_device_uid,\
        OSString * in_model_uid,\
        OSString * in_manufacturer_uid,\
        uint32_t in_zero_timestamp_period);\
\
    kern_return_t\
    SetCanBeDefaultInputDevice(\
        bool in_can_be_default);\
\
    uint32_t\
    CanBeDefaultInputDevice(\
);\
\
    kern_return_t\
    SetCanBeDefaultOutputDevice(\
        bool in_can_be_default);\
\
    uint32_t\
    CanBeDefaultOutputDevice(\
);\
\
    kern_return_t\
    SetCanBeDefaultSystemOutputDevice(\
        bool in_can_be_default);\
\
    uint32_t\
    CanBeDefaultSystemOutputDevice(\
);\
\
    kern_return_t\
    SetInputSafetyOffset(\
        uint32_t in_safety_offset);\
\
    uint32_t\
    GetInputSafetyOffset(\
);\
\
    kern_return_t\
    SetOutputSafetyOffset(\
        uint32_t in_safety_offset);\
\
    uint32_t\
    GetOutputSafetyOffset(\
);\
\
    kern_return_t\
    SetPreferredChannelsForStereo(\
        uint32_t in_left_channel,\
        uint32_t in_right_channel);\
\
    void\
    GetPreferredChannelsForStereo(\
        uint32_t * out_left_channel,\
        uint32_t * out_right_channel);\
\
    kern_return_t\
    SetPreferredOutputChannelLayout(\
        IOUserAudioChannelLabel * in_channel_labels,\
        size_t in_num_channels);\
\
    kern_return_t\
    SetPreferredInputChannelLayout(\
        IOUserAudioChannelLabel * in_channel_labels,\
        size_t in_num_channels);\
\
    kern_return_t\
    AddStream(\
        IOUserAudioStream * in_stream);\
\
    kern_return_t\
    RemoveStream(\
        IOUserAudioStream * in_stream);\
\
    kern_return_t\
    SetIOOperationHandler(\
        IOOperationHandler in_io_operation_block);\
\
    void\
    GetCurrentClientIOTime(\
        bool in_is_input,\
        uint64_t * out_sample_time,\
        uint64_t * out_host_time);\
\
    void\
    SetWantsStreamFormatsRestored(\
        bool in_wants_stream_formats_restored);\
\
\
protected:\
    /* _Impl methods */\
\
\
public:\
    /* _Invoke methods */\
\


#define IOUserAudioDevice_KernelMethods \
\
protected:\
    /* _Impl methods */\
\


#define IOUserAudioDevice_VirtualMethods \
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
        bool in_supports_prewarming,\
        OSString * in_device_uid,\
        OSString * in_model_uid,\
        OSString * in_manufacturer_uid,\
        uint32_t in_zero_timestamp_period) APPLE_KEXT_OVERRIDE;\
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
    StartIO(\
        IOUserAudioStartStopFlags in_flags) APPLE_KEXT_OVERRIDE;\
\
    virtual kern_return_t\
    StopIO(\
        IOUserAudioStartStopFlags in_flags) APPLE_KEXT_OVERRIDE;\
\
    virtual kern_return_t\
    PerformDeviceConfigurationChange(\
        uint64_t in_change_action,\
        OSObject * in_change_info) APPLE_KEXT_OVERRIDE;\
\
    virtual kern_return_t\
    AbortDeviceConfigurationChange(\
        uint64_t in_change_action,\
        OSObject * in_change_info) APPLE_KEXT_OVERRIDE;\
\
    virtual kern_return_t\
    HandleChangeSampleRate(\
        double in_sample_rate) APPLE_KEXT_OVERRIDE;\
\


#if !KERNEL

extern OSMetaClass          * gIOUserAudioDeviceMetaClass;
extern const OSClassLoadInformation IOUserAudioDevice_Class;

class IOUserAudioDeviceMetaClass : public OSMetaClass
{
public:
    virtual kern_return_t
    New(OSObject * instance) override;
};

#endif /* !KERNEL */

#if !KERNEL

class  IOUserAudioDeviceInterface : public OSInterface
{
public:
};

struct IOUserAudioDevice_IVars;
struct IOUserAudioDevice_LocalIVars;

class IOUserAudioDevice : public IOUserAudioClockDevice, public IOUserAudioDeviceInterface
{
#if !KERNEL
    friend class IOUserAudioDeviceMetaClass;
#endif /* !KERNEL */

#if !KERNEL
public:
#ifdef IOUserAudioDevice_DECLARE_IVARS
IOUserAudioDevice_DECLARE_IVARS
#else /* IOUserAudioDevice_DECLARE_IVARS */
    union
    {
        IOUserAudioDevice_IVars * ivars;
        IOUserAudioDevice_LocalIVars * lvars;
    };
#endif /* IOUserAudioDevice_DECLARE_IVARS */
#endif /* !KERNEL */

#if !KERNEL
    static OSMetaClass *
    sGetMetaClass() { return gIOUserAudioDeviceMetaClass; };
#endif /* KERNEL */

    using super = IOUserAudioClockDevice;

#if !KERNEL
    IOUserAudioDevice_Methods
    IOUserAudioDevice_VirtualMethods
#endif /* !KERNEL */

};
#endif /* !KERNEL */


#endif /* !__DOCUMENTATION__ */

/* IOUserAudioDevice.iig:610-612 */


#pragma mark Private Class Extension
/* IOUserAudioDevice.iig:653- */

#endif /* IOUserAudioDevice_h */
