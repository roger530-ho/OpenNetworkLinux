/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MFD_CA120_32T16X_H_
#define _LINUX_MFD_CA120_32T16X_H_

#include <linux/device.h>
#include <linux/types.h>

/**
 * struct ca120_32t16x_ops - Byte access ops exported by the MFD core.
 * @read8:  Read one byte from an 8-bit register address.
 * @write8: Write one byte to an 8-bit register address.
 *
 * These ops are passed via platform_data when creating the MFD child devices.
 * They may sleep and must be safe in process context.
 */
struct ca120_32t16x_ops {
    int (*read)(struct device *mfd_dev, u8 reg, u8 *val);
    int (*write)(struct device *mfd_dev, u8 reg, u8 val);
};

#define FXCVR_DEVNAME "ca120-32t16x-fxcvr"
#define CXCVR_DEVNAME "ca120-32t16x-cxcvr"
#define XCVR_DRVNAME "ca120-32t16x-xcvr"
#define FAN_DRVNAME "ca120-32t16x-fan"
#define PSU_DRVNAME "ca120-32t16x-psu"
#define PSU1_DEVNAME "ca120-32t16x-psu1"
#define PSU2_DEVNAME "ca120-32t16x-psu2"
#define LED_DRVNAME "ca120-32t16x-led"

#endif /* _LINUX_MFD_CA120_32T16X_H_ */