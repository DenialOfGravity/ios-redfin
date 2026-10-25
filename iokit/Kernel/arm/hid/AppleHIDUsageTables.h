/*
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
#ifndef _APPLEHIDUSAGETABLES_H
#define _APPLEHIDUSAGETABLES_H

/*
 * SWEET (redfin 0971): IOHIDFamily-1090 and later (including 1915.140.3, the version the rest of this
 * directory comes from) publish this header empty. These are the Apple vendor pages and usages the sensor
 * services need, copied from the last release that published them, IOHIDFamily-1035.70.7
 * (github.com/apple-oss-distributions/IOHIDFamily, tag IOHIDFamily-1035.70.7, IOHIDFamily/AppleHIDUsageTables.h).
 * kHIDUsage_AppleVendorMotion_DeviceOrientationType* (used by IOHIDEventDriver-1915) are not in it: their
 * values are unpublished.
 */

/* Usage Pages */
enum {
    kHIDPage_AppleVendor                        = 0xFF00,
    kHIDPage_AppleVendorKeyboard                = 0xFF01,
    kHIDPage_AppleVendorMouse                   = 0xFF02,
    kHIDPage_AppleVendorAccelerometer           = 0xFF03,
    kHIDPage_AppleVendorAmbientLightSensor      = 0xFF04,
    kHIDPage_AppleVendorTemperatureSensor       = 0xFF05,
    kHIDPage_AppleVendorHeadset                 = 0xFF07,
    kHIDPage_AppleVendorPowerSensor             = 0xFF08,
    kHIDPage_AppleVendorSmartCover              = 0xFF09,
    kHIDPage_AppleVendorPlatinum                = 0xFF0A,
    kHIDPage_AppleVendorLisa                    = 0xFF0B,
    kHIDPage_AppleVendorMotion                  = 0xFF0C,
    kHIDPage_AppleVendorBattery                 = 0xFF0D,
    kHIDPage_AppleVendorIRRemote                = 0xFF0E,
    kHIDPage_AppleVendorDebug                   = 0xFF0F,
    kHIDPage_AppleVendorIRInterface             = 0xFF10,
    kHIDPage_AppleVendorDFR                     = 0xFF11,
    kHIDPage_AppleVendorDFRBrightness           = 0xFF12,
    kHIDPage_AppleVendorSenderID                = 0xFF13,
    kHIDPage_AppleVendorFilteredEvent           = 0xFF50,
    kHIDPage_AppleVendorMultitouch              = 0xFF60,
    kHIDPage_AppleVendorDisplay                 = 0xFF92,
    kHIDPage_AppleVendorTopCase                 = 0x00FF
};

/* AppleVendor Page (0xff00) */
enum {
    kHIDUsage_AppleVendor_TopCase               = 0x0001, /* Application Collection */
    kHIDUsage_AppleVendor_Display               = 0x0002, /* Application Collection */
    kHIDUsage_AppleVendor_Accelerometer         = 0x0003, /* Application Collection */
    kHIDUsage_AppleVendor_AmbientLightSensor    = 0x0004, /* Application Collection */
    kHIDUsage_AppleVendor_TemperatureSensor     = 0x0005, /* Application Collection */
    kHIDUsage_AppleVendor_Keyboard              = 0x0006, /* Application Collection */
    kHIDUsage_AppleVendor_Headset               = 0x0007, /* Application Collection */
    kHIDUsage_AppleVendor_ProximitySensor       = 0x0008, /* Application Collection */
    kHIDUsage_AppleVendor_Gyro                  = 0x0009, /* Application Collection */
    kHIDUsage_AppleVendor_Compass               = 0x000A, /* Application Collection */
    kHIDUsage_AppleVendor_DeviceManagement      = 0x000B, /* Application Collection */
    kHIDUsage_AppleVendor_Trackpad              = 0x000C, /* Application Collection */
    kHIDUsage_AppleVendor_TopCaseReserved       = 0x000D, /* Application Collection */
    kHIDUsage_AppleVendor_Motion                = 0x000E, /* Application Collection */
    kHIDUsage_AppleVendor_KeyboardBacklight     = 0x000F, /* Application Collection */
    kHIDUsage_AppleVendor_DeviceMotionLite      = 0x0010, /* Application Collection */
    kHIDUsage_AppleVendor_Force                 = 0x0011, /* Application Collection */
    kHIDUsage_AppleVendor_BluetoothRadio        = 0x0012, /* Application Collection */
    kHIDUsage_AppleVendor_Orb                   = 0x0013, /* Application Collection */
    kHIDUsage_AppleVendor_AccessoryBattery      = 0x0014, /* Application Collection */
    kHIDUsage_AppleVendor_Humidity              = 0x0015, /* Application Collection */
    kHIDUsage_AppleVendor_DFR                   = 0x0016, /* Application Collection */
    kHIDUsage_AppleVendor_NXEvent               = 0x0017, /* Application Collection */
    kHIDUsage_AppleVendor_NXEvent_Translated    = 0x0018, /* Application Collection */
    kHIDUsage_AppleVendor_NXEvent_Diagnostic    = 0x0019, /* Application Collection */
    kHIDUsage_AppleVendor_Homer                 = 0x0020, /* Application Collection */
    kHIDUsage_AppleVendor_Color                 = 0x0021, /* Dynamic Value */
    kHIDUsage_AppleVendor_Accessibility         = 0x0022, /* Application Collection */
    kHIDUsage_AppleVendor_Message               = 0x0023, /* Application Collection */
    kHIDUsage_AppleVendor_Luna                  = 0x0024, /* Application Collection */
    kHIDUsage_AppleVendor_Payload               = 0x0025, /* Application Collection */
    kHIDUsage_AppleVendor_Perf                  = 0x0026, /* Application Collection */
    kHIDUsage_AppleVendor_Gallium               = 0x0027, /* Application Collection */
    kHIDUsage_AppleVendor_HIDRelay              = 0x0028, /* Application Collection */
    kHIDUsage_AppleVendor_SmartCover            = 0x0029  /* Application Collection */
};

/* AppleVendor Motion (0xff0C) */
enum {
    kHIDUsage_AppleVendorMotion_Motion          = 0x0001,
    kHIDUsage_AppleVendorMotion_Activity        = 0x0002,
    kHIDUsage_AppleVendorMotion_Gesture         = 0x0003,
    kHIDUsage_AppleVendorMotion_DeviceMotion    = 0x0004,
    kHIDUsage_AppleVendorMotion_DeviceMotion6   = 0x0005,
    kHIDUsage_AppleVendorMotion_Orientation     = 0x0006,
    kHIDUsage_AppleVendorMotion_DeviceMotion3   = 0x0007,
    kHIDUsage_AppleVendorMotion_DeviceMotion10  = 0x0008
};

#endif /* _APPLEHIDUSAGETABLES_H */
