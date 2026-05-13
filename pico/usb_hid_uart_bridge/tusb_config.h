#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

// Common
#define CFG_TUSB_MCU                OPT_MCU_RP2040
#define CFG_TUSB_OS                 OPT_OS_PICO
#define CFG_TUSB_DEBUG              0

// RHPort 0 runs in host mode at full speed.
#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_RHPORT1_MODE       (OPT_MODE_NONE)

// Host configuration
#define CFG_TUH_ENABLED             1
#define CFG_TUH_DEVICE_MAX          4
#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HUB                 1
#define CFG_TUH_HID                 4

#endif // _TUSB_CONFIG_H_
