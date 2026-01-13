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

/* MADCTL bits */
#define MADCTL_MY        0x80
#define MADCTL_MX        0x40
#define MADCTL_MV        0x20
#define MADCTL_RGB       0x00
#define MADCTL_BGR       0x08


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
    u32 rotation;
    u16 width;
    u16 height;
    u8 pixel_format;
    u8 power_level;     // Power Level Mode of the LCD
    
    // offset for the GRAM - window mapping path based on this panel
    u16 x_offset;
    u16 y_offset;

    // Hardware State - Datasheet influenced
    u8 madctl;
    u8 colmod;

    // Device Runtime State
    bool enabled;
    bool prepared;
    struct mutex lock;
};

static int my_st7735r_probe(struct spi_device *spi);
static void my_st7735r_remove(struct spi_device *spi);
static u8 st7735r_build_madctl(void);
static int st7735r_set_window(struct st7735r *lcd, u16 x0, u16 y0, u16 x1, u16 y1);
static int st7735r_write_data(struct st7735r *lcd, const void *buf, size_t len);
static u8 handle_panel_screen_rotation(struct st7735r *lcd);


static struct spi_driver my_st7735r_drv = {
	.driver = {
		.name = "my-st7735r-lcd",
		.of_match_table = my_st7735r_dt_ids,
	},
    .id_table = my_st7735r_ids,
	.probe = my_st7735r_probe,
	.remove = my_st7735r_remove,
};

static int st7735r_flush(struct st7735r *lcd, u16 x, u16 y, u16 w, u16 h, const void *buf)
{

    dev_info(lcd->dev, "%s: x0: %d y: %d w: %d h:%d\n", __func__, x, y, w, h);

    size_t len = w * h * 2;  //considering its RGB565
    int i, j;
    int ret;
    u16 *b = buf;

    if (!lcd->enabled)
        return -EIO;

    mutex_lock(&lcd->lock);

    ret = st7735r_set_window(lcd, x , y, w, h);

    if (ret) {
        dev_info(lcd->dev, "%s: unable to set window\n", __func__);
        goto out;
    }

    for (i = 0; i < h; i++) {
        for(j = 0; j < w; j++) {

            u16 pixel = b[i * w + j];

            u8 px[2] = {
                pixel >> 8,
                pixel & 0xFF
            };

            st7735r_write_data(lcd, px, 2);
        }
    }


out:
    mutex_unlock(&lcd->lock);
    return ret;

}

static void st7735r_hw_reset(struct st7735r *lcd)
{
    /* active-low reset */
    gpiod_set_value_cansleep(lcd->reset_gpio, 1);
    msleep(20);

    gpiod_set_value_cansleep(lcd->reset_gpio, 0);
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
    dev_info(lcd->dev, "CMD: 0x%02x", cmd);
    return st7735r_spi_write(lcd, &cmd, 1);
}

static int st7735r_write_data(struct st7735r *lcd, const void *buf, size_t len) {
    const u8 *b = buf;

    for(int i = 0; i < min(len, 8UL); i++)
        dev_info(lcd->dev, "%s: DATA[%d]: 0x%02x\n",__func__, i, b[i]);

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

    st7735r_write_cmd(lcd, ST7735R_SLPOUT);
    msleep(150);
    lcd->prepared = true;

    /* configuration */
    /* Pixel Format : RGB565/16-bit */
    st7735r_write_cmd(lcd, ST7735R_COLMOD);
    u8 fmt = 0x05;
    st7735r_write_data(lcd, &fmt, 1);
    
    /* Memory Access Control - 0x00 for RGB */
    st7735r_write_cmd(lcd, ST7735R_MADCTL);
    //u8 madctl = st7735r_build_madctl();
    u8 madctl = handle_panel_screen_rotation(lcd);
    madctl |= MADCTL_RGB;
    lcd->madctl = madctl;
    st7735r_write_data(lcd, &madctl, 1);

    st7735r_write_cmd(lcd, ST7735R_DISPON);
    msleep(20);
    lcd->enabled = true;

    return 0;
}

static int st7735r_set_window(struct st7735r *lcd, u16 x0, u16 y0, u16 x1, u16 y1){
    u8 buf[4];
    dev_info(lcd->dev, "%s: x0: %d y0: %d x1: %d y1:%d\n", __func__, x0, y0, x1, y1);

    /* if MADCTL_MV is set, it means hardware is swapped bw X and Y */
    /* TODO: refine this logic and test it */
    if (lcd->madctl & MADCTL_MV) {
        dev_info(lcd->dev, "%s(): called", __func__);
        dev_info(lcd->dev, "%s(): madctl value 0x%02x", __func__, lcd->madctl);
        swap(x0, y0);
        swap(x1, y1);
    }

    /* converting the w,h parameters into x1,y1 end parameters
     * TODO: find a consistent logic of converting the end parameters (x1, y1) when fed as (w, h)
     * */
    x1 = x0 + x1 - 1;
    y1 = y0 + y1 - 1;

    /* apply panel offsets */
    x0 += lcd->x_offset;
    x1 += lcd->x_offset;
    y0 += lcd->y_offset;
    y1 += lcd->y_offset;

    dev_info(lcd->dev, "%s: (w panel offset) x0: %d y0: %d x1: %d y1:%d\n", __func__, x0, y0, x1, y1);

    /* column address */
    st7735r_write_cmd(lcd, ST7735R_CASET);
    buf[0] = x0 >> 8;
    buf[1] = x0 & 0xFF;
    buf[2] = x1 >> 8;
    buf[3] = x1 & 0xFF;
    st7735r_write_data(lcd, buf, 4);

    /* row address */
    st7735r_write_cmd(lcd, ST7735R_RASET);
    buf[0] = y0 >> 8;
    buf[1] = y0 & 0xFF;
    buf[2] = y1 >> 8;
    buf[3] = y1 & 0xFF;
    st7735r_write_data(lcd, buf, 4);

    st7735r_write_cmd(lcd, ST7735R_RAMWR);

    return 0;

}


/* solid fill - validation */
static void st7735r_fill_screen(struct st7735r *lcd, u16 color)
{
    int x, y;
    u8 px[2] = {
        color >> 8,
        color & 0xFF
    };

    mutex_lock(&lcd->lock);

    st7735r_set_window(lcd, 0, 0, 
                       lcd->width - 1, lcd->height - 1);

    for (y = 0; y < lcd->height; y++) {
        for(x = 0; x < lcd->width; x++) {
            st7735r_write_data(lcd, px, 2);
        }
    }

    mutex_unlock(&lcd->lock);
    
}

static u8 st7735r_build_madctl(void)
{
    u8 madctl = 0;

    /* Normal orientation */
    madctl |= 0;            /* MX = 0 */
    madctl |= 0;            /* MY = 0 */
    madctl |= 0;            /* MV = 0 */

    /* Most ST7735 panels are BGR */
    madctl |= MADCTL_BGR;

    return madctl;
}

static u8 handle_panel_screen_rotation(struct st7735r *lcd)
{
    u8 madctl = MADCTL_RGB;

    switch(lcd->rotation) {
        case 0:
            break;
        case 90:
            madctl |= MADCTL_MV | MADCTL_MX;
            break;
        case 180:
            madctl |= MADCTL_MX | MADCTL_MY;
            break;
        case 270:
            madctl |= MADCTL_MV | MADCTL_MY;
            break;
    }

    return madctl;
}


static int my_st7735r_probe(struct spi_device *spi) {
    dev_info(&spi->dev, "st7735r: probe called\n");
    
    struct st7735r *lcd;
    lcd = devm_kzalloc(&spi->dev, sizeof(*lcd), GFP_KERNEL);
    if (!lcd)
        return -ENOMEM;

    int ret;

    spi->mode = SPI_MODE_0;
    spi->bits_per_word = 8;
    
    ret = spi_setup(spi);
    if (ret) {
        dev_err(&spi->dev, "spi_setup failed: %d\n", ret);
        return ret;
    }

    dev_info(&spi->dev, "SPI Mode: CPOL=%d CPHA=%d CS_HIGH=%d\n",
             !!(spi->mode & SPI_CPOL),
             !!(spi->mode & SPI_CPHA),
             !!(spi->mode & SPI_CS_HIGH));
    dev_info(&spi->dev, 
             "SPI device info: bpw=%d, max_speed_hz=%d, chip_select=%d\n",
             spi->bits_per_word, spi->max_speed_hz, spi->chip_select);

    lcd->spi = spi;
    lcd->dev = &spi->dev;
    lcd->width = 128;
    lcd->height = 160;
    
    /* offset for this panel for GRAM <-> window adjustment, so that (0,0) means top left always */
    lcd->x_offset = 2;
    lcd->y_offset = 1;

    mutex_init(&lcd->lock);
    spi_set_drvdata(spi, lcd);

    /*gpios*/
    lcd->reset_gpio = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW);
    lcd->dc_gpio = devm_gpiod_get(&spi->dev, "dc", GPIOD_OUT_LOW);
    lcd->backlight_gpio = devm_gpiod_get(&spi->dev, "backlight", GPIOD_OUT_LOW);

    if (IS_ERR(lcd->reset_gpio) || IS_ERR(lcd->dc_gpio) || IS_ERR(lcd->backlight_gpio))
        return -EINVAL;

    device_property_read_u32(&spi->dev, "rotation", &lcd->rotation);
    if (lcd->rotation != 0 && lcd->rotation != 90 && lcd->rotation != 180
            && lcd->rotation != 270) {
        lcd->rotation = 0;
    }

    /* LCD Controller Reset */
    dev_dbg(&spi->dev, "calling hw reset");
    st7735r_hw_reset(lcd);
    dev_dbg(&spi->dev, "calling init sequence");
    st7735r_init_sequence(lcd);

    /* turn backlight ON */
    dev_dbg(&spi->dev, "enabling backlight");
    st7735r_backlight_on(lcd);

    st7735r_fill_screen(lcd, 0x0000);

        st7735r_fill_screen(lcd, 0x0000);
    u16 color = 0x001F;
    u8 px[2] = {
        color >> 8,
        color & 0xFF
    };

    /*
    st7735r_set_window(lcd, 127,159,128,160);
    st7735r_write_data(lcd, px, 2);
    st7735r_set_window(lcd, 0,0,1,1);
    st7735r_write_data(lcd, px, 2);
    */

    /*
    u16 dist_buf[30] = { 0xF800, 0x07E0, 0x001F, 0xF800, 0x07E0, 0x001F, 0xF800, 0x07E0, 0x001F, 
        0xF800, 0x07E0, 0x001F, 0xF800, 0x07E0, 0x001F, 0xF800, 0x07E0, 0x001F, 
        0xF800, 0x07E0, 0x001F, 0xF800, 0x07E0, 0x001F, 0xF800, 0x07E0, 0x001F, 
        0xF800, 0x07E0, 0x001F };
    */

    u16 buf[30] = { 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 
        0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 
        0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 
        0xF800, 0xF800, 0xF800 }; 

    st7735r_flush(lcd, 40, 60, 30, 1, (void *) &buf);
    msleep(2000);

    //st7735r_flush(lcd, 0, 0, 30, 1, (void *) &buf);

    dev_info(&spi->dev, "probe done");
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


