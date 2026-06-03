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

#ifndef IOUserAudioUtils_h
#define IOUserAudioUtils_h

#include <os/log.h>
#include <cstdio>

#define	DebugMsg(inFormat, args...)	os_log(OS_LOG_DEFAULT, "%s: " inFormat "\n", __FUNCTION__, ##args)

#define	FailIf(inCondition, inAction, inHandler, inMessage)									\
			{																				\
				bool __failed = (inCondition);												\
				if(__failed)																\
				{																			\
					DebugMsg(inMessage);													\
					{ inAction; }															\
					goto inHandler;															\
				}																			\
			}

#define	FailIfError(inError, inAction, inHandler, inMessage)								\
			{																				\
				IOReturn __Err = (inError);													\
				if(__Err != 0)																\
				{																			\
					DebugMsg(inMessage ", Error: %d (0x%X)", __Err, (unsigned int)__Err);	\
					{ inAction; }															\
					goto inHandler;															\
				}																			\
			}

#define	FailIfNULL(inPointer, inAction, inHandler, inMessage)								\
			if((inPointer) == NULL)															\
			{																				\
				DebugMsg(inMessage);														\
				{ inAction; }																\
				goto inHandler;																\
			}

#endif /* IOUserAudioUtils_h */
