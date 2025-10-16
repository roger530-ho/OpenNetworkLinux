// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * CA120-32T16X Fan driver.
 *
 * Copyright (C) 2025 Accton Technology Corporation.
 *
 * This driver binds to a platform device created by the ca120-32t16x-mfd core
 * driver. It provides hardware monitoring for the fan controller.
 *
 * The driver reports the following through the hwmon sysfs interface:
 * - Fan Speed: 4 inputs (fan1_input..fan4_input) for 4 fans.
 * - Fan Fault: Fault status for each rotor (fanX_fault).
 * - Fan Presence: Presence status for each of the 4 fan trays.
 * - PWM Control: A single shared PWM output (pwm1) for all fans.
 */
#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>
#include <linux/hwmon.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/hwmon-sysfs.h>
#include "x86-64-apresia-ca120-32t16x-mfd.h"

/* Register definitions for the fan controller */
#define REG_FAN_PRESENT         0x40 /* Bitmask for fan presence (active low) */
#define REG_FAN_FAULT           0x43 /* Bitmask for fan fault (active low) */
#define REG_FAN_DIRECTION       0xFF /* Reserved for fan direction (currently unused by HW) */
#define REG_FAN_PWM_CONTROL     0x30 /* Register to control the duty cycle */


/*
 * The device has 4 fan speed channels, mapped to specific registers.
 * Channels 0-3 correspond to fans 1-4.
 */
static const u8 fan_speed_regs[] = {
    0x31, 0x33, 0x35, 0x37,  /* fans 1-4 */
};

/* Hardware-specific constants */
#define NUM_FANS               4    /* Number of physical fan units (e.g., fan1, fan2) */
#define NUM_FAN_CHANNELS       ARRAY_SIZE(fan_speed_regs)    /* Number of fan speed sensor channels (front/rear rotors) */
#define FAN_PWM_MAX            100      /* Maximum PWM duty cycle value (represents 0-100%) */
#define FAN_RPM_FACTOR         (4 * 30)    /* Multiplier to convert register value to RPM */
#define UPDATE_INTERVAL        500    /* Default data update interval in milliseconds */

/**
 * struct ca120_32t16x_fan_data - Driver's private data structure
 * @mfd_dev:	 Pointer to the parent MFD device for I/O.
 * @ops:	 MFD-provided byte access operations (read/write).
 * @update_lock:     Mutex to protect access to cached data during updates.
 * @last_updated:    Timestamp (in jiffies) of the last successful data update.
 * @valid:	   Flag indicating if the cached data is currently valid.
 * @pwm:	     Cached value of the PWM control register.
 * @fan_present:     Cached value of the fan presence register.
 * @fan_direction:   Cached value of the fan direction register.
 * @fan_fault:	 Cached value of the fan fault register.
 * @fan_speed:	 Array of cached fan speed register values.
 * @update_interval: Configurable data update interval in milliseconds.
 */
struct ca120_32t16x_fan_data {
    struct device *mfd_dev;
    const struct ca120_32t16x_ops *ops;
    struct mutex update_lock;

    /* Cached data, protected by update_lock */
    unsigned long last_updated;
    bool valid;
    u8 pwm;
    u8 fan_present;
    u8 fan_direction;
    u8 fan_fault;
    u8 fan_speed[NUM_FAN_CHANNELS];

    /* Configuration */
    u32 update_interval;
};

/**
 * ca120_32t16x_fan_rpm_from_reg() - Convert register value to RPM.
 * @reg_val: The raw 8-bit register value.
 *
 * @return: The calculated RPM value.
 */
static inline int ca120_32t16x_fan_rpm_from_reg(u8 reg_val)
{
    /* R.P.M value = read value x 4*60/2  */
    return reg_val * FAN_RPM_FACTOR;
}

/**
 * ca120_32t16x_fan_pwm_to_reg() - Convert percentage (0..100) to 5-bit step code (0..16).
 * @pwm: Target duty in percent. Values are clamped to [0, 100].
 *
 * Hardware implements 17 equidistant steps: N/16 where N = 0..16.
 * We round to nearest step using DIV_ROUND_CLOSEST().
 *
 * Return: Register code in range [0, 16] (to be written into 0x30).
 */
static inline u8 ca120_32t16x_fan_pwm_to_reg(int pwm)
{
    int steps;

    if (pwm < 0) {
        pwm = 0;
    } else if (pwm > 100) {
        pwm = 100;
    }

    /* Round to nearest 1/16 step (N in [0..16]); pwm is integer 0..100.
     * Rounding map [pwm% -> step]:
     *   1-3->0, 4-9->1, 10-15->2, 16-21->3, 22-28->4,
     *   29-34->5, 35-40->6, 41-46->7, 47-53->8, 54-59->9,
     *   60-65->10, 66-71->11, 72-78->12, 79-84->13,
     *   85-90->14, 91-96->15, 97-100->16; (0->0).
     * Boundary rule for integers: N->N+1 when
     *   pwm >= ceil(((2*N + 1) * 100) / 32).
     * Example: 7->8 flips at pwm >= 47 (ceil(1500/32)=47).
     */  
    steps = DIV_ROUND_CLOSEST(pwm * 16, 100);

    if (steps < 0) {
        steps = 0;
    } else if (steps > 16) {
        steps = 16;
    }

    return (u8)steps;
}

/**
 * ca120_32t16x_fan_pwm_from_reg - Convert 5-bit step code (0..16) to percentage (0..100).
 * @reg_val: Raw register code read from 0x30.
 *
 * We report the mathematically correct percent = round(N * 100 / 16).
 * If user prefers "rounded buckets" (e.g., show 70 instead of 75),
 * do that only at presentation layer, not in the driver core.
 *
 * Return: Percentage in [0, 100].
 */
static inline int ca120_32t16x_fan_pwm_from_reg(u8 reg_val)
{
    int code = reg_val;

    if (code < 0) {
        code = 0;
    } else if (code > 16) {
        code = 16;
    }

    /* Convert step code N (0..16) to integer percent using rounding:
     *   percent = DIV_ROUND_CLOSEST(N * 100, 16)
     * Mapping [code -> %]:
     *   0->0, 1->6, 2->13, 3->19, 4->25, 5->31, 6->38, 7->44,
     *   8->50, 9->56, 10->63, 11->69, 12->75, 13->81, 14->88,
     *   15->94, 16->100.
     * Note: Mirrors ca120_32t16x_fan_pwm_to_reg() rounding; readback equals
     * round(pwm).
     */
    return DIV_ROUND_CLOSEST(code * 100, 16);
}

/**
 * ca120_32t16x_fan_is_present() - Check if a fan is present.
 * @present_reg: The value of the fan presence register.
 * @fan_id: The fan index to check (0-3).
 *
 * Return: True if the fan is present, false otherwise.
 */
static inline bool ca120_32t16x_fan_is_present(u8 present_reg, int fan_id)
{
    return !(present_reg & BIT(fan_id));
}

/**
 * ca120_32t16x_fan_is_fault() - Check if a fan is faulty.
 * @fault_reg: The value of the fan fault register.
 * @fan_id: The fan index to check (0-3).
 *
 * Return: True if the fan is faulty, false otherwise.
 */
static inline bool ca120_32t16x_fan_is_fault(u8 fault_reg, int fan_id)
{
    return !(fault_reg & BIT(fan_id));
}

/**
 * ca120_32t16x_fan_direction() - Get the fan direction.
 * @direction_reg: The value of the fan direction register.
 * @fan_id: The fan index to check.
 *
 * Return: 0 for Front-to-Back, 1 for Back-to-Front.
 */
static inline bool ca120_32t16x_fan_direction(u8 direction_reg, int fan_id)
{
    return 0; /* Fixed to F2B (0) as per hardware limitation */
}

/**
 * ca120_32t16x_fan_update_device() - Update cached data from the hardware.
 * @data: Pointer to the driver's private data.
 *
 * This function reads all sensor data from the device into the driver's
 * cache. It enforces a minimum interval between updates to prevent
 * bus flooding.
 *
 * Return: 0 on success, or a negative errno on failure.
 */
static int ca120_32t16x_fan_update_device(struct ca120_32t16x_fan_data *data)
{
    int ret = 0;
    int i;

    mutex_lock(&data->update_lock);

    /*
     * Only update if the cache is invalid or the update interval has passed.
     * This check is crucial for performance.
     */
    if (time_before(jiffies, data->last_updated + msecs_to_jiffies(data->update_interval))
        && data->valid) {
        goto exit_unlock;
    }

    data->valid = false;
    /* Read the shared PWM control value */
    ret = data->ops->read(data->mfd_dev, REG_FAN_PWM_CONTROL, &data->pwm);
    if (ret < 0) {
        goto exit_unlock;
    }

    /* Read the fan presence status for all fans */
    ret = data->ops->read(data->mfd_dev, REG_FAN_PRESENT, &data->fan_present);
    if (ret < 0) {
        goto exit_unlock;
    }

    /*
     * TODO: Fan direction is not supported by the hardware.
     * We skip reading the register and use a fixed value.
     */
    data->fan_direction = 0;

    /* Read the fan fault status for all fans */
    ret = data->ops->read(data->mfd_dev, REG_FAN_FAULT, &data->fan_fault);
    if (ret < 0) {
        goto exit_unlock;
    }

    /* Read the speed for all 8 fan channels */
    for (i = 0; i < NUM_FAN_CHANNELS; i++) {
        ret = data->ops->read(data->mfd_dev, fan_speed_regs[i], &data->fan_speed[i]);
        if (ret < 0) {
            goto exit_unlock;
        }
    }

    data->last_updated = jiffies;
    data->valid = true;

exit_unlock:
    mutex_unlock(&data->update_lock);
    return ret;
}


/**
 * fan_pwm_show() - Sysfs callback to show PWM duty cycle.
 * @dev: Device pointer.
 * @attr: Device attribute pointer.
 * @buf: Buffer to write the value to.
 *
 * Return: Number of bytes written, or a negative errno.
 */
static ssize_t fan_pwm_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct ca120_32t16x_fan_data *data = dev_get_drvdata(dev);
    int ret;

    ret = ca120_32t16x_fan_update_device(data);
    if (ret < 0)
        return ret;

    return sprintf(buf, "%d\n", ca120_32t16x_fan_pwm_from_reg(data->pwm));
}

/**
 * fan_pwm_store() - Sysfs callback to set PWM duty cycle.
 * @dev: Device pointer.
 * @attr: Device attribute pointer.
 * @buf: Buffer containing the value to write.
 * @count: Number of bytes in the buffer.
 *
 * Return: Number of bytes written, or a negative errno.
 */
static ssize_t fan_pwm_store(struct device *dev, struct device_attribute *attr,
                         const char *buf, size_t count)
{
    struct ca120_32t16x_fan_data *data = dev_get_drvdata(dev);
    long val;
    u8 reg_val;
    int ret;

    ret = kstrtol(buf, 10, &val);
    if (ret < 0) {
        return ret;
    }

    reg_val = ca120_32t16x_fan_pwm_to_reg(val);

    /* Keep HW write and cache update atomic w.r.t. readers. */
    mutex_lock(&data->update_lock);
    ret = data->ops->write(data->mfd_dev, REG_FAN_PWM_CONTROL, reg_val);
    if (ret == 0) {
        data->pwm = reg_val;
        data->last_updated = jiffies;
        data->valid = true;
    }
    mutex_unlock(&data->update_lock);
    if (ret < 0)
        return ret;

    return count;
}

/**
 * FAN_INPUT_ATTR - Macro to generate a fanX_input show function.
 * @index: The fan channel number (1-8).
 *
 * Creates a sysfs show function that reads the cached speed for the given
 * fan channel and converts it to RPM.
 */
#define FAN_INPUT_ATTR(index) \
static ssize_t fan##index##_input_show(struct device *dev, \
                                      struct device_attribute *attr, char *buf) \
{ \
    struct ca120_32t16x_fan_data *data = dev_get_drvdata(dev); \
    int ret; \
    \
    ret = ca120_32t16x_fan_update_device(data); \
    if (ret < 0) \
        return ret; \
    \
    return sprintf(buf, "%d\n", ca120_32t16x_fan_rpm_from_reg(data->fan_speed[index-1])); \
}

FAN_INPUT_ATTR(1);
FAN_INPUT_ATTR(2);
FAN_INPUT_ATTR(3);
FAN_INPUT_ATTR(4);

/**
 * FAN_FAULT_ATTR - Macro to generate a fanX_fault show function.
 * @index: The fan channel number (1-8).
 *
 * Creates a sysfs show function that reports a fault (1) if the fan speed
 * is zero, and no fault (0) otherwise.
 */
#define FAN_FAULT_ATTR(index) \
static ssize_t fan##index##_fault_show(struct device *dev, \
                                       struct device_attribute *attr, char *buf) \
{ \
    struct ca120_32t16x_fan_data *data = dev_get_drvdata(dev); \
    int ret; \
    \
    ret = ca120_32t16x_fan_update_device(data); \
    if (ret < 0) \
        return ret; \
    \
    return sprintf(buf, "%d\n", \
                   ca120_32t16x_fan_is_fault(data->fan_fault, index-1) ? 1 : 0); \
}

FAN_FAULT_ATTR(1);
FAN_FAULT_ATTR(2);
FAN_FAULT_ATTR(3);
FAN_FAULT_ATTR(4);

/**
 * FAN_PRESENT_ATTR - Macro to generate a fanX_present show function.
 * @index: The physical fan number (1-4).
 *
 * Creates a sysfs show function that reports if a fan tray is present.
 */
#define FAN_PRESENT_ATTR(index) \
static ssize_t fan##index##_present_show(struct device *dev, \
                                         struct device_attribute *attr, char *buf) \
{ \
    struct ca120_32t16x_fan_data *data = dev_get_drvdata(dev); \
    int ret; \
    \
    ret = ca120_32t16x_fan_update_device(data); \
    if (ret < 0) \
        return ret; \
    \
    return sprintf(buf, "%d\n", \
                   ca120_32t16x_fan_is_present(data->fan_present, index-1) ? 1 : 0); \
}

FAN_PRESENT_ATTR(1);
FAN_PRESENT_ATTR(2);
FAN_PRESENT_ATTR(3);
FAN_PRESENT_ATTR(4);

/**
 * FAN_DIRECTION_ATTR - Macro to generate a fanX_direction show function.
 * @index: The physical fan number (1-4).
 *
 * Creates a sysfs show function for airflow direction. Currently a placeholder.
 */
#define FAN_DIRECTION_ATTR(index) \
static ssize_t fan##index##_direction_show(struct device *dev, \
                                           struct device_attribute *attr, char *buf) \
{ \
    struct ca120_32t16x_fan_data *data = dev_get_drvdata(dev); \
    int ret; \
    \
    ret = ca120_32t16x_fan_update_device(data); \
    if (ret < 0) \
        return ret; \
    \
    return sprintf(buf, "%d\n", \
                   ca120_32t16x_fan_direction(data->fan_direction, index-1) ? 1 : 0); \
}

FAN_DIRECTION_ATTR(1);
FAN_DIRECTION_ATTR(2);
FAN_DIRECTION_ATTR(3);
FAN_DIRECTION_ATTR(4);

/* Define device attributes using hwmon-sysfs helpers */
#define ATTR_INDEX_UNUSED 0 /* For SENSOR_DEVICE_ATTR index when callback is not shared */

static SENSOR_DEVICE_ATTR_RW(fan_pwm, fan_pwm, ATTR_INDEX_UNUSED);
static SENSOR_DEVICE_ATTR_RO(fan1_input, fan1_input, ATTR_INDEX_UNUSED);
static SENSOR_DEVICE_ATTR_RO(fan2_input, fan2_input, ATTR_INDEX_UNUSED);
static SENSOR_DEVICE_ATTR_RO(fan3_input, fan3_input, ATTR_INDEX_UNUSED);
static SENSOR_DEVICE_ATTR_RO(fan4_input, fan4_input, ATTR_INDEX_UNUSED);
static SENSOR_DEVICE_ATTR_RO(fan1_fault, fan1_fault, ATTR_INDEX_UNUSED);
static SENSOR_DEVICE_ATTR_RO(fan2_fault, fan2_fault, ATTR_INDEX_UNUSED);
static SENSOR_DEVICE_ATTR_RO(fan3_fault, fan3_fault, ATTR_INDEX_UNUSED);
static SENSOR_DEVICE_ATTR_RO(fan4_fault, fan4_fault, ATTR_INDEX_UNUSED);
static SENSOR_DEVICE_ATTR_RO(fan1_present, fan1_present, ATTR_INDEX_UNUSED);
static SENSOR_DEVICE_ATTR_RO(fan2_present, fan2_present, ATTR_INDEX_UNUSED);
static SENSOR_DEVICE_ATTR_RO(fan3_present, fan3_present, ATTR_INDEX_UNUSED);
static SENSOR_DEVICE_ATTR_RO(fan4_present, fan4_present, ATTR_INDEX_UNUSED);
static SENSOR_DEVICE_ATTR_RO(fan1_direction, fan1_direction, ATTR_INDEX_UNUSED);
static SENSOR_DEVICE_ATTR_RO(fan2_direction, fan2_direction, ATTR_INDEX_UNUSED);
static SENSOR_DEVICE_ATTR_RO(fan3_direction, fan3_direction, ATTR_INDEX_UNUSED);
static SENSOR_DEVICE_ATTR_RO(fan4_direction, fan4_direction, ATTR_INDEX_UNUSED);

/* Array of all attributes */
static struct attribute *ca120_32t16x_fan_attrs[] = {
    &sensor_dev_attr_fan_pwm.dev_attr.attr,
    &sensor_dev_attr_fan1_input.dev_attr.attr,
    &sensor_dev_attr_fan2_input.dev_attr.attr,
    &sensor_dev_attr_fan3_input.dev_attr.attr,
    &sensor_dev_attr_fan4_input.dev_attr.attr,
    &sensor_dev_attr_fan1_fault.dev_attr.attr,
    &sensor_dev_attr_fan2_fault.dev_attr.attr,
    &sensor_dev_attr_fan3_fault.dev_attr.attr,
    &sensor_dev_attr_fan4_fault.dev_attr.attr,
    &sensor_dev_attr_fan1_present.dev_attr.attr,
    &sensor_dev_attr_fan2_present.dev_attr.attr,
    &sensor_dev_attr_fan3_present.dev_attr.attr,
    &sensor_dev_attr_fan4_present.dev_attr.attr,
    &sensor_dev_attr_fan1_direction.dev_attr.attr,
    &sensor_dev_attr_fan2_direction.dev_attr.attr,
    &sensor_dev_attr_fan3_direction.dev_attr.attr,
    &sensor_dev_attr_fan4_direction.dev_attr.attr,
    NULL
};

static const struct attribute_group ca120_32t16x_fan_group = {
    .attrs = ca120_32t16x_fan_attrs,
};

static const struct attribute_group *ca120_32t16x_fan_groups[] = {
    &ca120_32t16x_fan_group,
    NULL
};

/* Allow the update interval to be configured at module load time */
static unsigned int update_interval = UPDATE_INTERVAL;
module_param(update_interval, uint, 0644);
MODULE_PARM_DESC(update_interval, "Data update interval in milliseconds (default: 1000)");

/**
 * ca120_32t16x_fan_probe() - Probe function for the platform driver.
 * @pdev: The platform device instance.
 *
 * This function is called when a device matching the driver is found.
 * It allocates driver data, initializes it, and registers with the
 * hwmon subsystem to create the sysfs interface.
 *
 * Return: 0 on success, or a negative errno on failure.
 */
static int ca120_32t16x_fan_probe(struct platform_device *pdev)
{
    struct ca120_32t16x_fan_data *data;
    struct device *hwmon_dev;
    int ret;

    /* Allocate and initialize the driver's private data structure */
    data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->mfd_dev = pdev->dev.parent;
    data->ops = dev_get_platdata(&pdev->dev);
    if (!data->ops || !data->ops->read || !data->ops->write) {
        dev_err(&pdev->dev, "missing ca120-32t16x ops from parent\n");
        return -ENODEV;
    }
    mutex_init(&data->update_lock);
    /* Use the value from the module parameter, or the default */
    data->update_interval = update_interval;

    /* Set driver data for sysfs callbacks */
    platform_set_drvdata(pdev, data);

    /*
     * Register the device with the hwmon subsystem using only custom attributes.
     * This will create all the sysfs files based on ca120_32t16x_fan_groups.
     */
    hwmon_dev = devm_hwmon_device_register_with_groups(&pdev->dev, 
                                                       "ca120_32t16x_fan",
                                                       data,
                                                       ca120_32t16x_fan_groups);
    if (IS_ERR(hwmon_dev)) {
        ret = PTR_ERR(hwmon_dev);
        dev_err(&pdev->dev, "Failed to register hwmon device: %d\n", ret);
        return ret;
    }

    dev_info(&pdev->dev, "ca120-32t16x fan driver registered successfully\n");
    return 0;
}

static struct platform_driver ca120_32t16x_fan_driver = {
    .probe = ca120_32t16x_fan_probe,
    .driver = {
        .name = FAN_DRVNAME
    },
};
module_platform_driver(ca120_32t16x_fan_driver);

MODULE_AUTHOR("Roger Ho <roger530_ho@accton.com>");
MODULE_DESCRIPTION("CA120-32T16X Fan Driver");
MODULE_LICENSE("GPL");
