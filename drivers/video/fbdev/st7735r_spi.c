#define pr_fmt(fmt) "ST7735R: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/delay.h>


#define ST7735R_NOOP        0x00
#define ST7735R_SWRESET     0x01
#define ST7735R_RDDID       0x04
#define ST7735R_RDDST       0x09
#define ST7735R_SLPIN       0x10
#define ST7735R_SLPOUT      0x11
#define ST7735R_DISPOFF     0x28
#define ST7735R_DISPON      0x29
#define ST7735R_CASET       0x2A
#define ST7735R_RASET       0x2B
#define ST7735R_RAMWR       0x2C
#define ST7735R_MADCTL      0x36
#define ST7735R_COLMOD      0x3A



/* to support Device via Device Tree Bindings */
static const struct of_device_id my_st7735r_dt_ids[] = {
	{.compatible = "abhishek,st7735r-test",
	/* .data = (void *) x, */ },
    /* {.compatible = "my-st7735r-lcd"}, */
    { }
};
MODULE_DEVICE_TABLE(of, my_st7735r_dt_ids);

/* to support Device and allow Driver matching using SPI Device ID (non-DT discovery) */
 static const struct spi_device_id my_st7735r_ids[] = {
	{"st7735r", 0 },
	{"my-st7735r-lcd", 0},
    {}
};
MODULE_DEVICE_TABLE(spi, my_st7735r_ids);

static int my_st7735r_probe(struct spi_device *spi);
static void my_st7735r_remove(struct spi_device *spi);


/* my driver's state/struct */
struct st7735r {
    // Kernel Interfaces/Contracts
    struct spi_device *spi;
    struct device *dev;

    // Control Signals
    struct gpio_desc *reset_gpio;
    struct gpio_desc *dc_gpio;
    struct gpio_desc *backlight_gpio;

    // Driver Responsibilities
    u8 rotation;
    u16 width;
    u16 height;
    u8 pixel_format;
    u8 power_level;     // Power Level Mode of the LCD

    // Hardware State - Datasheet influenced
    u8 madctl;
    u8 colmod;

    // Device Runtime State
    bool enabled;
    struct mutex lock;
};

static struct spi_driver my_st7735r_drv = {
	.driver = {
		.name = "my-st7735r-lcd",
		.of_match_table = my_st7735r_dt_ids,
	},
    .id_table = my_st7735r_ids,
	.probe = my_st7735r_probe,
	.remove = my_st7735r_remove,
};

static void st7735r_hw_reset(struct st7735r *lcd)
{
    /* active-low reset */
    gpiod_set_value_cansleep(lcd->reset_gpio, 0);
    msleep(20);

    gpiod_set_value_cansleep(lcd->reset_gpio, 1);
    msleep(150);
}

static void st7735r_backlight_on(struct st7735r *lcd) {
    gpiod_set_value_cansleep(lcd->backlight_gpio, 1);
}


static void st7735r_backlight_off(struct st7735r *lcd) {
    gpiod_set_value_cansleep(lcd->backlight_gpio, 0);
}


/* helpers for cmd/data write */
static int st7735r_spi_write(struct st7735r *lcd, const void *buf, size_t len) {
    struct spi_transfer t = {
        .tx_buf = buf,
        .len = len,
    };

    struct spi_message m;

    spi_message_init(&m);
    spi_message_add_tail(&t, &m);

    return spi_sync(lcd->spi, &m);
}

static int st7735r_write_cmd(struct st7735r *lcd, u8 cmd) {
    gpiod_set_value_cansleep(lcd->dc_gpio, 0);
    return st7735r_spi_write(lcd, &cmd, 1);
}

static int st7735r_write_data(struct st7735r *lcd, const void *buf, size_t len) {
    gpiod_set_value_cansleep(lcd->dc_gpio, 1);
    return st7735r_spi_write(lcd, buf, len);
}


static void st7735r_sw_reset(struct st7735r *lcd)
{
    st7735r_write_cmd(lcd, ST7735R_SWRESET);
    
    // datasheet recommends it must be waited 120ms before sending next command to display module, once SWRESET has been done, during both SLEEP IN or SLEEP OUT modes
    msleep(150);
}

/* power init sequence for the LCD while display bringup 
 * The init sequence is:
 * 1. Hardware RESET
 * 2. Software RESET    (not required, but safer)
 * 3. 120ms Delay       (for the module to restore default values of internal registers)
 * 4. Sleep Out/SLPOUT  (needed, because post POWERON, LCD module's default state is in SLEEP IN mode, meaning display scan engine off, timing generator off, analog blocks off)
 * 5. Delay             (mdndatory)
 * 6. Configuration     (CMD level config like MADCTL, COLMOD, etc)
 * 7. Display On/DISPON (needed, because post POWERON, or SW/HW RESET, default lcd is in DISPLAY OFF mode by default)
 * */
static int st7735r_init_sequence(struct st7735r *lcd)
{
    st7735r_hw_reset(lcd);
    st7735r_sw_reset(lcd);

    /*  TODO: extras - maybe check if the module is really in sleep in mode here. while testing, add a READ status command to read the current SLEEP IN/OUT mode of the display module */
    
    st7735r_write_cmd(lcd, ST7735R_SLPOUT);
    msleep(120);

    /* configuration */
    /* Pixel Format : RGB565/16-bit */
    st7735r_write_cmd(lcd, ST7735R_COLMOD);
    u8 fmt = 0x05;
    st7735r_write_data(lcd, &fmt, 1);
    
    /* Memory Access Control - 0x00 for RGB */
    st7735r_write_cmd(lcd, ST7735R_MADCTL);
    u8 madctl = 0x00;
    st7735r_write_data(lcd, &madctl, 1);

    st7735r_write_cmd(lcd, ST7735R_DISPON);

    return 0;
}

static int my_st7735r_probe(struct spi_device *spi) {
    dev_info(&spi->dev, "st7735r: probe called\n");
    
    struct st7735r *lcd;
    lcd = devm_kzalloc(&spi->dev, sizeof(*lcd), GFP_KERNEL);
    if (!lcd)
        return -ENOMEM;

    lcd->spi = spi;
    lcd->dev = &spi->dev;
    lcd->width = 128;
    lcd->height = 160;

    mutex_init(&lcd->lock);
    spi_set_drvdata(spi, lcd);

    /*gpios*/
    lcd->reset_gpio = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_HIGH);
    lcd->dc_gpio = devm_gpiod_get(&spi->dev, "dc", GPIOD_OUT_LOW);
    lcd->backlight_gpio = devm_gpiod_get(&spi->dev, "backlight", GPIOD_OUT_LOW);

    if (IS_ERR(lcd->reset_gpio) || IS_ERR(lcd->dc_gpio) || IS_ERR(lcd->backlight_gpio))
        return -EINVAL;

    /* LCD Controller Reset */
    st7735r_hw_reset(lcd);
    st7735r_init_sequence(lcd);

    /* turn backlight ON */
    st7735r_backlight_on(lcd);
    
    dev_info(&spi->dev, "ST7735R RESET done, backlight enabled\n");

    dev_info(&spi->dev, 
             "SPI device info: mode=%d, max_speed_hz=%d, chip_select=%d\n",
             spi->mode, spi->max_speed_hz, spi->chip_select);
    return 0;
}

static void my_st7735r_remove(struct spi_device *spi) {
    struct st7735r *lcd = spi_get_drvdata(spi);

    st7735r_backlight_off(lcd);
    dev_info(&spi->dev, "st7735r: remove called\n");
}

static int __init st7735r_init(void) {
    return spi_register_driver(&my_st7735r_drv);
}

static void __exit st7735r_exit(void) {
    spi_unregister_driver(&my_st7735r_drv);
}

module_init(st7735r_init);
module_exit(st7735r_exit);


MODULE_AUTHOR("Abhishek");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal Driver for SPI ST7735R LCD Probe");


