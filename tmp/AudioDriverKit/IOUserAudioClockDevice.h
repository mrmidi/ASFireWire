/* iig(DriverKit-456.120.3) generated from IOUserAudioClockDevice.iig */

/* IOUserAudioClockDevice.iig:1-41 */
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

#ifndef IOUserAudioClockDevice_h
#define IOUserAudioClockDevice_h

#include <DriverKit/IOService.h>  /* .iig include */
#include <DriverKit/OSSharedPtr.h>
#include <AudioDriverKit/IOUserAudioObject.h>  /* .iig include */

using namespace AudioDriverKit;

class IOUserAudioDriver;
class IODispatchQueue;
class IOUserAudioControl;

/* source class IOUserAudioClockDevice IOUserAudioClockDevice.iig:42-891 */

#if __DOCUMENTATION__
#define KERNEL IIG_KERNEL

/*!
 * @class IOUserAudioClockDevice
 *
 * @discussion
 * The IOUserAudioClockDevice class is a subclass of the IOUserAudioObject class.
 * IOUserAudioClockDevice handles the necessary configurations to be able to run IO.
 */
class LOCALONLY IOUserAudioClockDevice: public IOUserAudioObject
{
public:
    /*!
     * @function Create
     *
     * @abstract
     * static factory method to allocate and initialize an IOUserAudioClockDevice.
     *
     * @discussion
     * If IOUserAudioClockDevice is subclassed to override behavior, Create should not be
     * used to allocate/initialize the custom subclass.
     *
     * @param in_driver
     * The IOUserAudioDriver that owns this object.
     *
     * @param in_supports_prewarming
     * A bool that specifies if the device supports prewarming IO.
     *
     * @param in_device_uid
     * OSString pointer for the clock device unique identifier
     *
     * @param in_model_uid
     * OSString pointer for the clock device model unique identifier
     *
     * @param in_manufacturer_uid
     * OSString pointer for the clock device manufacturer unique identifier
	 *
	 * @param in_zero_timestamp_period
	 * A uint32_t whose value indicates the number of sample frames the host can
	 * expect between successive time stamps returned from GetZeroTimeStamp(). In
	 * other words, if GetZeroTimeStamp() returned a sample time of X, the host can
	 * expect that the next valid time stamp that will be returned will be X plus
	 * the value of this property.
	 *
     * @return
     * OSSharedPtr to an IOUserAudioClockDevice if it was successfully allocated and initialized
     */
    static OSSharedPtr<IOUserAudioClockDevice> Create(IOUserAudioDriver* in_driver,
                                                      bool in_supports_prewarming,
                                                      OSString* in_device_uid,
													  OSString* in_model_uid,
													  OSString* in_manufacturer_uid,
													  uint32_t in_zero_timestamp_period);
    
	/*!
	 * @function init
	 *
	 * @abstract
	 * Initializes a IOUserAudioClockDevice.
	 *
	 * @param in_driver
	 * The IOUserAudioDriver that owns this object.
	 *
	 * @param in_supports_prewarming
	 * A bool that specifies if the device supports prewarming IO.
	 *
	 * @param in_device_uid
	 * OSString pointer for the clock device unique identifier
	 *
	 * @param in_model_uid
	 * OSString pointer for the clock device model unique identifier
	 *
	 * @param in_manufacturer_uid
	 * OSString pointer for the clock device manufacturer unique identifier
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
					  uint32_t in_zero_timestamp_period);
	
	/*!
	 * @function free
	 *
	 * @abstract
	 * frees the IOUserAudioClockDevice.
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

#pragma mark IO Methods
	/*!
	 * @function StartIO
	 *
	 * @abstract
	 * Tells the clock device to start IO.
	 *
	 * @discussion
	 * Default implementation will always return kIOReturnSuccess.
	 * Subclass and override this method to handle any hardware specific things when IO is starting, then
	 * call super class to update IO state.
	 * This call is expected to always succeed or fail. The hardware can take as long
	 * as necessary in this call such that it always either succeeds (and kIOReturnSuccess) or fails.
	 *
	 * @param in_flags
	 * IOUserAudioStartStopFlags to indicate how IO is starting.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	virtual kern_return_t StartIO(IOUserAudioStartStopFlags in_flags);
	
	/*!
	 * @function StopIO
	 *
	 * @abstract
	 * Tells the clock device to stop IO.
	 *
	 * @discussion
	 * Default implementation will always return kIOReturnSuccess.
	 * Subclass and override this method to handle any hardware specific things when IO is stopping, then
	 * call super class to update IO state.
	 *
	 * @param in_flags
	 * IOUserAudioStartStopFlags to indicate how IO is stopping.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	virtual kern_return_t StopIO(IOUserAudioStartStopFlags in_flags);
	
	/*!
	 * @function RequestDeviceConfigurationChange
	 *
	 * @abstract
	 * Drivers invoke this routine to tell the host to initiate a configuration change operation.
	 *
	 * @discussion
	 * When a audio device object needs to change its structure or change any
	 * state related to IO for any reason, it must begin this operation by invoking
	 * this Host method. The device object may not perform the state change until
	 * the Host gives the device clearance to do so by invoking the
	 * PerformDeviceConfigurationChange() routine. Note that the call to
	 * PerformDeviceConfigurationChange() may be deferred to another thread at the
	 * discretion of the host.
	 *
	 * The sorts of changes that must go through this mechanism are anything that
	 * affects either the structure of the device or IO. This includes, but is not
	 * limited to, changing stream layout, adding/removing controls, changing the
	 * nominal sample rate of the device, changing any sample formats on any stream
	 * on the device, changing the size of the ring buffer, changing presentation
	 * latency, and changing the safety offset.
	 *
	 * @param in_change_action
	 * A uint64_t indicating the action the device object wants to take. It will
	 * be passed back to the device in the invocation of
	 * PerformDeviceConfigurationChange(). Note that this value is purely for
	 * driver's usage. The Host does not look at this value.
	 *
	 * @param in_change_info
	 * A pointer to an OSObject about the configuration change, can be nullptr. Note
	 * that this value is purely for the driver's usage. The Host does not
	 * look at this value.  Object reference should be retained/released as necessary.
	 *
	 * @return
	 * Returns kern_return_t indicating success or failure.
	 */
	kern_return_t RequestDeviceConfigurationChange(uint64_t in_change_action,
												   OSObject* in_change_info) LOCALONLY;

	/*!
	 * @function PerformDeviceConfigurationChange
	 *
	 * @abstract
	 * This is called by the host to allow the clock device to perform a configuration
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
	virtual kern_return_t PerformDeviceConfigurationChange(uint64_t change_action,
														   OSObject* in_change_info);

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
	virtual kern_return_t AbortDeviceConfigurationChange(uint64_t change_action,
														 OSObject* in_change_info);

#pragma mark Overridable Audio Device Setters
	/*!
	 * @function HandleChangeSampleRate
	 *
	 * @abstract
	 * Virtual method will be called when the clock device's sample rate will be changed.
	 *
	 * @discussion
	 * Default implementation will call SetSampleRate() and return kIOReturnSuccess.
	 * Subclass and override this method to handle changes to this value and
	 * return kIOReturnSucess upon success.
	 *
	 * @param in_sample_rate
	 * The double sample rate attempting to be set on the clock device.
	 *
	 * @return
	 * Returns kIOReturnSuccess on sucess. Upon sucess the value should be updated.
	 */
	virtual kern_return_t HandleChangeSampleRate(double in_sample_rate);

#pragma mark Audio Device Setters/Getters
	/*!
	 * @function GetSupportsPrewarming
	 *
	 * @abstract
	 * Get bool value indicating clock device's support for prewarming
	 *
	 * @discussion
	 * true if clock device supports prewarming.
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns bool
	 */
	bool GetSupportsPrewarming();
	
	/*!
	 * @function SetZeroTimeStampPeriod
	 *
	 * @abstract
	 * Set zero time stamp of the clock device.
	 *
	 * @discussion
	 * A uint32_t whose value indicates the number of sample frames the host can
	 * expect between successive time stamps returned from GetZeroTimeStamp(). In
	 * other words, if GetZeroTimeStamp() returned a sample time of X, the host can
	 * expect that the next valid time stamp that will be returned will be X plus
	 * the value of this property.
	 *
	 * Setting this value should only be done during PerformDeviceConfigurationChange() call.
	 * If the value needs to be changed, RequestDeviceConfigChange() should be called to allow
	 * IO to stop and the config change to be performed.
	 *
	 * Setting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_zts_period
	 * uint32_t of the zero time stamp period.
	 *
	 * @return
	 * Returns kern_return_t.
	 */
	kern_return_t SetZeroTimeStampPeriod(uint32_t in_zts_period);
	
	/*!
	 * @function GetZeroTimestampPeriod
	 *
	 * @abstract
	 * Get zero timestamp period of the clock device.
	 *
	 * @discussion
	 * A uint32_t whose value indicates the number of sample frames the host can
	 * expect between successive time stamps returned from GetZeroTimeStamp(). In
	 * other words, if GetZeroTimeStamp() returned a sample time of X, the host can
	 * expect that the next valid time stamp that will be returned will be X plus
	 * the value of this property.
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns uint32_t
	 */
	uint32_t GetZeroTimestampPeriod();
	
	/*!
	 * @function SetSampleRate
	 *
	 * @abstract
	 * Set the current sample rate for the clock device.
	 *
	 * @discussion
	 * Changing the sample rate will send a notification to the host to update the object state if successful.
	 * Setting the sample rate will be synchronized using the work queue created by the object.
	 *
	 * @param in_sample_rate
	 * The sample rate to set on the clock device..
	 *
	 * @return
	 * Returns kern_return_t.
	 */
	kern_return_t SetSampleRate(double in_sample_rate);
	
	/*!
	 * @function GetSampleRate
	 *
	 * @abstract
	 * Get sample rate of the clock device.
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns double
	 */
	double GetSampleRate();

	/*!
	 * @function SetAvailableSampleRates
	 *
	 * @abstract
	 * Set the available sample rates for the clock device.
	 *
	 * @discussion
	 * Changing the available sample rates will send a notification to the host to update the object state if successful.
	 * Setting the sample rates will be synchronized using the work queue created by the object.
	 *
	 * @param in_sample_rates
	 * Pointer to a buffer of double''s with size corresponding to in_num_rates.
	 *
	 * @param in_num_rates
	 * size_t of the number of sample rates in in_sample_rates buffer.
	 *
	 * @return
	 * Returns kern_return_t.
	 */
	kern_return_t SetAvailableSampleRates(const double* in_sample_rates,
										  size_t in_num_rates);

	/*!
	 * @function GetNumberAvailableSampleRates
	 *
	 * @abstract
	 * Get number of available sample rates of the clock device.
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns size_t.
	 */
	size_t GetNumberAvailableSampleRates();

	/*!
	 * @function GetAvailableSampleRates
	 *
	 * @abstract
	 * Get availble sample rates of the clock device.
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @param out_sample_rates
	 * Pointer to a buffer of double's with size corresponding to in_num_rates
	 *
	 * @param in_num_rates
	 *
	 * @param in_num_rates
	 * size_t of the number of rates in out_sample_rates buffer.
	 *
	 * @return
	 * Returns size_t indicating how many rates were set in the out_sample_rates buffer.
	 */
	size_t GetAvailableSampleRates(double* out_sample_rates,
								   size_t in_num_rates);

	/*!
	 * @function SetOutputLatency
	 *
	 * @abstract
	 * Set the output latency of the clock device in sample frames.
	 *
	 * @discussion
	 * Drivers can change the latency of the clock device dynamically.  A notification will be sent
	 * to the host to update the object state if successful.
	 * Setting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_latency
	 * uint32_t output latency value to set. Value is in sample frames.
	 *
	 * @return
	 * Returns kern_return_t.
	 */
	kern_return_t SetOutputLatency(uint32_t in_latency);

	/*!
	 * @function GetOutputLatency
	 *
	 * @abstract
	 * Get the output latency of the clock device in sample frames.
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns uint32_t
	 */
	uint32_t GetOutputLatency();

	/*!
	 * @function SetInputLatency
	 *
	 * @abstract
	 * Set the input latency of the clock device in sample frames.
	 *
	 * @discussion
	 * Drivers can change the latency of the clock device dynamically.  A notification will be sent
	 * to the host to update the object state if successful.
	 * Setting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_latency
	 * uint32_t input latency value to set. Value is in sample frames..
	 *
	 * @return
	 * Returns kern_return_t.
	 */
	kern_return_t SetInputLatency(uint32_t in_latency);
	
	/*!
	 * @function GetInputLatency
	 *
	 * @abstract
	 * Get the input latency of the clock device in sample frames.
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns uint32_t
	 */
	uint32_t GetInputLatency();

	/*!
	 * @function GetUID
	 *
	 * @abstract
	 * Get the unique identifier of the clock device
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns an OSSharedPtr to an OSString
	 */
	OSSharedPtr<OSString> GetUID();

	/*!
	 * @function SetTransportType
	 *
	 * @abstract
	 * Set the transport type of the IOUserAudioClockDevice
	 *
	 * @discussion
	 * Drivers can change the transport type of the clock device dynamically.  A notification will be sent
	 * to the host to update the object state if successful.
	 *
	 * @param in_transport_type
	 * IOUserAudioTransportType to set
	 *
	 * @return
	 * Returns kern_return_t
	 */
	kern_return_t SetTransportType(IOUserAudioTransportType in_transport_type);

	/*!
	 * @function GetTransportType
	 *
	 * @abstract
	 * Get the transport type of the IOUserAudioClockDevice.
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns IOUserAudioTransportType
	 */
	IOUserAudioTransportType GetTransportType();

	/*!
	 * @function SetClockDomain
	 *
	 * @abstract
	 * Set the uint32_t clock domain value of the IOUserAudioClockDevice.
	 * A uint32_t whose value indicates the clock domain to which the IOUserAudioClockDevice
	 * belongs. IOUserAudioClockDevice's that have the same value for this property are able to
	 * be synchronized in hardware. However, a value of 0 indicates that the clock
	 * domain for the device is unspecified and should be assumed to be separate
	 * from every other device's clock domain, even if they have the value of 0 as
	 * their clock domain as well.
	 *
	 * @discussion
	 * Drivers can change the clock domain  of the clock device dynamically.  A notification will be sent
	 * to the host to update the object state if successful.
	 *
	 * @param in_clock_domain
	 * uint32_t clock domain to set
	 *
	 * @return
	 * Returns kern_return_t
	 */
	kern_return_t SetClockDomain(uint32_t in_clock_domain);
	
	/*!
	 * @function GetClockDomain
	 *
	 * @abstract
	 * Get the uint32_t clock domain value of the IOUserAudioClockDevice.
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns uint32_t
	 */
	uint32_t GetClockDomain();

	/*!
	 * @function SetClockAlgorithm
	 *
	 * @abstract
	 * Set the IOUserAudioClockAlgorithm value of the IOUserAudioClockDevice
	 *
	 * @discussion
	 * Drivers can change the clock algorithm  of the clock device dynamically.  A notification will be sent
	 * to the host to update the object state if successful.
	 *
	 * @param in_clock_algorithm
	 * IOUserAudioClockAlgorithm  to set
	 *
	 * @return
	 * Returns kern_return_t
	 */
	kern_return_t SetClockAlgorithm(IOUserAudioClockAlgorithm in_clock_algorithm);

	/*!
	 * @function GetClockAlgorithm
	 *
	 * @abstract
	 * Get the IOUserAudioClockAlgorithm of the IOUserAudioClockDevice.
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns IOUserAudioClockAlgorithm
	 */
	IOUserAudioClockAlgorithm GetClockAlgorithm();

	/*!
	 * @function GetClockIsStable
	 *
	 * @abstract
	 * Set bool for clock stability of the IOUserAudioClockDevice.
	 *
	 * @discussion
	 * Setting the value will be synchronized using the work queue created by the object.
	 *
	 * @param in_clock_is_stable
	 * True if clock is stable. False if clock is unstable.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	kern_return_t SetClockIsStable(bool in_clock_is_stable);
	
	/*!
	 * @function GetClockIsStable
	 *
	 * @abstract
	 * Get bool for clock stability of the IOUserAudioClockDevice.
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns bool
	 */
	bool GetClockIsStable();
	
	/*!
	 * @function GetDeviceIsRunning
	 *
	 * @abstract
	 * Get bool value indicating if device is running.
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns bool
	 */
	bool GetDeviceIsRunning();

	/*!
	 * @function GetDeviceTransportState
	 *
	 * @abstract
	 * Get the IOUserAudioDeviceTransportState of the device.
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 *
	 * @return
	 * Returns IOUserAudioDeviceTransportState
	 */
	IOUserAudioDeviceTransportState GetDeviceTransportState();

	/*!
	 * @function SetDeviceIsAlive
	 *
	 * @abstract
	 * Set bool to indicate the device is alive.
	 *
	 * @discussion
	 * A bool where true means the device is ready and available and false
	 * means the device is unusable and will most likely go away shortly.
	 *
	 * @param in_is_alive
	 * True if device is alive.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	kern_return_t SetDeviceIsAlive(bool in_is_alive);
	
	/*!
	 * @function GetDeviceIsAlive
	 *
	 * @abstract
	 * Get bool value indicating if the device is alive
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 * Default value with be true when the device is created.
	 *
	 * @return
	 * Returns bool
	 */
	bool GetDeviceIsAlive();
	
	/*!
	 * @function SetIsHidden
	 *
	 * @abstract
	 * Set bool value indicating if the device is hidden
	 *
	 * @discussion
	 * A bool value where true indicates that the device is not included
	 * in the normal list of devices provided and cannot be the default device.
	 * Hidden devices can only be discovered by it's unique identifier
	 *
	 * @param in_is_hidden
	 * True if device is hidden.
	 *
	 * @return
	 * Returns kern_return_t
	 */
	kern_return_t SetIsHidden(bool in_is_hidden);
	
	/*!
	 * @function GetIsHidden
	 *
	 * @abstract
	 * Get bool value indicating if the device is hidden
	 *
	 * @discussion
	 * Getting the value will be synchronized using the work queue created by the object.
	 * Default value with be false when the device is created.
	 *
	 * @return
	 * Returns bool
	 */
	bool GetIsHidden();


#pragma mark Audio Controls
	/*!
	 * @function AddControl
	 *
	 * @abstract
	 * Add a IOUserAudioControl to the IOUserAudioClockDevice
	 *
	 * @discussion
	 * The control's reference count will be incremented if it was successfully added to the clock device.
	 *
	 * @param in_control
	 * IOUserAudioControl to add to the clock device.
	 *
	 * @return
	 * Returns kIOReturnSuccess if control was successfully added.
	 */
	kern_return_t AddControl(IOUserAudioControl* in_control);
	
	/*!
	 * @function RemoveControl
	 *
	 * @abstract
	 * Remove a IOUserAudioControl from the IOUserAudioClockDevice.
	 *
	 * @discussion
	 * The control's reference count will be decremented if it was successfully removed from the clock device.
	 *
	 * @param in_control
	 * IOUserAudioControl to remove from the clock device.
	 *
	 * @return
	 * Returns kIOReturnSuccess if control was successfully removed.
	 */
	kern_return_t RemoveControl(IOUserAudioControl* in_control);
	
#pragma mark Timestamp Getter/Setter
	/*!
	 * @function UpdateCurrentZeroTimestamp
	 *
	 * @abstract
	 * Update the current timestamp value.
	 *
	 * @discussion
	 * Updating the current timestamp should use the time passed in the hardware interrupt.
	 *
	 * @param in_sample_time
	 * uint64_t the most current sample time being tracked by the hardware device.
	 *
	 * @param in_host_time
	 * uint64_t the most current host time being tracked by the hardware device.
	 */
	void UpdateCurrentZeroTimestamp(uint64_t in_sample_time,
									uint64_t in_host_time);
	
	/*!
	 * @function GetCurrentZeroTimestamp
	 *
	 * @abstract
	 * Get the current zero timestamp value.
	 *
	 * @param out_sample_time
	 * pointer to uint64_t that will be set with last updated sample time.
	 *
	 * @param out_host_time
	 * pointer to uint64_t that will be set with last updated host time.
	 */
	void GetCurrentZeroTimestamp(uint64_t* out_sample_time,
								 uint64_t* out_host_time);

#pragma mark Client Status Info
	
	/*!
	 * @function GetCurrentClientSampleTime
	 *
	 * @abstract
	 * Get the current sample time in the ring buffer written to/read from by the client
	 *
	 * @param out_input_sample_time
	 * pointer to uint64_t that will be set with the current input sample time read by the client.
	 *
	 * @param out_output_sample_time
	 * pointer to uint64_t that will be set with the current output sample time written by the client.
	 */
	void GetCurrentClientSampleTime(uint64_t* out_input_sample_time,
									uint64_t* out_output_sample_time);
	
#pragma mark Control Restoration
	/*!
	 * @function SetWantsControlsRestored
	 *
	 * @abstract
	 * Setter on the device object that tells the host that the controls for the device should or should not be
	 * saved/restored when the device is first published. If the device doesn't implement
	 * this property, it is assumed that the settings should be saved and restored.
	 * Note that this should be set before the device is published to the host
	 *
	 * @param in_wants_controls_restored
	 * bool value indicating if the host should or should not restore control settings for the device
	 * A value of false indicates that the controls for the device should NOT be saved/restored
	 * A value of true indicated that the controls for the device should be saved/restored
	 */
	void SetWantsControlsRestored(bool in_wants_controls_restored);
};

#undef KERNEL
#else /* __DOCUMENTATION__ */

/* generated class IOUserAudioClockDevice IOUserAudioClockDevice.iig:42-891 */


#define IOUserAudioClockDevice_Methods \
\
public:\
\
    void\
    _SetClientIOStatusValues(\
        bool in_is_input,\
        uint64_t in_sample_time,\
        uint64_t in_host_time);\
\
    void\
    _GetClientIOStatusValues(\
        bool in_is_input,\
        uint64_t * out_sample_time,\
        uint64_t * out_host_time);\
\
    void *\
    _GetIOOperationStatusPointer(\
);\
\
    kern_return_t\
    _HandleChangeSampleRate(\
        double in_sample_rate);\
\
    IOUserAudioObjectID\
    _GetIOOperationStatusBufferObjectID(\
);\
\
    IOUserAudioObjectID\
    _GetClientStatusBufferObjectID(\
);\
\
    IOUserAudioObjectID\
    _GetTimestampBufferObjectID(\
);\
\
    IOUserAudioObjectID\
    _GetIOMemoryObjectID(\
        IOUserAudioObjectID in_stream_object_id);\
\
    OSSharedPtr<IOMemoryDescriptor>\
    _GetIOMemoryDescriptorFromObjectID(\
        IOUserAudioObjectID in_object_id);\
\
    static OSSharedPtr<IOUserAudioClockDevice>\
    Create(\
        IOUserAudioDriver * in_driver,\
        bool in_supports_prewarming,\
        OSString * in_device_uid,\
        OSString * in_model_uid,\
        OSString * in_manufacturer_uid,\
        uint32_t in_zero_timestamp_period);\
\
    kern_return_t\
    RequestDeviceConfigurationChange(\
        uint64_t in_change_action,\
        OSObject * in_change_info);\
\
    bool\
    GetSupportsPrewarming(\
);\
\
    kern_return_t\
    SetZeroTimeStampPeriod(\
        uint32_t in_zts_period);\
\
    uint32_t\
    GetZeroTimestampPeriod(\
);\
\
    kern_return_t\
    SetSampleRate(\
        double in_sample_rate);\
\
    double\
    GetSampleRate(\
);\
\
    kern_return_t\
    SetAvailableSampleRates(\
        const double * in_sample_rates,\
        size_t in_num_rates);\
\
    size_t\
    GetNumberAvailableSampleRates(\
);\
\
    size_t\
    GetAvailableSampleRates(\
        double * out_sample_rates,\
        size_t in_num_rates);\
\
    kern_return_t\
    SetOutputLatency(\
        uint32_t in_latency);\
\
    uint32_t\
    GetOutputLatency(\
);\
\
    kern_return_t\
    SetInputLatency(\
        uint32_t in_latency);\
\
    uint32_t\
    GetInputLatency(\
);\
\
    OSSharedPtr<OSString>\
    GetUID(\
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
    SetClockDomain(\
        uint32_t in_clock_domain);\
\
    uint32_t\
    GetClockDomain(\
);\
\
    kern_return_t\
    SetClockAlgorithm(\
        IOUserAudioClockAlgorithm in_clock_algorithm);\
\
    IOUserAudioClockAlgorithm\
    GetClockAlgorithm(\
);\
\
    kern_return_t\
    SetClockIsStable(\
        bool in_clock_is_stable);\
\
    bool\
    GetClockIsStable(\
);\
\
    bool\
    GetDeviceIsRunning(\
);\
\
    IOUserAudioDeviceTransportState\
    GetDeviceTransportState(\
);\
\
    kern_return_t\
    SetDeviceIsAlive(\
        bool in_is_alive);\
\
    bool\
    GetDeviceIsAlive(\
);\
\
    kern_return_t\
    SetIsHidden(\
        bool in_is_hidden);\
\
    bool\
    GetIsHidden(\
);\
\
    kern_return_t\
    AddControl(\
        IOUserAudioControl * in_control);\
\
    kern_return_t\
    RemoveControl(\
        IOUserAudioControl * in_control);\
\
    void\
    UpdateCurrentZeroTimestamp(\
        uint64_t in_sample_time,\
        uint64_t in_host_time);\
\
    void\
    GetCurrentZeroTimestamp(\
        uint64_t * out_sample_time,\
        uint64_t * out_host_time);\
\
    void\
    GetCurrentClientSampleTime(\
        uint64_t * out_input_sample_time,\
        uint64_t * out_output_sample_time);\
\
    void\
    SetWantsControlsRestored(\
        bool in_wants_controls_restored);\
\
\
protected:\
    /* _Impl methods */\
\
\
public:\
    /* _Invoke methods */\
\


#define IOUserAudioClockDevice_KernelMethods \
\
protected:\
    /* _Impl methods */\
\


#define IOUserAudioClockDevice_VirtualMethods \
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
        uint64_t change_action,\
        OSObject * in_change_info) APPLE_KEXT_OVERRIDE;\
\
    virtual kern_return_t\
    AbortDeviceConfigurationChange(\
        uint64_t change_action,\
        OSObject * in_change_info) APPLE_KEXT_OVERRIDE;\
\
    virtual kern_return_t\
    HandleChangeSampleRate(\
        double in_sample_rate) APPLE_KEXT_OVERRIDE;\
\


#if !KERNEL

extern OSMetaClass          * gIOUserAudioClockDeviceMetaClass;
extern const OSClassLoadInformation IOUserAudioClockDevice_Class;

class IOUserAudioClockDeviceMetaClass : public OSMetaClass
{
public:
    virtual kern_return_t
    New(OSObject * instance) override;
};

#endif /* !KERNEL */

#if !KERNEL

class  IOUserAudioClockDeviceInterface : public OSInterface
{
public:
    virtual bool
    init(IOUserAudioDriver * in_driver,
        bool in_supports_prewarming,
        OSString * in_device_uid,
        OSString * in_model_uid,
        OSString * in_manufacturer_uid,
        uint32_t in_zero_timestamp_period) = 0;

    virtual kern_return_t
    StartIO(IOUserAudioStartStopFlags in_flags) = 0;

    virtual kern_return_t
    StopIO(IOUserAudioStartStopFlags in_flags) = 0;

    virtual kern_return_t
    PerformDeviceConfigurationChange(uint64_t change_action,
        OSObject * in_change_info) = 0;

    virtual kern_return_t
    AbortDeviceConfigurationChange(uint64_t change_action,
        OSObject * in_change_info) = 0;

    virtual kern_return_t
    HandleChangeSampleRate(double in_sample_rate) = 0;

    bool
    init_Call(IOUserAudioDriver * in_driver,
        bool in_supports_prewarming,
        OSString * in_device_uid,
        OSString * in_model_uid,
        OSString * in_manufacturer_uid,
        uint32_t in_zero_timestamp_period)  { return init(in_driver, in_supports_prewarming, in_device_uid, in_model_uid, in_manufacturer_uid, in_zero_timestamp_period); };\

    kern_return_t
    StartIO_Call(IOUserAudioStartStopFlags in_flags)  { return StartIO(in_flags); };\

    kern_return_t
    StopIO_Call(IOUserAudioStartStopFlags in_flags)  { return StopIO(in_flags); };\

    kern_return_t
    PerformDeviceConfigurationChange_Call(uint64_t change_action,
        OSObject * in_change_info)  { return PerformDeviceConfigurationChange(change_action, in_change_info); };\

    kern_return_t
    AbortDeviceConfigurationChange_Call(uint64_t change_action,
        OSObject * in_change_info)  { return AbortDeviceConfigurationChange(change_action, in_change_info); };\

    kern_return_t
    HandleChangeSampleRate_Call(double in_sample_rate)  { return HandleChangeSampleRate(in_sample_rate); };\

};

struct IOUserAudioClockDevice_IVars;
struct IOUserAudioClockDevice_LocalIVars;

class IOUserAudioClockDevice : public IOUserAudioObject, public IOUserAudioClockDeviceInterface
{
#if !KERNEL
    friend class IOUserAudioClockDeviceMetaClass;
#endif /* !KERNEL */

#if !KERNEL
public:
#ifdef IOUserAudioClockDevice_DECLARE_IVARS
IOUserAudioClockDevice_DECLARE_IVARS
#else /* IOUserAudioClockDevice_DECLARE_IVARS */
    union
    {
        IOUserAudioClockDevice_IVars * ivars;
        IOUserAudioClockDevice_LocalIVars * lvars;
    };
#endif /* IOUserAudioClockDevice_DECLARE_IVARS */
#endif /* !KERNEL */

#if !KERNEL
    static OSMetaClass *
    sGetMetaClass() { return gIOUserAudioClockDeviceMetaClass; };
#endif /* KERNEL */

    using super = IOUserAudioObject;

#if !KERNEL
    IOUserAudioClockDevice_Methods
    IOUserAudioClockDevice_VirtualMethods
#endif /* !KERNEL */

};
#endif /* !KERNEL */


#endif /* !__DOCUMENTATION__ */

/* IOUserAudioClockDevice.iig:893-895 */


#pragma mark Private Class Extension
/* IOUserAudioClockDevice.iig:936- */

#endif /* IOUserAudioClockDevice_h */
