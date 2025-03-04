/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Ha Thach (tinyusb.org) for Adafruit Industries
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

//--------------------------------------------------------------------+
// Button
//--------------------------------------------------------------------+

// Enter UF2 mode if GPIO is pressed while 2nd stage bootloader indicator
// is on e.g RGB = Purple. If it is GPIO0, user should not hold this while
// reset since that will instead run the 1st stage ROM bootloader
#define PIN_BUTTON_UF2        0

// GPIO that implement 1-bit memory with RC components which hold the
// pin value long enough for double reset detection.
// #define PIN_DOUBLE_RESET_RC   34

// GPIO to enter DFU OTA mode on reset
// should be LOW to enter DFU mode
 #define PIN_DFU_TRIGGER       34

//--------------------------------------------------------------------+
// LED
//--------------------------------------------------------------------+

// LED for indicator and writing flash
#define LED_PIN               15
#define LED_STATE_ON          1

// Uncomment & wire-up NeoPixel to board for testing
// // GPIOs connected to NeoPixel data & power
// #define NEOPIXEL_PIN          01
//
// #define NEOPIXEL_POWER_PIN    02
// #define NEOPIXEL_POWER_STATE  1
//
// // Brightness percentage from 1 to 255
// #define NEOPIXEL_BRIGHTNESS   0x64
//
// // Number of neopixels
// #define NEOPIXEL_NUMBER       1

//--------------------------------------------------------------------+
// USB UF2
//--------------------------------------------------------------------+

// // TinyUFT vid/pid resevered for Enerty
// #define USB_VID           0x303A
// #define USB_PID           0x80DA
// #define USB_MANUFACTURER  "ENERTY"
// #define USB_PRODUCT       "Module M"

// USB to RS485 converter VID and PID
#define USB_VID           0x0403
#define USB_PID           0x80DA
#define USB_MANUFACTURER  "FTDI"
#define USB_PRODUCT       "USB-RS485 Cable"

#define UF2_PRODUCT_NAME  USB_MANUFACTURER " " USB_PRODUCT
#define UF2_BOARD_ID      "ESP32S2FN4R2-ModuleM-1-0-0"
#define UF2_VOLUME_LABEL  "ENERTYMBOOT"
#define UF2_INDEX_URL     "https://www.google.com/search?q=ENERTY+module+m"
