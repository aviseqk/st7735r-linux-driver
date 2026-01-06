#define pr_fmt(fmt) "ST7735R: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/of.h>


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

static struct spi_driver my_st7735r_drv = {
	.driver = {
		.name = "my-st7735r-lcd",
		.of_match_table = my_st7735r_dt_ids,
	},
    .id_table = my_st7735r_ids,
	.probe = my_st7735r_probe,
	.remove = my_st7735r_remove,
};


static int my_st7735r_probe(struct spi_device *spi) {
    dev_info(&spi->dev, "st7735r: probe called\n");

    dev_info(&spi->dev, 
             "SPI device info: mode=%d, max_speed_hz=%d, chip_select=%d\n",
             spi->mode, spi->max_speed_hz, spi->chip_select);
    dev_info(&spi->dev, "chip_select only: %u\n", spi->chip_select);
    return 0;
}

static void my_st7735r_remove(struct spi_device *spi) {
    dev_info(&spi->dev, "st7735r: remove called\n");
}

// module_spi_driver(my_st7735r_drv);


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


