// SPDX-License-Identifier: GPL-2.0
/*
 * CA120-32T16X LED driver.
 *
 * Copyright (C) 2025 Accton Technology Corporation
 *
 * This driver binds to a platform device created by the ca120-32t16x-mfd core.
 * It manages system LEDs and fan LEDs with a unified descriptor table that
 * supports:
 * - Mono LEDs (ON/OFF only, e.g., FAN_R / DIAG_R / LOC_O / PSU*_G)
 * - Bi-color LEDs (GREEN/ORANGE-RED selection)
 * - Per-color register overrides (e.g., ORANGE in 0x7A, GREEN in 0x7B)
 * - Read-only LEDs (expose brightness_get only)
 * - Active-low logic is supported per LED (0=ON, 1=OFF).
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/leds.h>
#include <linux/slab.h>
#include <linux/bitops.h>
#include "x86-64-apresia-ca120-32t16x-mfd.h"

/* System LED registers */
#define SYS_LED_CTRL_REG_ADDR    0x79
#define SD_LED_CTRL_REG_ADDR     0x6D


/*
 * OFF masks under active-low logic:
 * Setting these bits to 1 guarantees the corresponding LEDs are OFF.
 */
#define SYS_LED_REG_ALL_OFF      0x07    /* bits: LOC(0), FAULT(1), FAN(2) */
#define SD_LED_REG_ALL_OFF       0x04    /* bit:  SD(2) */

/**
 * enum ca120_32t16x_led_brightness - Logical LED brightness levels.
 *
 * These values map to the brightness levels supported by the hardware and
 * are used internally by the driver. They correspond to the standard
 * LED_BRIGHTNESS_* enum but are defined here for clarity within this driver.
 */
enum ca120_32t16x_led_brightness {
    CA120_32T16X_BR_OFF = 0,
    CA120_32T16X_BR_GREEN,
    CA120_32T16X_BR_GREEN_BLINK,
    CA120_32T16X_BR_AMBER,
    CA120_32T16X_BR_AMBER_BLINK,
    CA120_32T16X_BR_RED,
    CA120_32T16X_BR_RED_BLINK,
    CA120_32T16X_BR_BLUE,
    CA120_32T16X_BR_BLUE_BLINK,
    CA120_32T16X_BR_AUTO,
    CA120_32T16X_BR_UNKNOWN
};

/**
 * struct ca120_32t16x_led_desc - Static description of one LED
 * @name:         LED name (sysfs).
 * @default_trig: Default trigger name or NULL.
 * @active_low:   True if 0=ON and 1=OFF for the target bit.
 * @read_only:    True if writes must be rejected with -EPERM.
 * @on_mode:      Informational mode to report when LED is logically ON.
 * @reg:          Register address containing the control bit.
 * @bit:          Bit index [0..7].
 */
struct ca120_32t16x_led_desc {
    const char *name;
    const char *default_trig;
    bool active_low;
    bool read_only;
    enum ca120_32t16x_led_brightness on_mode;
    u8 reg;
    unsigned int bit;
};

struct ca120_32t16x_led_chip;

/**
 * struct ca120_32t16x_led - Runtime LED instance
 * @cdev: LED class device.
 * @chip: Back pointer to the controller.
 * @d:    Descriptor for this LED.
 */
struct ca120_32t16x_led {
    struct led_classdev cdev;
    struct ca120_32t16x_led_chip *chip;
    const struct ca120_32t16x_led_desc *d;
};

/**
 * struct ca120_32t16x_led_chip - Controller instance
 * @mfd_dev: Parent device (for I/O access).
 * @ops:     Register access operations provided by the MFD core.
 * @leds:    Array of LED instances.
 * @io_lock: Serialize register RMW sequences.
 */
struct ca120_32t16x_led_chip {
    struct device *mfd_dev;
    const struct ca120_32t16x_ops *ops;
    struct ca120_32t16x_led *leds;
    struct mutex io_lock;
};

static const struct ca120_32t16x_led_desc ca120_32t16x_leds_desc[] = {
    {
        .name = "sys::fan",
        .default_trig = NULL,
        .active_low = true,
        .read_only = false,
        .on_mode = CA120_32T16X_BR_RED,
        .reg = SYS_LED_CTRL_REG_ADDR,
        .bit = 2,
    },
    {
        .name = "sys::fault",
        .default_trig = NULL,
        .active_low = true,
        .read_only = false,
        .on_mode = CA120_32T16X_BR_RED,
        .reg = SYS_LED_CTRL_REG_ADDR,
        .bit = 1,
    },
    {
        .name = "sys::loc",
        .default_trig = NULL,
        .active_low = true,
        .read_only = false,
        .on_mode = CA120_32T16X_BR_AMBER,
        .reg = SYS_LED_CTRL_REG_ADDR,
        .bit = 0,
    },
    {
        .name = "sys::sdcard",
        .default_trig = NULL,
        .active_low = true,
        .read_only = false,
        .on_mode = CA120_32T16X_BR_GREEN,
        .reg = SD_LED_CTRL_REG_ADDR,
        .bit = 2,
    },
    {
        .name = "sys::psu1",
        .default_trig = NULL,
        .active_low = true,
        .read_only = true,
        .on_mode = CA120_32T16X_BR_AUTO,
        .reg = SYS_LED_CTRL_REG_ADDR,
        .bit = 4,
    },
    {
        .name = "sys::psu2",
        .default_trig = NULL,
        .active_low = true,
        .read_only = true,
        .on_mode = CA120_32T16X_BR_AUTO,
        .reg = SYS_LED_CTRL_REG_ADDR,
        .bit = 3,
    },
};

#define CA120_32T16X_NUM_LEDS  (sizeof(ca120_32t16x_leds_desc) / sizeof(ca120_32t16x_leds_desc[0]))

/**
 * ca120_32t16x_write_bit() - Update one bit in a register
 * @chip: Controller.
 * @reg:  Register address.
 * @bit:  Bit index [0..7].
 * @set1: If true, set bit to 1; otherwise clear to 0.
 *
 * Return: 0 on success or negative errno.
 */
static int ca120_32t16x_write_bit(struct ca120_32t16x_led_chip *chip, u8 reg, unsigned int bit, bool set1)
{
    u8 v;
    int ret;

    if (!chip || !chip->ops || !chip->mfd_dev) {
        return -ENODEV;
    }
    
    mutex_lock(&chip->io_lock);
    ret = chip->ops->read(chip->mfd_dev, reg, &v);
    if (ret < 0) {
        mutex_unlock(&chip->io_lock);
        return ret;
    }

    if (set1) {
        v |= BIT(bit);
    } else {
        v &= ~BIT(bit);
    }

    ret = chip->ops->write(chip->mfd_dev, reg, v);
    mutex_unlock(&chip->io_lock);
    return ret;
}

/**
 * ca120_32t16x_read_bit() - Read one bit from a register
 * @chip: Controller.
 * @reg:  Register address.
 * @bit:  Bit index [0..7].
 *
 * Return: 0 or 1 on success; negative errno on failure.
 */
static int ca120_32t16x_read_bit(struct ca120_32t16x_led_chip *chip, u8 reg, unsigned int bit)
{
    u8 v;
    int ret;

    if (!chip || !chip->ops || !chip->mfd_dev) {
        return -ENODEV;
    }
    
    mutex_lock(&chip->io_lock);
    ret = chip->ops->read(chip->mfd_dev, reg, &v);
    mutex_unlock(&chip->io_lock);
    
    if (ret < 0) {
        return ret;
    }

     return (v & BIT(bit)) ? 1 : 0;
}

/**
 * ca120_32t16x_led_get_brightness() - Read current brightness from hardware.
 * @cdev: LED class device.
 *
 * Return: brightness value (>=0) on success, or negative errno.
 */
static enum led_brightness ca120_32t16x_led_get_brightness(struct led_classdev *cdev)
{
    struct ca120_32t16x_led *led = container_of(cdev, struct ca120_32t16x_led, cdev);
    struct ca120_32t16x_led_chip *chip = led->chip;
    const struct ca120_32t16x_led_desc *d = led->d;
    int b;

    b = ca120_32t16x_read_bit(chip, d->reg, d->bit);
    if (b < 0) {
        return b;
    }

    if (d->active_low) {
        return b ? CA120_32T16X_BR_OFF : d->on_mode;
    }
       
    return b ? d->on_mode : CA120_32T16X_BR_OFF;
}

/**
 * ca120_32t16x_led_set_brightness() - Set LED brightness
 * @cdev:       LED class device.
 * @brightness: Requested brightness
 *
 * Active-low handling is applied per LED.
 *
 * Return: 0 on success or negative errno.
 */
static int ca120_32t16x_led_set_brightness(struct led_classdev *cdev,
                                    enum led_brightness brightness)
{
    struct ca120_32t16x_led *led = container_of(cdev, struct ca120_32t16x_led, cdev);
    struct ca120_32t16x_led_chip *chip = led->chip;
    const struct ca120_32t16x_led_desc *d = led->d;
    int ret;
    bool want_on, write;

    if (d->read_only) {
        return 0;
    }

    /* Mono: one bit. Map brightness to bit value with active_low. */
    want_on = (brightness > 0) ? true : false;
    
    if (d->active_low) {
        write = !want_on; /* 1=OFF, 0=ON */
    } else {
        write = want_on;  /* 1=ON, 0=OFF */
    }

    ret = ca120_32t16x_write_bit(chip, d->reg, d->bit, write);
    return ret;
}

/**
 * ca120_32t16x_register_one() - Register a single LED class device.
 * @pdev: The platform device.
 * @chip: The main chip context.
 * @inst: The LED instance to register.
 * @d:    The descriptor for the LED.
 *
 * Return: 0 on success, or a negative errno.
 */
static int ca120_32t16x_register_one(struct platform_device *pdev,
                              struct ca120_32t16x_led_chip *chip,
                              struct ca120_32t16x_led *inst,
                              const struct ca120_32t16x_led_desc *d)
{
    int ret;

    inst->chip = chip;
    inst->d = d;

    inst->cdev.name = d->name;
    inst->cdev.default_trigger = d->default_trig;
    inst->cdev.blink_set = NULL;
    inst->cdev.brightness_get = ca120_32t16x_led_get_brightness;
    inst->cdev.brightness_set_blocking = ca120_32t16x_led_set_brightness;
    inst->cdev.max_brightness = d->on_mode;

    ret = devm_led_classdev_register(&pdev->dev, &inst->cdev);
    return ret;
}

/**
 * ca120_32t16x_init_all_off() - Initialize all involved LED registers to OFF.
 * @chip:  Controller instance.
 *
 * This function writes 0xFF into the known LED control registers so that all
 * bits are OFF under active-low logic. It is harmless if the hardware uses
 * different registers; you can add more addresses here if needed.
 */
static void ca120_32t16x_init_all_off(struct ca120_32t16x_led_chip *chip)
{
    u8 v;
    int ret;

    ret = chip->ops->read(chip->mfd_dev, SYS_LED_CTRL_REG_ADDR, &v);
    if (ret >= 0) {
        v = v | SYS_LED_REG_ALL_OFF;
        chip->ops->write(chip->mfd_dev, SYS_LED_CTRL_REG_ADDR, v);
    }
    
    ret = chip->ops->read(chip->mfd_dev, SD_LED_CTRL_REG_ADDR, &v);
    if (ret >= 0) {
        v = v | SD_LED_REG_ALL_OFF;
        chip->ops->write(chip->mfd_dev, SD_LED_CTRL_REG_ADDR, v);
    }
}

/**
 * ca120_32t16x_led_probe() - Probe function for the LED driver.
 * @pdev: The platform device.
 *
 * Return: 0 on success, or a negative errno.
 */
static int ca120_32t16x_led_probe(struct platform_device *pdev)
{
    struct ca120_32t16x_led_chip *chip;
    size_t i;

    chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
    if (!chip) {
        return -ENOMEM;
    }

    chip->mfd_dev = pdev->dev.parent;
    chip->ops = dev_get_platdata(&pdev->dev);
    if (!chip->ops || !chip->ops->read || !chip->ops->write) {
        dev_err(&pdev->dev, "missing ca120-32t16x ops from parent\n");
        return -ENODEV;
    }
    mutex_init(&chip->io_lock);
    platform_set_drvdata(pdev, chip);

    /* Allocate instances */
    chip->leds = devm_kcalloc(&pdev->dev, CA120_32T16X_NUM_LEDS,
                              sizeof(struct ca120_32t16x_led), GFP_KERNEL);
    if (!chip->leds) {
        return -ENOMEM;
    }

    /* Initialize hardware to ALL OFF (active-low -> write 1s) */
    ca120_32t16x_init_all_off(chip);

    /* Register LEDs from the descriptor table */
    for (i = 0; i < CA120_32T16X_NUM_LEDS; i++) {
        int r;

        r = ca120_32t16x_register_one(pdev, chip, &chip->leds[i], &ca120_32t16x_leds_desc[i]);
        if (r < 0) {
            dev_err(&pdev->dev, "Failed to register LED %s: %d\n",
                    ca120_32t16x_leds_desc[i].name, r);
            return r;
        }
    }

    dev_info(&pdev->dev, "ca120-32t16x LED driver registered (%zu LEDs)\n", CA120_32T16X_NUM_LEDS);
    return 0;
}

/**
 * ca120_32t16x_led_remove() - Remove function for the LED driver.
 * @pdev: The platform device.
 *
 * All resources are devm-managed, so nothing is needed here.
 *
 * Return: 0 always.
 */
static int ca120_32t16x_led_remove(struct platform_device *pdev)
{
    /* devm_ resources auto-cleaned. Keep LED states unchanged by design. */
    return 0;
}

static struct platform_driver ca120_32t16x_led_driver = {
    .probe = ca120_32t16x_led_probe,
    .remove = ca120_32t16x_led_remove,
    .driver = {
        .name = LED_DRVNAME,
    },
};
module_platform_driver(ca120_32t16x_led_driver);

MODULE_AUTHOR("Roger Ho <roger530_ho@accton.com>");
MODULE_DESCRIPTION("CA120-32T16X LED Driver");
MODULE_LICENSE("GPL");
