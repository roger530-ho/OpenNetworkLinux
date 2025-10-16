// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * CA120-32T16X Power Supply Unit (PSU) status driver.
 *
 * Copyright (C) 2025 Accton Technology Corporation.
 *
 * This driver binds to a platform device created by the ca120_32t16x-mfd core
 * driver. It provides hardware monitoring for a Power Supply Unit (PSU).
 *
 * The driver reports the following real-time status from the CPLD:
 * - Presence: Whether the PSU is physically inserted.
 * - Power Good: Whether the PSU output voltage is stable.
 */
#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/mod_devicetable.h>
#include "x86-64-apresia-ca120-32t16x-mfd.h"

/* PSU status register and bit definitions (within the CPLD) */
#define PSU_STATUS_REG          0x4A
#define PSU1_PRESENT_BIT        0
#define PSU1_POWER_GOOD_BIT     2
#define PSU2_PRESENT_BIT        4
#define PSU2_POWER_GOOD_BIT     6

/*
 * Cache update interval.
 * Defines how long a value read from hardware is considered valid.
 * This is to prevent flooding the hardware bus with too many unnecessary
 * I/O operations when multiple attributes are read in a short period.
 * - A smaller value increases data freshness but also increases I/O.
 * - A larger value improves efficiency but data may be stale.
 * HZ is the number of timer ticks per second, as defined by the kernel.
 */
#define UPDATE_INTERVAL         (HZ / 10) /* Currently 100ms */

/**
 * enum psu_index - Distinguishes between PSU1 and PSU2.
 * @PSU1: Index for the first power supply unit.
 * @PSU2: Index for the second power supply unit.
 */
enum psu_index {
    PSU1,
    PSU2
};

/**
 * struct psu_config - Defines CPLD bit layout for a specific PSU slot.
 * @present_bit:    The bit position for the presence signal (active-low).
 * @power_good_bit: The bit position for the power good signal.
 */
struct psu_config {
    u8 present_bit;
    u8 power_good_bit;
};

static const struct psu_config psu_configs[] = {
    [PSU1] = { PSU1_PRESENT_BIT, PSU1_POWER_GOOD_BIT },
    [PSU2] = { PSU2_PRESENT_BIT, PSU2_POWER_GOOD_BIT },
};

/**
 * struct ca120_32t16x_psu_data - Driver's private data structure.
 * @mfd_dev:      Pointer to the parent MFD device for CPLD access.
 * @ops:	  MFD-provided byte access operations (read/write).
 * @update_lock:  Mutex to protect access to all cached data.
 * @valid:        Flag indicating if the cached data is current.
 * @last_updated: Timestamp (in jiffies) of the last successful data update.
 * @index:        The PSU index (0 or 1) for this driver instance.
 * @status:       Cached value of the CPLD PSU status register.
 */
struct ca120_32t16x_psu_data {
    struct device *mfd_dev;
    const struct ca120_32t16x_ops *ops;
    struct mutex update_lock;
    bool valid;
    unsigned long last_updated;
    u8 index;
    u8 status;
};

/**
 * enum psu_sysfs_attributes - Indexes for sysfs attributes.
 * @PSU_PRESENT:    Index for the 'psu_present' attribute.
 * @PSU_POWER_GOOD: Index for the 'psu_power_good' attribute.
 */
enum psu_sysfs_attributes {
    PSU_PRESENT,
    PSU_POWER_GOOD,
};

/**
 * ca120_32t16x_psu_update_device() - Reads sensor data from CPLD into cache.
 * @data: Driver's private data structure.
 *
 * This function updates the cached data by reading from the CPLD.
 * It only performs the read if the cache is stale.
 *
 * @return: 0 on success, or a negative errno on failure.
 */
static int ca120_32t16x_psu_update_device(struct ca120_32t16x_psu_data *data)
{
    int ret = 0;

    mutex_lock(&data->update_lock);

    if (time_before(jiffies, data->last_updated + UPDATE_INTERVAL) && data->valid) {
        goto exit_unlock;
    }

    ret = data->ops->read(data->mfd_dev, PSU_STATUS_REG, &data->status);
    if (ret < 0) {
        dev_err(data->mfd_dev, "Failed to read CPLD status register: %d\n", ret);
        data->valid = false;
        goto exit_unlock;
    }

    data->last_updated = jiffies;
    data->valid = true;

exit_unlock:
    mutex_unlock(&data->update_lock);
    return ret;
}

/**
 * psu_attr_show() - Shared callback for reading all PSU sysfs attributes.
 * @dev:     The device being read.
 * @devattr: The specific device_attribute being read.
 * @buf:     The buffer to write the result into.
 *
 * @return: The number of bytes written, or a negative errno.
 */
static ssize_t psu_attr_show(struct device *dev, struct device_attribute *devattr, char *buf)
{
    struct ca120_32t16x_psu_data *data = dev_get_drvdata(dev);
    struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
    const struct psu_config *config = &psu_configs[data->index];
    int ret;

    ret = ca120_32t16x_psu_update_device(data);
    if (ret < 0) {
        return ret;
    }

    switch (attr->index) {
    case PSU_PRESENT:
        /* Presence bit is active-low, so we invert the logic. */
        return sprintf(buf, "%d\n", !((data->status >> config->present_bit) & 1));
    case PSU_POWER_GOOD:
        return sprintf(buf, "%d\n", (data->status >> config->power_good_bit) & 1);
    default:
        return -EINVAL;
    }
}

static SENSOR_DEVICE_ATTR_RO(psu_present, psu_attr, PSU_PRESENT);
static SENSOR_DEVICE_ATTR_RO(psu_power_good, psu_attr, PSU_POWER_GOOD);

static struct attribute *ca120_32t16x_psu_attrs[] = {
    &sensor_dev_attr_psu_present.dev_attr.attr,
    &sensor_dev_attr_psu_power_good.dev_attr.attr,
    NULL
};

static const struct attribute_group ca120_32t16x_psu_group = {
    .attrs = ca120_32t16x_psu_attrs,
};

static const struct attribute_group *ca120_32t16x_psu_groups[] = {
    &ca120_32t16x_psu_group,
    NULL
};

/**
 * ca120_32t16x_psu_probe() - Probe function for the PSU platform driver.
 * @pdev: The platform device instance.
 *
 * @return: 0 on success, or a negative errno on failure.
 */
static int ca120_32t16x_psu_probe(struct platform_device *pdev)
{
    const struct platform_device_id *id = platform_get_device_id(pdev);
    struct ca120_32t16x_psu_data *data;
    struct device *hwmon_dev;
    int ret;

    data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
    if (!data) {
        return -ENOMEM;
    }

    data->index = id->driver_data;
    data->mfd_dev = pdev->dev.parent;
    data->ops = dev_get_platdata(&pdev->dev);
    if (!data->ops || !data->ops->read || !data->ops->write) {
        dev_err(&pdev->dev, "missing ca120-32t16x ops from parent\n");
        return -ENODEV;
    }
    mutex_init(&data->update_lock);
    platform_set_drvdata(pdev, data);

    hwmon_dev = devm_hwmon_device_register_with_groups(&pdev->dev, "ca120_32t16x_psu",
                                                       data, ca120_32t16x_psu_groups);
    if (IS_ERR(hwmon_dev)) {
        ret = PTR_ERR(hwmon_dev);
        dev_err(&pdev->dev, "Failed to register hwmon device: %d\n", ret);
        return ret;
    }

    dev_info(&pdev->dev, "ca120-32t16x PSU%d driver registered successfully\n", data->index + 1);

    return 0;
}

/**
 * ca120_32t16x_psu_remove() - Remove function for the PSU platform driver.
 * @pdev: The platform device instance.
 *
 * All resources are devm-managed, so this function is empty.
 *
 * @return: 0 always.
 */
static int ca120_32t16x_psu_remove(struct platform_device *pdev)
{
    return 0;
}

static const struct platform_device_id ca120_32t16x_psu_ids[] = {
    { .name = PSU1_DEVNAME, .driver_data = PSU1 },
    { .name = PSU2_DEVNAME, .driver_data = PSU2 },
    { /* Sentinal */ }
};
MODULE_DEVICE_TABLE(platform, ca120_32t16x_psu_ids);


static struct platform_driver ca120_32t16x_psu_driver = {
    .probe = ca120_32t16x_psu_probe,
    .remove = ca120_32t16x_psu_remove,
    .id_table = ca120_32t16x_psu_ids,
    .driver = {
        .name = PSU_DRVNAME
    },
};
module_platform_driver(ca120_32t16x_psu_driver);

MODULE_AUTHOR("Roger Ho <roger530_ho@accton.com>");
MODULE_DESCRIPTION("CA120-32T16X PSU Driver");
MODULE_LICENSE("GPL");
