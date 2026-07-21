/*
  CH552T Corsair Commander Pro compatible lower-board firmware for CH55xDuino.

  SPDX-License-Identifier: MIT
  Copyright (c) 2026 seven

  First target: make Windows enumerate as Corsair Commander Pro compatible HID:
    VID:PID = 1B1C:0C10
    HID OUT report: 64 bytes, host writes report-id 0 + command frame
    HID IN  report: 16 bytes, data fields at response offsets 1..n

  Arduino IDE / CH55xDuino notes:
    - Select your CH552/CH552T board.
    - USB setting must be a USER USB RAM option, not the stock built-in USB mode.
      CH55xDuino examples call this e.g. "USER USB setting" / user148/user266.
    - This file is HID-only.

  Pin plan used here, matching CH55xDuino decimal pin naming convention:
    P3.4 = 34 -> Fan1 PWM / hardware PWM2
    P1.5 = 15 -> Fan2 PWM / hardware PWM1
    P3.2 = 32 -> Fan1 tach / INT0-style input
    P3.3 = 33 -> Fan2 tach / INT1-style input

  Hardware protection remains required:
    PWM:  CH552 pin -> 270R -> fan PWM, 1N4148 clamp to +5V, optional 10k pulldown
    Tach: fan tach -> 10k -> CH552 input, 1N4148 clamp to +5V, 100k pulldown,
          optional 10k pull-up to +5V if fan tach is open collector without pull-up
*/

#ifndef USER_USB_RAM
#error "In Arduino IDE select a CH55xDuino USER USB RAM setting, not the stock USB mode."
#endif

#include <Arduino.h>
#include <stdint.h>
#include <stdbool.h>
#include "include/ch5xx.h"
#include "include/ch5xx_usb.h"

// Fallbacks for CH55xDuino versions where these request constants are only
// pulled in by the bundled USB examples.
#ifndef DEFAULT_ENDP0_SIZE
#define DEFAULT_ENDP0_SIZE 8
#endif
#ifndef USB_REQ_TYP_MASK
#define USB_REQ_TYP_MASK 0x60
#endif
#ifndef USB_REQ_TYP_STANDARD
#define USB_REQ_TYP_STANDARD 0x00
#endif
#ifndef USB_REQ_TYP_CLASS
#define USB_REQ_TYP_CLASS 0x20
#endif
#ifndef USB_GET_STATUS
#define USB_GET_STATUS 0x00
#endif
#ifndef USB_GET_DESCRIPTOR
#define USB_GET_DESCRIPTOR 0x06
#endif
#ifndef USB_SET_ADDRESS
#define USB_SET_ADDRESS 0x05
#endif
#ifndef USB_GET_CONFIGURATION
#define USB_GET_CONFIGURATION 0x08
#endif
#ifndef USB_SET_CONFIGURATION
#define USB_SET_CONFIGURATION 0x09
#endif
#ifndef USB_GET_INTERFACE
#define USB_GET_INTERFACE 0x0A
#endif
#ifndef HID_GET_REPORT
#define HID_GET_REPORT 0x01
#endif
#ifndef HID_SET_IDLE
#define HID_SET_IDLE 0x0A
#endif
#ifndef bUDA_GP_BIT
#define bUDA_GP_BIT 0x80
#endif

#define USB_VID 0x1B1C
#define USB_PID 0x0C10

// Endpoint buffer placement inside CH55xDuino USER USB RAM.
// Keep these ranges non-overlapping: EP0 uses 8 bytes, EP1 IN uses 16 bytes,
// and EP2 OUT uses 64 bytes.
#define EP0_ADDR 0
#define EP1_IN_ADDR 16
#define EP2_OUT_ADDR 96

#define FAN_COUNT_PHYSICAL 2
#define FAN_COUNT_LOGICAL 6
#define TACH_PULSES_PER_REV 2UL

#define FAN1_PWM_PIN 34  // P3.4, CH55xDuino hardware PWM2
#define FAN2_PWM_PIN 15  // P1.5, CH55xDuino hardware PWM1
#define FAN1_TACH_PIN 32 // P3.2
#define FAN2_TACH_PIN 33 // P3.3

#define CPRO_CMD_GET_FIRMWARE    0x02
#define CPRO_CMD_GET_BOOTLOADER  0x06
#define CPRO_CMD_GET_TEMP_CONFIG 0x10
#define CPRO_CMD_GET_TEMP        0x11
#define CPRO_CMD_GET_VOLTS       0x12
#define CPRO_CMD_GET_FAN_MODES   0x20
#define CPRO_CMD_GET_FAN_RPM     0x21
#define CPRO_CMD_GET_FAN_PWM     0x22
#define CPRO_CMD_SET_FAN_DUTY    0x23
#define CPRO_CMD_SET_FAN_TARGET  0x24
#define CPRO_CMD_SET_FAN_PROFILE 0x25
#define CPRO_CMD_SET_FAN_MODE    0x28

#define CPRO_FAN_MODE_DISCONNECTED 0x00
#define CPRO_FAN_MODE_DC           0x01
#define CPRO_FAN_MODE_PWM          0x02

// CH55xDuino USB buffers. Endpoint DMA addresses must be in USER USB RAM.
__xdata __at(EP0_ADDR)     uint8_t Ep0Buffer[8];
__xdata __at(EP1_IN_ADDR)  uint8_t Ep1InBuffer[16];
__xdata __at(EP2_OUT_ADDR) uint8_t Ep2OutBuffer[64];

__data uint16_t SetupLen;
__data uint8_t SetupReq;
__code uint8_t *__data pDescr;
volatile __xdata uint8_t UsbConfig = 0;
volatile __bit Ep1InBusy = 0;

// Tach pulse counters are updated by external interrupt callbacks and consumed
// once per second in loop().  Most PC fans produce two tach pulses per turn.
volatile uint16_t tachPulses[2] = {0, 0};
uint16_t fanRpm[2] = {0, 0};
uint8_t fanDuty[2] = {50, 50};
uint8_t fanMode[2] = {CPRO_FAN_MODE_PWM, CPRO_FAN_MODE_PWM};
unsigned long lastRpmMs = 0;

// Device descriptor: Corsair Commander Pro identity.
__code uint8_t DeviceDescriptor[] = {
  18, 0x01,
  0x10, 0x01,
  0x00, 0x00, 0x00,
  8,
  (uint8_t)(USB_VID & 0xff), (uint8_t)(USB_VID >> 8),
  (uint8_t)(USB_PID & 0xff), (uint8_t)(USB_PID >> 8),
  0x00, 0x01,
  1, 2, 3,
  1
};

// Vendor HID report descriptor: 64-byte OUT, 16-byte IN, no report ID.
__code uint8_t HidReportDescriptor[] = {
  0x06, 0x00, 0xFF,       // Usage Page (Vendor Defined)
  0x09, 0x01,             // Usage
  0xA1, 0x01,             // Collection Application
  0x15, 0x00,             // Logical Min 0
  0x26, 0xFF, 0x00,       // Logical Max 255
  0x75, 0x08,             // Report Size 8
  0x95, 0x40,             // Report Count 64
  0x09, 0x01,
  0x91, 0x02,             // Output Data Var Abs
  0x95, 0x10,             // Report Count 16
  0x09, 0x01,
  0x81, 0x02,             // Input Data Var Abs
  0xC0
};

// One-interface HID config: EP1 IN 16 bytes, EP2 OUT 64 bytes.
// Total length is 9(config)+9(interface)+9(HID)+7(IN ep)+7(OUT ep) = 41.
// A previous 34-byte total length made Windows read only the IN endpoint while
// bNumEndpoints still said 2, causing HIDClass Code 10 / failed start.
__code uint8_t ConfigurationDescriptor[] = {
  9, 0x02, 41, 0x00, 1, 1, 0, 0x80, 50,
  9, 0x04, 0, 0, 2, 0x03, 0x00, 0x00, 0,
  9, 0x21, 0x11, 0x01, 0x00, 1, 0x22, sizeof(HidReportDescriptor), 0,
  7, 0x05, 0x81, 0x03, 16, 0, 1,
  7, 0x05, 0x02, 0x03, 64, 0, 1
};

__code uint8_t LanguageDescriptor[] = {4, 0x03, 0x09, 0x04};
__code uint16_t ManufacturerDescriptor[] = {
  ((7 + 1) * 2) | (0x03 << 8), 'C','o','r','s','a','i','r'
};
__code uint16_t ProductDescriptor[] = {
  ((13 + 1) * 2) | (0x03 << 8), 'C','o','m','m','a','n','d','e','r',' ','P','r','o'
};
__code uint16_t SerialDescriptor[] = {
  ((5 + 1) * 2) | (0x03 << 8), 'C','H','5','5','2'
};

static void put_be16(uint8_t *p, uint16_t value) {
  // Commander Pro protocol returns multi-byte numeric fields big-endian.
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)(value & 0xff);
}

static void setFanDuty(uint8_t fan, uint8_t duty) {
  // Host commands use percent 0..100; CH55xDuino analogWrite uses 0..255.
  if (fan >= FAN_COUNT_PHYSICAL) return;
  if (duty > 100) duty = 100;
  fanDuty[fan] = duty;
  uint8_t pwm = (uint8_t)(((uint16_t)duty * 255U) / 100U);
  analogWrite(fan == 0 ? FAN1_PWM_PIN : FAN2_PWM_PIN, pwm);
}

static uint8_t getFanMode(uint8_t fan) {
  if (fan >= FAN_COUNT_PHYSICAL) return CPRO_FAN_MODE_DISCONNECTED;
  return fanMode[fan];
}

static uint16_t getFanRpm(uint8_t fan) {
  if (fan >= FAN_COUNT_PHYSICAL) return 0;
  return fanRpm[fan];
}

static void handleCommanderFrame(__xdata uint8_t *outFrame) {
  // EP2 OUT receives a 64-byte command frame.  EP1 IN returns a 16-byte
  // response frame.  Response byte 0 must be a status code, not a command echo,
  // otherwise Linux corsair-cpro treats command 0x10 as -EINVAL during probe.
  uint8_t cmd = outFrame[0];
  uint8_t *resp = Ep1InBuffer;
  memset(resp, 0, 16);
  // Real Commander Pro responses use byte 0 as status/error code:
  //   0x00 success, 0x01 invalid command, 0x10 invalid argument,
  //   0x11 disconnected temp, 0x12 PWM not fixed-duty controlled.
  // Linux corsair-cpro checks this byte during probe; echoing the command
  // makes command 0x10 look like -EINVAL and causes probe error -22.
  resp[0] = 0x00;

  switch (cmd) {
    case CPRO_CMD_GET_FIRMWARE:
      resp[1] = 1; resp[2] = 0; resp[3] = 0;
      break;
    case CPRO_CMD_GET_BOOTLOADER:
      resp[1] = 0; resp[2] = 1;
      break;
    case CPRO_CMD_GET_TEMP_CONFIG:
      resp[1] = resp[2] = resp[3] = resp[4] = 0;
      break;
    case CPRO_CMD_GET_TEMP:
      put_be16(&resp[1], 2500); // 25.00 C dummy
      break;
    case CPRO_CMD_GET_VOLTS:
      if (outFrame[1] == 0) put_be16(&resp[1], 12000);
      else if (outFrame[1] == 1) put_be16(&resp[1], 5000);
      else put_be16(&resp[1], 3300);
      break;
    case CPRO_CMD_GET_FAN_MODES:
      for (uint8_t i = 0; i < FAN_COUNT_LOGICAL; i++) resp[1 + i] = getFanMode(i);
      break;
    case CPRO_CMD_GET_FAN_RPM:
      put_be16(&resp[1], getFanRpm(outFrame[1]));
      break;
    case CPRO_CMD_GET_FAN_PWM:
      if (outFrame[1] < FAN_COUNT_PHYSICAL) resp[1] = fanDuty[outFrame[1]];
      else resp[0] = 0x12;
      break;
    case CPRO_CMD_SET_FAN_DUTY:
      setFanDuty(outFrame[1], outFrame[2]);
      break;
    case CPRO_CMD_SET_FAN_TARGET:
      // Linux hwmon may expose fan*_target. Accept target-RPM writes for
      // compatibility, but keep the simple fixed-duty control loop for now.
      break;
    case CPRO_CMD_SET_FAN_PROFILE:
      // Accept for compatibility; no temperature/RPM closed-loop yet.
      break;
    case CPRO_CMD_SET_FAN_MODE:
      if (outFrame[2] < FAN_COUNT_PHYSICAL) {
        fanMode[outFrame[2]] = outFrame[3];
        if (fanMode[outFrame[2]] == CPRO_FAN_MODE_DISCONNECTED) setFanDuty(outFrame[2], 0);
      }
      break;
    default:
      resp[0] = 0x01;
      break;
  }

  UEP1_T_LEN = 16;
  Ep1InBusy = 1;
  UEP1_CTRL = (UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
}

#pragma save
#pragma nooverlay
void tach1ISR() { tachPulses[0]++; }
#pragma restore

#pragma save
#pragma nooverlay
void tach2ISR() { tachPulses[1]++; }
#pragma restore

static void updateRpm() {
  // Convert the number of falling tach edges seen during the last second into
  // RPM.  This intentionally avoids floating point to keep SDCC output small.
  unsigned long now = millis();
  if (now - lastRpmMs < 1000) return;
  lastRpmMs = now;
  noInterrupts();
  uint16_t p0 = tachPulses[0]; tachPulses[0] = 0;
  uint16_t p1 = tachPulses[1]; tachPulses[1] = 0;
  interrupts();
  fanRpm[0] = (uint16_t)((uint32_t)p0 * 60UL / TACH_PULSES_PER_REV);
  fanRpm[1] = (uint16_t)((uint32_t)p1 * 60UL / TACH_PULSES_PER_REV);
}

static void USB_EP0_SETUP() {
  // EP0 SETUP handles standard USB enumeration requests plus the minimal HID
  // class requests Windows/Linux send before starting the interrupt endpoints.
  uint8_t len = USB_RX_LEN;
  if (len != 8) {
    UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;
    return;
  }

  SetupLen = ((uint16_t)Ep0Buffer[7] << 8) | Ep0Buffer[6];
  SetupReq = Ep0Buffer[1];
  len = 0;

  if ((Ep0Buffer[0] & USB_REQ_TYP_MASK) == USB_REQ_TYP_STANDARD) {
    switch (SetupReq) {
      case USB_GET_DESCRIPTOR:
        switch (Ep0Buffer[3]) {
          case 1: pDescr = DeviceDescriptor; len = sizeof(DeviceDescriptor); break;
          case 2: pDescr = ConfigurationDescriptor; len = sizeof(ConfigurationDescriptor); break;
          case 3:
            if (Ep0Buffer[2] == 0) { pDescr = LanguageDescriptor; len = sizeof(LanguageDescriptor); }
            else if (Ep0Buffer[2] == 1) { pDescr = (__code uint8_t *)ManufacturerDescriptor; len = ((uint8_t *)ManufacturerDescriptor)[0]; }
            else if (Ep0Buffer[2] == 2) { pDescr = (__code uint8_t *)ProductDescriptor; len = ((uint8_t *)ProductDescriptor)[0]; }
            else if (Ep0Buffer[2] == 3) { pDescr = (__code uint8_t *)SerialDescriptor; len = ((uint8_t *)SerialDescriptor)[0]; }
            else len = 0xff;
            break;
          case 0x22: pDescr = HidReportDescriptor; len = sizeof(HidReportDescriptor); break;
          default: len = 0xff; break;
        }
        if (len != 0xff) {
          if (SetupLen > len) SetupLen = len;
          len = (SetupLen >= DEFAULT_ENDP0_SIZE) ? DEFAULT_ENDP0_SIZE : SetupLen;
          for (uint8_t i = 0; i < len; i++) Ep0Buffer[i] = pDescr[i];
          SetupLen -= len;
          pDescr += len;
        }
        break;
      case USB_SET_ADDRESS:
        SetupLen = Ep0Buffer[2];
        break;
      case USB_SET_CONFIGURATION:
        UsbConfig = Ep0Buffer[2];
        break;
      case USB_GET_CONFIGURATION:
        Ep0Buffer[0] = UsbConfig;
        len = 1;
        break;
      case USB_GET_STATUS:
        Ep0Buffer[0] = 0; Ep0Buffer[1] = 0; len = 2;
        break;
      case USB_GET_INTERFACE:
        Ep0Buffer[0] = 0; len = 1;
        break;
      default:
        len = 0xff;
        break;
    }
  } else if ((Ep0Buffer[0] & USB_REQ_TYP_MASK) == USB_REQ_TYP_CLASS) {
    // HID class: SET_IDLE and GET_REPORT are enough for this vendor HID bring-up.
    if (SetupReq == HID_SET_IDLE) {
      len = 0;
    } else if (SetupReq == HID_GET_REPORT) {
      memset(Ep0Buffer, 0, DEFAULT_ENDP0_SIZE);
      len = DEFAULT_ENDP0_SIZE;
    } else {
      len = 0xff;
    }
  } else {
    len = 0xff;
  }

  if (len == 0xff) {
    SetupReq = 0xff;
    UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;
  } else {
    UEP0_T_LEN = len;
    UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
  }
}

static void USB_EP0_IN() {
  if (SetupReq == USB_GET_DESCRIPTOR) {
    uint8_t len = (SetupLen >= DEFAULT_ENDP0_SIZE) ? DEFAULT_ENDP0_SIZE : SetupLen;
    for (uint8_t i = 0; i < len; i++) Ep0Buffer[i] = pDescr[i];
    SetupLen -= len;
    pDescr += len;
    UEP0_T_LEN = len;
    UEP0_CTRL ^= bUEP_T_TOG;
  } else if (SetupReq == USB_SET_ADDRESS) {
    USB_DEV_AD = (USB_DEV_AD & bUDA_GP_BIT) | SetupLen;
    UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
  } else {
    UEP0_T_LEN = 0;
    UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
  }
}

static void USB_EP0_OUT() {
  UEP0_T_LEN = 0;
  UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
}

static void USB_EP1_IN() {
  // Host has consumed the last 16-byte response.  NAK further IN tokens until
  // the next command prepares a fresh response.
  UEP1_T_LEN = 0;
  Ep1InBusy = 0;
  UEP1_CTRL = (UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
}

static void USB_EP2_OUT() {
  // Host sent a Commander Pro command frame on the interrupt OUT endpoint.
  if (U_TOG_OK) {
    handleCommanderFrame(Ep2OutBuffer);
  }
  UEP2_CTRL = (UEP2_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_ACK;
}

#pragma save
#pragma nooverlay
void USBInterrupt(void) {
  // CH55xDuino installs this USB ISR when USER USB RAM mode is selected.  The
  // ISR dispatches token events to EP0/EP1/EP2 handlers and resets endpoint
  // state after a USB bus reset.
  if (UIF_TRANSFER) {
    uint8_t ep = USB_INT_ST & MASK_UIS_ENDP;
    uint8_t tok = USB_INT_ST & MASK_UIS_TOKEN;
    if (ep == 0 && tok == UIS_TOKEN_SETUP) USB_EP0_SETUP();
    else if (ep == 0 && tok == UIS_TOKEN_IN) USB_EP0_IN();
    else if (ep == 0 && tok == UIS_TOKEN_OUT) USB_EP0_OUT();
    else if (ep == 1 && tok == UIS_TOKEN_IN) USB_EP1_IN();
    else if (ep == 2 && tok == UIS_TOKEN_OUT) USB_EP2_OUT();
    UIF_TRANSFER = 0;
  }

  if (UIF_BUS_RST) {
    UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
    UEP1_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK;
    UEP2_CTRL = bUEP_AUTO_TOG | UEP_R_RES_ACK;
    USB_DEV_AD = 0;
    UIF_SUSPEND = 0;
    UIF_TRANSFER = 0;
    UIF_BUS_RST = 0;
    UsbConfig = 0;
  }

  if (UIF_SUSPEND) {
    UIF_SUSPEND = 0;
  }
}
#pragma restore

static void USBDeviceCfgLocal() {
  // Device mode, full speed, internal D+ pull-up enabled.
  USB_CTRL = 0x00;
  USB_CTRL &= ~bUC_HOST_MODE;
  USB_CTRL |= bUC_DEV_PU_EN | bUC_INT_BUSY | bUC_DMA_EN;
  USB_DEV_AD = 0x00;
  USB_CTRL &= ~bUC_LOW_SPEED;
  UDEV_CTRL &= ~bUD_LOW_SPEED;
#if defined(CH551) || defined(CH552) || defined(CH549)
  UDEV_CTRL = bUD_PD_DIS;
#endif
  UDEV_CTRL |= bUD_PORT_EN;
}

static void USBDeviceEndPointCfgLocal() {
  // Bind SIE DMA pointers to our USB RAM buffers and enable EP1 IN + EP2 OUT.
#if defined(CH559)
  UEP0_DMA_H = ((uint16_t)Ep0Buffer >> 8);
  UEP0_DMA_L = ((uint16_t)Ep0Buffer >> 0);
  UEP1_DMA_H = ((uint16_t)Ep1InBuffer >> 8);
  UEP1_DMA_L = ((uint16_t)Ep1InBuffer >> 0);
  UEP2_DMA_H = ((uint16_t)Ep2OutBuffer >> 8);
  UEP2_DMA_L = ((uint16_t)Ep2OutBuffer >> 0);
#else
  UEP0_DMA = (uint16_t)Ep0Buffer;
  UEP1_DMA = (uint16_t)Ep1InBuffer;
  UEP2_DMA = (uint16_t)Ep2OutBuffer;
#endif
  UEP4_1_MOD = bUEP1_TX_EN;
  UEP2_3_MOD = bUEP2_RX_EN;
  UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
  UEP1_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK;
  UEP2_CTRL = bUEP_AUTO_TOG | UEP_R_RES_ACK;
}

static void USBDeviceIntCfgLocal() {
  // Enable USB transfer/reset/suspend interrupts and global interrupts.
  USB_INT_EN |= bUIE_SUSPEND;
  USB_INT_EN |= bUIE_TRANSFER;
  USB_INT_EN |= bUIE_BUS_RST;
  USB_INT_FG |= 0x1F;
  IE_USB = 1;
  EA = 1;
}

void USBInitLocal() {
  USBDeviceCfgLocal();
  USBDeviceEndPointCfgLocal();
  USBDeviceIntCfgLocal();
}

void setup() {
  // Start fans at a safe 50% duty, configure tach inputs, then attach USB.
  pinMode(FAN1_PWM_PIN, OUTPUT);
  pinMode(FAN2_PWM_PIN, OUTPUT);
  pinMode(FAN1_TACH_PIN, INPUT_PULLUP);
  pinMode(FAN2_TACH_PIN, INPUT_PULLUP);
  setFanDuty(0, 50);
  setFanDuty(1, 50);
  attachInterrupt(0, tach1ISR, FALLING); // INT0 / P3.2
  attachInterrupt(1, tach2ISR, FALLING); // INT1 / P3.3
  USBInitLocal();
}

void loop() {
  // USB work is interrupt-driven; the main loop only maintains RPM counters.
  updateRpm();
}
