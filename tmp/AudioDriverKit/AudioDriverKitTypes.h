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

#ifndef AudioDriverKitTypes_h
#define AudioDriverKitTypes_h

#include <DriverKit/IOLib.h>

/*!
 * @constant kIOUserAudioDriverUserClientType
 *
 * @abstract
 * User client type required for connection to the Host.
 * Passed as an arguement to IOService::NewUserClient when Core Audio Host is
 * creating a new user client.
 */
#define kIOUserAudioDriverUserClientType 1128363364

namespace AudioDriverKit
{

/*!
 * @enum IOUserAudioReservedConfigChangeAction
 *
 * @abstract
 * Reserved configuration change IDs when changing object state that requires a config change
 *
 * @constant SampleRate
 *
 * @constant RingBufferFrameSize
 *
 * @constant StreamFormat
 */
enum class IOUserAudioReservedConfigChangeAction : uint64_t
{
	SampleRate = 1,
	RingBufferFrameSize = 2,
	StreamFormat = 3
};

/*!
 * @enum IOUserAudioStartStopFlags
 *
 * @abstract
 * Flags used to indicate how IO is starting or stopping.
 *
 * @constant None
 * IO is starting or stopping for normal IO operation, which should result in
 * enabling/disabling all necessary hardware.
 *
 * @constant Prewarm
 * IO is starting or stoping for prewarming.  The minimal hardware should be
 * enabled/disabled to minimize transition to normal IO operation.
 *
 * @discussion
 * Additional bits are reserved for future use
 */
enum class IOUserAudioStartStopFlags : uint64_t
{
	None = 0,
	Prewarm = (1L << 0),
};

/*!
 * @enum IOUserAudioDeviceTransportState
 *
 * @abstract
 * The current transport state of the device.
 *
 * @constant Stopped
 * Device transport state is stopped.  The hardware necessary for IO should be disabled.
 *
 * @constant Prewarmed
 * Device transport state is prewarmed.  The minimal hardware for IO should be
 * enabled to minimize transition to normal IO operation.
 *
 * @constant Running
 * Device transport state is running.  The hardware should be
 * enabled to fully run IO.
 */
enum class IOUserAudioDeviceTransportState : uint64_t
{
	Stopped = 0,
	Prewarmed = 1,
	Running = 2,
};


//==================================================================================================
#pragma mark -
#pragma mark Basic Types

/*!
 * @typedef IOUserAudioObjectID
 *
 * @abstract
 * A uint32_t that provides a handle on a specific IOUserAudioObject.
 */
typedef uint32_t IOUserAudioObjectID;

/*!
 * @constant kIOUserAudioObjectIDDriver
 *
 * @abstract
 * IOUserAudioObjectID's that are always the same
 *
 * @constant IOUserAudioObjectIDTypeDriver
 * The IOUserAudioObjectID that always refers to the one and only instance of the
 * IOUserAudioDriver
 */
constexpr IOUserAudioObjectID kIOUserAudioObjectIDDriver = 1;

/*!
 * @typedef IOUserAudioObjectPropertySelector
 *
 * @abstract
 * An IOUserAudioObjectPropertySelector is a four char code that identifies,
 * along with the IOUserAudioObjectPropertyScope and
 * IOUserAudioObjectPropertyElement, a specific piece of information about an
 * IOUserAudioObject.
 *
 * @discussion
 * The property selector specifies the general classification of the property
 * such as volume, stream format, latency, etc. Note that each class has a
 * different set of selectors. A subclass inherits its super class's set of
 * selectors, although it may not implement them all.
 */
typedef uint32_t IOUserAudioObjectPropertySelector;

/*!
 * @enum IOUserAudioObjectPropertyScope
 *
 * @abstract
 * An IOUserAudioObjectPropertyScope is a four char code that identifies, along with the
 * IOUserAudioObjectPropertySelector and IOUserAudioObjectPropertyElement, a
 * specific piece of information about an IOUserAudioObject.
 *
 * @discussion
 * The scope specifies the section of the object in which to look for the property,
 * such as input, output, global, etc. Note that each class
 * has a different set of scopes. A subclass inherits its superclass's set of
 * scopes.
 *
 * @constant kIOUserAudioObjectPropertyScopeGlobal
 * The IOUserAudioObjectPropertyScope for properties that apply to the object as a
 * whole. All objects have a global scope and for most it is their only scope.
 *
 * @constant kIOUserAudioObjectPropertyScopeInput
 * The IOUserAudioObjectPropertyScope for properties that apply to the input side of
 * an object.
 *
 * @constant kIOUserAudioObjectPropertyScopeOutput
 * The IOUserAudioObjectPropertyScope for properties that apply to the output side of
 * an object.
 *
 * @constant kIOUserAudioObjectPropertyScopePlayThrough
 * The IOUserAudioObjectPropertyScope for properties that apply to the play through
 * side of an object.
 */
enum class IOUserAudioObjectPropertyScope : uint32_t
{
	Global = 'glob',
	Input = 'inpt',
	Output = 'outp',
	PlayThrough = 'ptru'
};

/*!
 * @typedef IOUserAudioObjectPropertyElement
 *
 * @abstract
 * An IOUserAudioObjectPropertyElement is an integer that
 * identifies, along with the IOUserAudioObjectPropertySelector and
 * IOUserAudioObjectPropertyScope, a specific piece of information about an
 * IOUserAudioObject.
 *
 * @discussion
 * The element selects one of possibly many items in the
 * section of the object in which to look for the property. Elements are number
 * sequentially where 0 represents the main element. Elements are particular
 * to an instance of a class, meaning that two instances can have different
 * numbers of elements in the same scope. There is no inheritance of elements.
 */
typedef uint32_t IOUserAudioObjectPropertyElement;

/*!
 * @constant kIOUserAudioObjectPropertyElementMain
 * The IOUserAudioObjectPropertyElement value for properties that apply to the main
 * element or to the entire scope.
*/
constexpr IOUserAudioObjectPropertyElement IOUserAudioObjectPropertyElementMain = 0;

/*!
 * @struct IOUserAudioObjectPropertyAddress
 *
 * @abstract
 * An IOUserAudioObjectPropertyAddress collects the three
 * parts that identify a specific property together in a struct for easy
 * transmission.
 *
 * @field mSelector
 * The IOUserAudioObjectPropertySelector for the property.
 *
 * @field mScope
 * The IOUserAudioObjectPropertyScope for the property.
 *
 * @field mElement
 * The IOUserAudioObjectPropertyElement for the property.
 */
struct IOUserAudioObjectPropertyAddress
{
	IOUserAudioObjectPropertySelector mSelector;
	IOUserAudioObjectPropertyScope mScope;
	IOUserAudioObjectPropertyElement mElement;
};

/*!
 * @enum IOUserAudioCustomPropertyDataType
 *
 * @abstract
 * Data/Qualifier  type used for custom properties
 *
 * @constant CustomPropertyDataTypeNone
 * The custom property does not have any data
 *
 * @constant CustomPropertyDataTypeOSString
 * The custom property data type is an OSString
 *
 * @constant CustomPropertyDataTypeOSDictionary
 * The custom property data type is an OSDictionary
 */
enum class IOUserAudioCustomPropertyDataType : uint32_t
{
	None = 0,
	String = 'cfst',
	Dictionary = 'plst'
};


/*!
 * @struct IOUserAudioCustomPropertyInfo
 *
 * @abstract
 * The IOUserAudioCustomPropertyInfo struct is used to describe enough about
 * a custom property to allow the Host to marshal the data between the Host and
 * its clients.
 *
 * @field mSelector
 * The IOUserAudioObjectPropertySelector of the custom property
 *
 * @field mPropertyDataType
 * A IOUserAudioCustomPropertyDataType whose value indicates the
 * data type of the data of the custom property.
 *
 * @field mQualifierDataType
 * A IOUserAudioCustomPropertyDataType whose value indicates the
 * data type of the qualifier data of the custom property.
 */
struct IOUserAudioCustomPropertyInfo
{
	IOUserAudioObjectPropertySelector mSelector;
	IOUserAudioCustomPropertyDataType mPropertyDataType;
	IOUserAudioCustomPropertyDataType mQualifierDataType;
};

/*!
 * @enum IOUserAudioTransportType
 *
 * @abstract
 * Commonly used values for transport types
 *
 * @constant Unknown
 * The transport type ID returned when a device doesn't provide a transport
 * type.
 *
 * @constant BuiltIn
 * The transport type ID for AudioDevices built into the system.
 *
 * @constant PCI
 * The transport type ID for AudioDevices connected via the PCI bus.
 *
 * @constant USB
 * The transport type ID for AudioDevices connected via USB.
 *
 * @constant FireWire
 * The transport type ID for AudioDevices connected via FireWire.
 *
 * @constant Bluetooth
 * The transport type ID for AudioDevices connected via Bluetooth.
 *
 * @constant BluetoothLE
 * The transport type ID for AudioDevices connected via Bluetooth Low Energy.
 *
 * @constant HDMI
 * The transport type ID for AudioDevices connected via HDMI.
 *
 * @constant DisplayPort
 * The transport type ID for AudioDevices connected via DisplayPort.
 *
 * @constant AirPlay
 * The transport type ID for AudioDevices connected via AirPlay.
 *
 * @constant AVB
 * The transport type ID for AudioDevices connected via AVB.
 *
 * @constant ThunderBolt
 * The transport type ID for AudioDevices connected via Thunderbolt.
 */
enum class IOUserAudioTransportType : uint32_t
{
	Unknown = 0,
	BuiltIn = 'bltn',
	PCI = 'pci ',
	USB = 'usb ',
	FireWire = '1394',
	Bluetooth = 'blue',
	BluetoothLE = 'blea',
	HDMI = 'hdmi',
	DisplayPort = 'dprt',
	AirPlay = 'airp',
	AVB = 'eavb',
	Thunderbolt = 'thun'
};

/*!
 * @enum IOUserAudioStreamTerminalType
 *
 * @abstract
 * Various constants that describe the terminal type of an IOUserAudioStream.
 *
 * @constant Unknown
 * The ID used when the terminal type for the IOUserAudioStream is not known.
 *
 * @constant Line
 * The ID for a terminal type of a line level stream. Note that this applies to
 * both input streams and output streams
 *
 * @constant DigitalAudioInterface
 * The ID for a terminal type of stream from/to a digital audio interface as
 * defined by ISO 60958 (aka SPDIF or AES/EBU). Note that this applies to both
 * input streams and output streams
 *
 * @constant Speaker
 * The ID for a terminal type of a speaker.
 *
 * @constant Headphones
 * The ID for a terminal type of headphones.
 *
 * @constant LFESpeaker
 * The ID for a terminal type of a speaker for low frequency effects.
 *
 * @constant ReceiverSpeaker
 * The ID for a terminal type of a speaker on a telephone handset receiver.
 *
 * @constant Microphone
 * The ID for a terminal type of a microphone.
 *
 * @constant HeadsetMicrophone
 * The ID for a terminal type of a microphone attached to an headset.
 *
 * @constant ReceiverMicrophone
 * The ID for a terminal type of a microphone on a telephone handset receiver.
 *
 * @constant TTY
 * The ID for a terminal type of a device providing a TTY signal.
 *
 * @constant HDMI
 * The ID for a terminal type of a stream from/to an HDMI port.
 *
 * @constant DisplayPort
 * The ID for a terminal type of a stream from/to an DisplayPort port.
 */
enum class IOUserAudioStreamTerminalType : uint32_t
{
	Unknown = 0,
	Line = 'line',
	DigitalAudioInterface = 'spdf',
	Speaker = 'spkr',
	Headphones = 'hdph',
	LFESpeaker = 'lfes',
	ReceiverSpeaker = 'rspk',
	Microphone = 'micr',
	HeadsetMicrophone = 'hmic',
	ReceiverMicrophone = 'rmic',
	TTY = 'tty_',
	HDMI = 'hdmi',
	DisplayPort = 'dprt'
};

/*!
 * @enum IOUserAudioClockAlgorithm
 *
 * @abstract
 * Clock Smoothing Algorithm Selectors.  The valid values for IOUserAudioClockAlgorithm
 *
 * @constant Raw
 * When this value for the clock algorithm is specified, the Host will not
 * apply any filtering to the time stamps returned from GetCurrentZeroTimeStamp(). The
 * values will be used as-is.
 *
 * @constant SimpleIIR
 * When this value for the clock algorithm is specified, the Host applies a
 * simple IIR filter to the time stamp stream. This is the default algorithm
 * used for devices that don't implement DevicePropertyClockAlgorithm.
 *
 * @constant TwelvePtMovingWindowAverage
 * This clock algorithm uses a 12 point moving window average to filter the time
 * stamps returned from GetCurrentZeroTimeStamp().
 */
enum class IOUserAudioClockAlgorithm : uint32_t
{
	Raw = 'raww',
	SimpleIIR = 'iirf',
	TwelvePtMovingWindowAverage = 'mavg'
};

/*!
 * @enum IOUserAudioClassID
 *
 * @abstract
 * IOUserAudioClassID's are used to identify the class of an IOUserAudiooObject.
 *
 * @constant Object
 * The IOUserAudioClassID that identifies the IOUserAudioObject class.
 *
 * @constant Driver
 * The IOUserAudioClassID that identifies the IOUserAudioDriver class
 *
 * @constant Box
 * The IOUserAudioClassID that identifies the IOUserAudioBox class
 *
 * @constant Clock
 * The IOUserAudioClassID that identifies the IOUserAudioClockDevice class
 *
 * @constant Device
 * The IOUserAudioClassID that identifies the IOUserAudioDevice class
 *
 * @constant Stream
 * The IOUserAudioClassID that identifies the IOUserAudioStream class
 *
 * @constant Control
 * The IOUserAudioClassID that identifies the IOUserAudioControl class
 *
 * @constant SliderControl
 * The IOUserAudioClassID that identifies the IOUserAudioSliderControl class
 *
 * @constant LevelControl
 * The IOUserAudioClassID that identifies the IOUserAudioLevelControl class
 *
 * @constant VolumeControl
 * The IOUserAudioClassID that identifies the IOUserAudioVolumeControl class
 *
 * @constant LFEVolumeControl
 * A subclass of the IOUserAudioLevelControl class for an LFE channel that results from
 * bass management. Note that LFE channels that are represented as normal audio
 * channels must use IOUserAudioClassID VolumeControl to manipulate the level.
 *
 * @constant BooleanControl
 * The IOUserAudioClassID that identifies the IOUserAudioBooleanControl class
 *
 * @constant SoloControl
 * A subclass of the IOUserAudioBooleanControl class where a true value means that
 * solo is enabled making just that element audible and the other elements
 * inaudible.
 *
 * @constant JackControl
 * A subclass of the IOUserAudioBooleanControl class where a true value means
 * something is plugged into that element.
 *
 * @constant LFEMuteControl
 * A subclass of the IOUserAudioBooleanControl class where true means that mute is
 * enabled making that LFE element inaudible. This control is for LFE channels
 * that result from bass management. Note that LFE channels that are
 * represented as normal audio channels must use an AudioMuteControl.
 *
 * @constant PhantomPowerControl
 * A subclass of the IOUserAudioBooleanControl class where true means that the
 * element's hardware has phantom power enabled.
 *
 * @constant PhaseInvertControl
 * A subclass of the IOUserAudioBooleanControl class where true means that the phase
 * of the signal on the given element is being inverted by 180 degrees.
 *
 * @constant ClipLightControl
 * A subclass of the IOUserAudioBooleanControl class where true means that the signal
 * for the element has exceeded the sample range. Once a clip light is turned
 * on, it is to stay on until either the value of the control is set to false
 * or the current IO session stops and a new IO session starts.
 *
 * @constant TalkbackControl
 * An IOUserAudioBooleanControl where true means that the talkback channel is
 * enabled. This control is for talkback channels that are handled outside of
 * the regular IO channels. If the talkback channel is among the normal IO
 * channels, it will use IOUserAudioMuteControl.
 *
 * @constant ListenbackControl
 * An IOUserAudioBooleanControl where true means that the listenback channel is
 * audible. This control is for listenback channels that are handled outside of
 * the regular IO channels. If the listenback channel is among the normal IO
 * channels, it will use IOUserAudioMuteControl.
 *
 * @constant MuteControl
 * The IOUserAudioClassID that identifies the IOUserAudioMuteControl class
 *
 * @constant SelectorControl
 * The IOUserAudioClassID that identifies the IOUserAudioSelectorControl class
 *
 * @constant DataSourceControl
 * A subclass of the IOUserAudioSelectorControl class that identifies where the data
 * for the element is coming from.
 *
 * @constant DataDestinationControl
 * A subclass of the IOUserAudioSelectorControl class that identifies where the data
 * for the element is going.
 *
 * @constant ClockSourceControl
 * A subclass of the IOUserAudioSelectorControl class that identifies where the
 * timing info for the object is coming from.
 *
 * @constant LineLevelControl
 * A subclass of the IOUserAudioSelectorControl class that identifies the nominal
 * line level for the element. Note that this is not a gain stage but rather
 * indicating the voltage standard (if any) used for the element, such as
 * +4dBu, -10dBV, instrument, etc.
 *
 * @constant HighPassFilerControl
 * A subclass of the IOUserAudioSelectorControl class that indicates the setting for
 * the high pass filter on the given element.
 *
 * @constant StereoPanControl
 * The IOUserAudioClassID that identifies the IOUserAudioStereoPanControl class
 */
enum class IOUserAudioClassID : uint32_t
{
	Object = 'aobj',
	Driver = 'aplg',
	Box = 'abox',
	Clock = 'aclk',
	Device = 'adev',
	Stream = 'astr',
	Control = 'actl',
	SliderControl = 'sldr',
	LevelControl = 'levl',
	VolumeControl = 'vlme',
	LFEVolumeControl = 'subv',
	BooleanControl = 'togl',
	MuteControl = 'mute',
	SoloControl = 'solo',
	JackControl = 'jack',
	LFEMuteControl = 'subm',
	PhantomPowerControl = 'phan',
	PhaseInvertControl = 'phsi',
	ClipLightControl = 'clip',
	TalkbackControl = 'talb',
	ListenbackControl = 'lsnb',
	SelectorControl = 'slct',
	DataSourceControl = 'dsrc',
	DataDestinationControl = 'dest',
	ClockSourceControl = 'clck',
	LineLevelControl = 'nlvl',
	HighPassFilterControl = 'hipf',
	StereoPanControl = 'span'
};

/*!
 * @enum IOUserAudioStreamDirection
 *
 * @abstract
 * A uint32_t to indicate an IOUserAudioStream class as either input or output direction
 *
 * @constant Output
 * Output stream direction
 *
 * @constant Input
 * Input stream direction
 */
enum class IOUserAudioStreamDirection : uint32_t
{
	Output = 0,
	Input = 1
};

/*!
 * @enum IOUserAudioFormatID
 *
 * @abstract
 * The IOUserAudioFormatIDs used to identify individual formats of audio data.
 *
 * @constant FormatLinearPCM
 * Linear PCM, uses the standard flags.
 *
 * @constant AC3
 * AC-3, has no flags.
 *
 * @constant 60958AC3
 * AC-3 packaged for transport over an IEC 60958 compliant digital audio
 * interface. Uses the standard flags.
 *
 * @constant AppleIMA4
 * Apples implementation of IMA 4:1 ADPCM, has no flags.
 *
 * @constant MPEG4AAC
 * MPEG-4 Low Complexity AAC audio object, has no flags.
 *
 * @constant MPEG4CELP
 *
 * MPEG-4 CELP audio object, has no flags
 *
 * @constant MPEG4HVXC
 * MPEG-4 HVXC audio object, has no flags.
 *
 * @constant MPEG4TwinVQ
 * MPEG-4 TwinVQ audio object type, has no flags.
 *
 * @constant MACE3
 * MACE 3:1, has no flags.
 *
 * @constant MACE6
 * MACE 6:1, has no flags.
 *
 * @constant ULaw
 * µLaw 2:1, has no flags.
 *
 * @constant ALaw
 * aLaw 2:1, has no flags.
 *
 * @constant QDesign
 * QDesign music, has no flags
 *
 * @constant QDesign2
 * QDesign2 music, has no flags
 *
 * @constant QUALCOMM
 * QUALCOMM PureVoice, has no flags
 *
 * @constant MPEGLayer1
 * MPEG-1/2, Layer 1 audio, has no flags
 *
 * @constant MPEGLayer2
 * MPEG-1/2, Layer 2 audio, has no flags
 *
 * @constant MPEGLayer3
 * MPEG-1/2, Layer 3 audio, has no flags
 *
 * @constant TimeCode
 * A stream of IOAudioTimeStamps, uses the IOAudioTimeStamp flags.
 *
 * @constant MIDIStream
 * A stream of MIDIPacketLists where the time stamps in the MIDIPacketList are
 * sample offsets in the stream. The mSampleRate field is used to describe how
 * time is passed in this kind of stream and an AudioUnit that receives or
 * generates this stream can use this sample rate, the number of frames it is
 * rendering and the sample offsets within the MIDIPacketList to define the
 * time for any MIDI event within this list. It has no flags.
 *
 * @constant ParameterValueStream
 * A "side-chain" of Float32 data that can be fed or generated by an AudioUnit
 * and is used to send a high density of parameter value control information.
 * An AU will typically run a ParameterValueStream at either the sample rate of
 * the AudioUnit's audio data, or some integer divisor of this (say a half or a
 * third of the sample rate of the audio). The Sample Rate of the ASBD
 * describes this relationship. It has no flags.
 *
 * @constant AppleLossless
 * Apple Lossless, the flags indicate the bit depth of the source material.
 *
 * @constant MPEG4AAC_HE
 * MPEG-4 High Efficiency AAC audio object, has no flags.
 *
 * @constant MPEG4AAC_LD
 * MPEG-4 AAC Low Delay audio object, has no flags.
 *
 * @constant MPEG4AAC_ELD
 * MPEG-4 AAC Enhanced Low Delay audio object, has no flags. This is the formatID of
 * the base layer without the SBR extension. See also FormatMPEG4AAC_ELD_SBR
 *
 * @constant MPEG4AAC_ELD_SBR
 * MPEG-4 AAC Enhanced Low Delay audio object with SBR extension layer, has no flags.
 *
 * @constant MPEG4AAC_HE_V2
 * MPEG-4 High Efficiency AAC Version 2 audio object, has no flags.
 *
 * @constant MPEG4AAC_Spatial
 * MPEG-4 Spatial Audio audio object, has no flags.
 *
 * @constant MPEGD_USAC
 * MPEG-D Unified Speech and Audio Coding, has no flags.
 *
 * @constant AMR
 * The AMR Narrow Band speech codec.
 *
 * @constant AMR_WB
 * The AMR Wide Band speech codec.
 *
 * @constant Audible
 * The format used for Audible audio books. It has no flags.
 *
 * @constant iLBC
 * The iLBC narrow band speech codec. It has no flags.
 *
 * @constant DVIIntelIMA
 * DVI/Intel IMA ADPCM - ACM code 17.
 *
 * @constant MicrosoftGSM
 * Microsoft GSM 6.10 - ACM code 49.
 *
 * @constant AES3
 * This format is defined by AES3-2003, and adopted into MXF and MPEG-2
 * containers and SDTI transport streams with SMPTE specs 302M-2002 and
 * 331M-2000. It has no flags.
 *
 * @constant EnhancedAC3
 * Enhanced AC-3, has no flags.
 *
 * @constant FLAC
 * Free Lossless Audio Codec, the flags indicate the bit depth of the source material.
 *
 * @constant Opus
 * Opus codec, has no flags.
 */
enum class IOUserAudioFormatID : uint32_t
{
	LinearPCM = 'lpcm',
	AC3 = 'ac-3',
	AC360958 = 'cac3',
	AppleIMA4 = 'ima4',
	MPEG4AAC = 'aac ',
	MPEG4CELP = 'celp',
	MPEG4HVXC = 'hvxc',
	MPEG4TwinVQ = 'twvq',
	MACE3 = 'MAC3',
	MACE6 = 'MAC6',
	ULaw = 'ulaw',
	ALaw = 'alaw',
	QDesign = 'QDMC',
	QDesign2 = 'QDM2',
	QUALCOMM = 'Qclp',
	MPEGLayer1 = '.mp1',
	MPEGLayer2 = '.mp2',
	MPEGLayer3 = '.mp3',
	TimeCode = 'time',
	MIDIStream = 'midi',
	ParameterValueStream = 'apvs',
	AppleLossless = 'alac',
	MPEG4AAC_HE = 'aach',
	MPEG4AAC_LD = 'aacl',
	MPEG4AAC_ELD = 'aace',
	MPEG4AAC_ELD_SBR = 'aacf',
	MPEG4AAC_ELD_V2 = 'aacg',
	MPEG4AAC_HE_V2 = 'aacp',
	MPEG4AAC_Spatial = 'aacs',
	MPEGD_USAC = 'usac',
	AMR = 'samr',
	AMR_WB = 'sawb',
	Audible = 'AUDB',
	iLBC = 'ilbc',
	DVIIntelIMA = 0x6D730011,
	MicrosoftGSM = 0x6D730031,
	AES3 = 'aes3',
	EnhancedAC3 = 'ec-3',
	FLAC = 'flac',
	Opus = 'opus'
};


/*!
 * @enum IOUserAudioFormatFlags
 *
 * @abstract
 * Standard IOUserAudioFormatFlags values for IOUserAudioStreamBasicDescription.
 * These are the standard AudioFormatFlags for use in the mFormatFlags field of the
 * AudioStreamBasicDescription structure.
 *
 * @discussion
 * Typically, when an ASBD is being used, the fields describe the complete layout
 * of the sample data in the buffers that are represented by this description -
 * where typically those buffers are represented by an AudioBuffer that is
 * contained in an AudioBufferList.
 *
 * However, when an ASBD has the FormatFlagIsNonInterleaved flag, the
 * AudioBufferList has a different structure and semantic. In this case, the ASBD
 * fields will describe the format of ONE of the AudioBuffers that are contained in
 * the list, AND each AudioBuffer in the list is determined to have a single (mono)
 * channel of audio data. Then, the ASBD's mChannelsPerFrame will indicate the
 * total number of AudioBuffers that are contained within the AudioBufferList -
 * where each buffer contains one channel. This is used primarily with the
 * AudioUnit (and AudioConverter) representation of this list - and won't be found
 * in the AudioHardware usage of this structure.
 *
 * @constant FormatFlagIsFloat
 * Set for floating point, clear for integer.
 *
 * @constant FormatFlagIsBigEndian
 * Set for big endian, clear for little endian.
 *
 * @constant FormatFlagIsSignedInteger
 * Set for signed integer, clear for unsigned integer. This is only valid if
 * FormatFlagIsFloat is clear.
 *
 * @constant FormatFlagIsPacked
 * Set if the sample bits occupy the entire available bits for the channel,
 * clear if they are high or low aligned within the channel. Note that even if
 * this flag is clear, it is implied that this flag is set if the
 * AudioStreamBasicDescription is filled out such that the fields have the
 * following relationship:
 * ((mBitsPerSample / 8) * mChannelsPerFrame) == mBytesPerFrame
 *
 * @constant FormatFlagIsAlignedHigh
 * Set if the sample bits are placed into the high bits of the channel, clear
 * for low bit placement. This is only valid if FormatFlagIsPacked is
 * clear.
 *
 * @constant FormatFlagIsNonInterleaved
 * Set if the samples for each channel are located contiguously and the
 * channels are layed out end to end, clear if the samples for each frame are
 * layed out contiguously and the frames layed out end to end.
 *
 * @constant FormatFlagIsNonMixable
 * Set to indicate when a format is non-mixable. Note that this flag is only
 * used when interacting with the HAL's stream format information. It is not a
 * valid flag for any other uses.
 *
 * @constant FormatFlagsAreAllClear
 * Set if all the flags would be clear in order to preserve 0 as the wild card
 * value.
 *
 * @constant LinearPCMFormatFlagIsFloat
 * Synonym for FormatFlagIsFloat.
 *
 * @constant LinearPCMFormatFlagIsBigEndian
 * Synonym for FormatFlagIsBigEndian.
 *
 * @constant LinearPCMFormatFlagIsSignedInteger
 * Synonym for FormatFlagIsSignedInteger.
 *
 * @constant LinearPCMFormatFlagIsPacked
 * Synonym for FormatFlagIsPacked.
 *
 * @constant LinearPCMFormatFlagIsAlignedHigh
 * Synonym for FormatFlagIsAlignedHigh.
 *
 * @constant LinearPCMFormatFlagIsNonInterleaved
 * Synonym for FormatFlagIsNonInterleaved.
 *
 * @constant LinearPCMFormatFlagIsNonMixable
 * Synonym for FormatFlagIsNonMixable.
 *
 * @constant LinearPCMFormatFlagsAreAllClear
 * Synonym for FormatFlagsAreAllClear.
 *
 * @constant LinearPCMFormatFlagsSampleFractionShift
 * The linear PCM flags contain a 6-bit bitfield indicating that an integer
 * format is to be interpreted as fixed point. The value indicates the number
 * of bits are used to represent the fractional portion of each sample value.
 * This constant indicates the bit position (counting from the right) of the
 * bitfield in mFormatFlags.
 *
 * @constant LinearPCMFormatFlagsSampleFractionMask
 * number_fractional_bits = (mFormatFlags & LinearPCMFormatFlagsSampleFractionMask) >> LinearPCMFormatFlagsSampleFractionShift
 *
 * @constant AppleLosslessFormatFlag_16BitSourceData
 * This flag is set for Apple Lossless data that was sourced from 16 bit native
 * endian signed integer data.
 *
 * @constant AppleLosslessFormatFlag_20BitSourceData
 * This flag is set for Apple Lossless data that was sourced from 20 bit native
 * endian signed integer data aligned high in 24 bits.
 *
 * @constant AppleLosslessFormatFlag_24BitSourceData
 * This flag is set for Apple Lossless data that was sourced from 24 bit native
 * endian signed integer data.
 *
 * @constant AppleLosslessFormatFlag_32BitSourceData
 * This flag is set for Apple Lossless data that was sourced from 32 bit native
 * endian signed integer data.
 */
enum IOUserAudioFormatFlags : uint32_t
{
	FormatFlagIsFloat = (1U << 0),          // 0x1
	FormatFlagIsBigEndian = (1U << 1),      // 0x2
	FormatFlagIsSignedInteger = (1U << 2),  // 0x4
	FormatFlagIsPacked = (1U << 3),         // 0x8
	FormatFlagIsAlignedHigh = (1U << 4),    // 0x10
	FormatFlagIsNonInterleaved = (1U << 5), // 0x20
	FormatFlagIsNonMixable = (1U << 6),     // 0x40
	FormatFlagsAreAllClear = 0x80000000,

	LinearPCMFormatFlagIsFloat = FormatFlagIsFloat,
	LinearPCMFormatFlagIsBigEndian = FormatFlagIsBigEndian,
	LinearPCMFormatFlagIsSignedInteger = FormatFlagIsSignedInteger,
	LinearPCMFormatFlagIsPacked = FormatFlagIsPacked,
	LinearPCMFormatFlagIsAlignedHigh = FormatFlagIsAlignedHigh,
	LinearPCMFormatFlagIsNonInterleaved = FormatFlagIsNonInterleaved,
	LinearPCMFormatFlagIsNonMixable = FormatFlagIsNonMixable,
	LinearPCMFormatFlagsSampleFractionShift = 7,
	LinearPCMFormatFlagsSampleFractionMask =
	  (0x3F << LinearPCMFormatFlagsSampleFractionShift),
	LinearPCMFormatFlagsAreAllClear = FormatFlagsAreAllClear,

	AppleLosslessFormatFlag_16BitSourceData = 1,
	AppleLosslessFormatFlag_20BitSourceData = 2,
	AppleLosslessFormatFlag_24BitSourceData = 3,
	AppleLosslessFormatFlag_32BitSourceData = 4,

	FormatFlagsNativeEndian = 0,
	FormatFlagsNativeFloatPacked = FormatFlagIsFloat |
									FormatFlagsNativeEndian |
									FormatFlagIsPacked
};


/*!
 * @struct IOUserAudioStreamBasicDescription AudioStreamBasicDescription
 *
 * @abstract
 * This structure encapsulates all the information for describing the basic
 * format properties of a stream of audio data.
 *
 * @discussion
 * This structure is sufficient to describe any constant bit rate format that  has
 * channels that are the same size. Extensions are required for variable bit rate
 * data and for constant bit rate data where the channels have unequal sizes.
 * However, where applicable, the appropriate fields will be filled out correctly
 * for these kinds of formats (the extra data is provided via separate properties).
 * In all fields, a value of 0 indicates that the field is either unknown, not
 * applicable or otherwise is inapproprate for the format and should be ignored.
 * Note that 0 is still a valid value for most formats in the mFormatFlags field.
 *
 * In audio data a frame is one sample across all channels. In non-interleaved
 * audio, the per frame fields identify one channel. In interleaved audio, the per
 * frame fields identify the set of n channels. In uncompressed audio, a Packet is
 * one frame, (mFramesPerPacket == 1). In compressed audio, a Packet is an
 * indivisible chunk of compressed data, for example an AAC packet will contain
 * 1024 sample frames.
 *
 * @field mSampleRate
 * The number of sample frames per second of the data in the stream.
 *
 * @field mFormatID
 * The AudioFormatID indicating the general kind of data in the stream.
 *
 * @field mFormatFlags
 * The AudioFormatFlags for the format indicated by mFormatID.
 *
 * @field mBytesPerPacket
 * The number of bytes in a packet of data.
 *
 * @field mFramesPerPacket
 * The number of sample frames in each packet of data.
 *
 * @field mBytesPerFrame
 * The number of bytes in a single sample frame of data.
 *
 * @field mChannelsPerFrame
 * The number of channels in each frame of data.
 *
 * @field mBitsPerChannel
 * The number of bits of sample data for each channel in a frame of data.
 *
 * @field mReserved
 * Pads the structure out to force an even 8 byte alignment.
 */
struct IOUserAudioStreamBasicDescription
{
	double mSampleRate;
	IOUserAudioFormatID mFormatID;
	IOUserAudioFormatFlags mFormatFlags;
	uint32_t mBytesPerPacket;
	uint32_t mFramesPerPacket;
	uint32_t mBytesPerFrame;
	uint32_t mChannelsPerFrame;
	uint32_t mBitsPerChannel;
	uint32_t mReserved;
};


/*!
 *
 * @enum IOUserAudioChannelLabel Constants
 *
 * @abstract
 * These constants are to set the preferred channel layout on an IOUserAudioDevice
 *
 * @discussion
 * These channel labels attempt to list all labels in common use. Due to the
 * ambiguities in channel labeling by various groups, there may be some overlap or
 * duplication in the labels below. Use the label which most clearly describes what
 * you mean.
 */
enum class IOUserAudioChannelLabel : uint32_t
{
    Unknown                  = 0xFFFFFFFF,   ///< unknown or unspecified other use
    Unused                   = 0,            ///< channel is present, but has no intended use or destination
    UseCoordinates           = 100,          ///< channel is described by the mCoordinates fields.

    Left                     = 1,
    Right                    = 2,
    Center                   = 3,
    LFEScreen                = 4,
    LeftSurround             = 5,
    RightSurround            = 6,
    LeftCenter               = 7,
    RightCenter              = 8,
    CenterSurround           = 9,            ///< WAVE: "Back Center" or plain "Rear Surround"
    LeftSurroundDirect       = 10,
    RightSurroundDirect      = 11,
    TopCenterSurround        = 12,
    VerticalHeightLeft       = 13,           ///< WAVE: "Top Front Left"
    VerticalHeightCenter     = 14,           ///< WAVE: "Top Front Center"
    VerticalHeightRight      = 15,           ///< WAVE: "Top Front Right"

    TopBackLeft              = 16,
    TopBackCenter            = 17,
    TopBackRight             = 18,

    RearSurroundLeft         = 33,
    RearSurroundRight        = 34,
    LeftWide                 = 35,
    RightWide                = 36,
    LFE2                     = 37,
    LeftTotal                = 38,           ///< matrix encoded 4 channels
    RightTotal               = 39,           ///< matrix encoded 4 channels
    HearingImpaired          = 40,
    Narration                = 41,
    Mono                     = 42,
    DialogCentricMix         = 43,

    CenterSurroundDirect     = 44,           ///< back center, non diffuse
    
    Haptic                   = 45,
    
    LeftTopFront             = VerticalHeightLeft,
    CenterTopFront           = VerticalHeightCenter,
    RightTopFront            = VerticalHeightRight,
    LeftTopMiddle            = 49,
    CenterTopMiddle          = TopCenterSurround,
    RightTopMiddle           = 51,
    LeftTopRear              = 52,
    CenterTopRear            = 53,
    RightTopRear             = 54,
    
    // first order ambisonic channels
    Ambisonic_W              = 200,
    Ambisonic_X              = 201,
    Ambisonic_Y              = 202,
    Ambisonic_Z              = 203,

    // Mid/Side Recording
    MS_Mid                   = 204,
    MS_Side                  = 205,

    // X-Y Recording
    XY_X                     = 206,
    XY_Y                     = 207,
    
    // Binaural Recording
    BinauralLeft             = 208,
    BinauralRight            = 209,

    // other
    HeadphonesLeft           = 301,
    HeadphonesRight          = 302,
    ClickTrack               = 304,
    ForeignLanguage          = 305,

    // generic discrete channel
    Discrete                 = 400,

    // numbered discrete channel
    Discrete_0               = (1U<<16) | 0,
    Discrete_1               = (1U<<16) | 1,
    Discrete_2               = (1U<<16) | 2,
    Discrete_3               = (1U<<16) | 3,
    Discrete_4               = (1U<<16) | 4,
    Discrete_5               = (1U<<16) | 5,
    Discrete_6               = (1U<<16) | 6,
    Discrete_7               = (1U<<16) | 7,
    Discrete_8               = (1U<<16) | 8,
    Discrete_9               = (1U<<16) | 9,
    Discrete_10              = (1U<<16) | 10,
    Discrete_11              = (1U<<16) | 11,
    Discrete_12              = (1U<<16) | 12,
    Discrete_13              = (1U<<16) | 13,
    Discrete_14              = (1U<<16) | 14,
    Discrete_15              = (1U<<16) | 15,
    Discrete_65535           = (1U<<16) | 65535,
    
    // generic HOA ACN channel
    HOA_ACN                  = 500,
    
    // numbered HOA ACN channels
    HOA_ACN_0                = (2U << 16) | 0,
    HOA_ACN_1                = (2U << 16) | 1,
    HOA_ACN_2                = (2U << 16) | 2,
    HOA_ACN_3                = (2U << 16) | 3,
    HOA_ACN_4                = (2U << 16) | 4,
    HOA_ACN_5                = (2U << 16) | 5,
    HOA_ACN_6                = (2U << 16) | 6,
    HOA_ACN_7                = (2U << 16) | 7,
    HOA_ACN_8                = (2U << 16) | 8,
    HOA_ACN_9                = (2U << 16) | 9,
    HOA_ACN_10               = (2U << 16) | 10,
    HOA_ACN_11               = (2U << 16) | 11,
    HOA_ACN_12               = (2U << 16) | 12,
    HOA_ACN_13               = (2U << 16) | 13,
    HOA_ACN_14               = (2U << 16) | 14,
    HOA_ACN_15               = (2U << 16) | 15,
    HOA_ACN_65024            = (2U << 16) | 65024,    // 254th order uses 65025 channels
    
    BeginReserved            = 0xF0000000,           // Channel label values in this range are reserved for internal use
    EndReserved              = 0xFFFFFFFE
};


//==================================================================================================
#pragma mark -
#pragma mark IO Operations

/*!
 * @typedef IOUserAudioIOOperation
 *
 * @abstract
 * A uint32_t that specifies the IO operation that is called on the IOOperationHandler block
 */
typedef uint32_t IOUserAudioIOOperation;

/*!
 * @constant
 * IOUserAudioIOOperationBeginRead
 *
 * @discussion
 * This operation is called just prior to reading data from the device's stream buffers.
 * It is required that this operation is handled if the device has input streams.
 */
constexpr IOUserAudioIOOperation IOUserAudioIOOperationBeginRead = 0;

/*!
 * @constant
 * IOUserAudioIOOperationWriteEnd
 *
 * @discussion
 * This operation is called just after writing data to the device's stream buffers.
 * It is required that this operation be handled if the device has output streams.
 */
constexpr IOUserAudioIOOperation IOUserAudioIOOperationWriteEnd = 1;

/*!
 * @typedef IOOperationHandler
 *
 * @discussion
 * A  block that tells the device to perform an IOUserAudioIOOperation.
 * See IOUserAudioDevice::SetIOOperationHandler
 *
 * @param in_device
 * The IOUserAudioObjectID of the device that is performing the IO operation
 *
 * @param in_io_operation
 * The IOUserAudioIOOperation that is being performed
 *
 * @param in_io_buffer_frame_size
 * uint32_t that specifies the number of sample frames that will be processed
 * in the IO operation. Note that for some operations, this will be different than
 * the nominal buffer frame size
 *
 * @param in_sample_time
 * uint64_t sample time that indicates position in the device's timeline the
 * data for the IO Operation occurs.
 *
 * @return
 * Returns kern_return_t
 */
typedef kern_return_t (^IOOperationHandler) (IOUserAudioObjectID in_device,
											 IOUserAudioIOOperation in_io_operation,
											 uint32_t in_io_buffer_frame_size,
											 uint64_t in_sample_time,
											 uint64_t in_host_time);

} // namespace AudioDriverKit

#endif /* AudioDriverKitTypes_h */
