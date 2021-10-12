/*
 * ax_lcd_encoder.c - DRM slave encoder for ALINX AN071
 *
 * Copyright (C) 2017 ALINX
 * Author: guowc
 *
 * Based on udl_encoder.c and udl_connector.c, Copyright (C) 2012 Red Hat.
 * Also based on xilinx_drm_dp.c, Copyright (C) 2014 Xilinx, Inc.
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */
 
#include <drm/drmP.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder_slave.h>

#include <linux/device.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>



struct ax_lcd_encoder {
	struct drm_encoder *encoder;
	unsigned int bl_gpio;
	unsigned int rst_gpio;
	unsigned int spi_cs;
	unsigned int spi_scl;
	unsigned int spi_sdi;
	unsigned int spi_sdo;
};
static const struct drm_display_mode lcd_mode = {
	.clock = 33260,
	.hdisplay = 480,// 800,
	.hsync_start = 480 + 10,//800 + 40,
	.hsync_end = 480 + 10 + 2,// 800 + 40 + 128,
	.htotal = 480 + 10 + 2 + 33,//800 + 40 + 128 + 88,
	.vdisplay = 800,//480,
	.vsync_start = 800 + 40,//480 + 10,
	.vsync_end = 800 + 40 + 128,//480 + 10 + 2,
	.vtotal = 800 + 40 + 128 + 88,//480 + 10 + 2 + 33,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static inline struct ax_lcd_encoder *to_ax_lcd_encoder(struct drm_encoder *encoder)
{
	return to_encoder_slave(encoder)->slave_priv;
}

static bool ax_lcd_mode_fixup(struct drm_encoder *encoder,
			   const struct drm_display_mode *mode,
			   struct drm_display_mode *adjusted_mode)
{
	return true;
}

static void ax_lcd_encoder_mode_set(struct drm_encoder *encoder,
				 struct drm_display_mode *mode,
				 struct drm_display_mode *adjusted_mode)
{
}

static void
ax_lcd_encoder_dpms(struct drm_encoder *encoder, int mode)
{
}

static void ax_lcd_encoder_save(struct drm_encoder *encoder)
{
}

static void ax_lcd_encoder_restore(struct drm_encoder *encoder)
{
}

static int ax_lcd_encoder_mode_valid(struct drm_encoder *encoder,
				    struct drm_display_mode *mode)
{
	return MODE_OK;
}

static int ax_lcd_encoder_get_modes(struct drm_encoder *encoder,
				   struct drm_connector *connector)
{
   struct ax_lcd_encoder *ax_lcd = to_ax_lcd_encoder(encoder);
	struct edid *edid;
   struct drm_display_mode *mode;
   int num_modes = 0;
   
   {
         mode = drm_mode_duplicate(encoder->dev, &lcd_mode);
         mode->type |= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
         drm_mode_set_name(mode);
         drm_mode_probed_add(connector, mode);
         num_modes++;

   }   
	return num_modes;
}

static enum drm_connector_status ax_lcd_encoder_detect(struct drm_encoder *encoder,
		     struct drm_connector *connector)
{
   struct ax_lcd_encoder *ax_lcd = to_ax_lcd_encoder(encoder);

   return connector_status_connected; 
}

static struct drm_encoder_slave_funcs ax_lcd_encoder_slave_funcs = {
	.dpms 			= ax_lcd_encoder_dpms,
	.save			= ax_lcd_encoder_save,
	.restore		= ax_lcd_encoder_restore,
	.mode_fixup 	= ax_lcd_mode_fixup,
	.mode_valid		= ax_lcd_encoder_mode_valid,
	.mode_set 		= ax_lcd_encoder_mode_set,
	.detect			= ax_lcd_encoder_detect,
	.get_modes		= ax_lcd_encoder_get_modes,
};

static int ax_lcd_encoder_encoder_init(struct platform_device *pdev,
				      struct drm_device *dev,
				      struct drm_encoder_slave *encoder)
{
	struct ax_lcd_encoder *ax_lcd = platform_get_drvdata(pdev);
	struct device_node *sub_node;

	encoder->slave_priv = ax_lcd;
	encoder->slave_funcs = &ax_lcd_encoder_slave_funcs;

	ax_lcd->encoder = &encoder->base;

	return 0;
}

static void write_command(struct ax_lcd_encoder *fbdev, unsigned char y) // (uchar y,uchar x)
{
  unsigned char i;
  //csb=0;
  gpio_set_value(fbdev->spi_cs, 0);
  //sclb=0;
  gpio_set_value(fbdev->spi_scl, 0);
  //sdi=0;
  gpio_set_value(fbdev->spi_sdi, 0);
  //sclb=1;
  gpio_set_value(fbdev->spi_scl, 1);
  for(i=0;i<8;i++)
	{
	 //sclb=0;
	 gpio_set_value(fbdev->spi_scl, 0);
	  if (y&0x80)
	   {
		 // sdi=1;
		  gpio_set_value(fbdev->spi_sdi, 1);
		}
		  else
		 {
		  //sdi=0;
		  gpio_set_value(fbdev->spi_sdi, 0);
		 }
	  // sclb=1;
	   gpio_set_value(fbdev->spi_scl, 1);
	  y=y<<1;
	}
  // csb=1;
   gpio_set_value(fbdev->spi_cs, 1);
}
//***************************************************************
static void write_data(struct ax_lcd_encoder *fbdev, unsigned char w) // (uchar w, uchar v)
{
  unsigned char i;
//csb=0;
gpio_set_value(fbdev->spi_cs, 0);
//sclb=0;
gpio_set_value(fbdev->spi_scl, 0);
//sdi=1;
gpio_set_value(fbdev->spi_sdi, 1);
//sclb=1;
gpio_set_value(fbdev->spi_scl, 1);
for(i=0;i<8;i++)
 {
  //sclb=0;
  gpio_set_value(fbdev->spi_scl, 0);
	 if (w&0x80)
	 {
		//sdi=1;
		gpio_set_value(fbdev->spi_sdi, 1);
	   }
		  else
		{
		 //sdi=0;
		 gpio_set_value(fbdev->spi_sdi, 0);
		}
  // sclb=1;
   gpio_set_value(fbdev->spi_scl, 1);
   w=w<<1;
	}
   //csb=1;
   gpio_set_value(fbdev->spi_cs, 1);
}

static void vdmafb_init_drm(struct ax_lcd_encoder *fbdev)
{

	//res=1;
	gpio_set_value_cansleep(fbdev->rst_gpio, 1);
	msleep(1);
	//res=0;
	gpio_set_value_cansleep(fbdev->rst_gpio, 0);
	msleep(10);
	//res=1;
	gpio_set_value_cansleep(fbdev->rst_gpio, 1);
	msleep(200);
	//***************************************************************//LCD SETING
	write_command(fbdev, 0xFF);        // Change to Page 1 CMD
	write_data(fbdev, 0xff);
	write_data(fbdev, 0x98);
	write_data(fbdev, 0x06);
	write_data(fbdev, 0x04);
	write_data(fbdev, 0x01);

	write_command(fbdev, 0x08); //Output    SDA
	write_data(fbdev, 0x00); //10

	write_command(fbdev, 0x20);//set DE/VSYNC mode
	write_data(fbdev, 0x00); 	  //01 VSYNC MODE

	write_command(fbdev, 0x21); //DE = 1 Active
	write_data(fbdev, 0x01);//01

	//write_command(0x22);//
	//write_data(0x01);

	write_command(fbdev, 0x30);//Resolution setting 480 X 800
	write_data(fbdev, 0x02); //02

	write_command(fbdev, 0x31); //Inversion setting
	write_data(fbdev, 0x00); //02-2dot

	write_command(fbdev, 0x40); //BT DDVDH DDVDL
	write_data(fbdev, 0x00); //10,14,18 00	2XVCI

	write_command(fbdev, 0x41);
	write_data(fbdev, 0x44);  //avdd +5.2v,avee-5.2v 33

	write_command(fbdev, 0x42);
	write_data(fbdev, 0x00); //VGL=DDVDH+VCIP -DDVDL,VGH=2DDVDL-VCIP

	write_command(fbdev, 0x43);
	write_data(fbdev, 0x80); //SET VGH clamp level +10v

	write_command(fbdev, 0x44);
	write_data(fbdev, 0x86); //SET VGL clamp level -10v

	write_command(fbdev, 0x46);
	write_data(fbdev, 0x34);

	write_command(fbdev, 0x50);//VREG1 for positive Gamma
	write_data(fbdev, 0x94); //A8

	write_command(fbdev, 0x51);//VREG2 for negative Gamma
	write_data(fbdev, 0x94); //A8

	write_command(fbdev, 0x52);//VCOM
	write_data(fbdev, 0x00);

	write_command(fbdev, 0x53); //Forward Flicker
	write_data(fbdev, 0x67); //VCOM

	write_command(fbdev, 0x54); //VCOM
	write_data(fbdev, 0x00);

	write_command(fbdev, 0x55); //Backward Flicker
	write_data(fbdev, 0x67); //VCOM

	write_command(fbdev, 0x60);
	write_data(fbdev, 0x07);

	write_command(fbdev, 0x61);
	write_data(fbdev, 0x04);

	write_command(fbdev, 0x62);
	write_data(fbdev, 0x08);

	write_command(fbdev, 0x63);
	write_data(fbdev, 0x04);

	write_command(fbdev, 0xA0);  //Positive Gamma
	write_data(fbdev, 0x00);
	write_command(fbdev, 0xA1);        //
	write_data(fbdev, 0x0B);
	write_command(fbdev, 0xA2);        //
	write_data(fbdev, 0x13);
	write_command(fbdev, 0xA3);        //
	write_data(fbdev, 0x0C);
	write_command(fbdev, 0xA4);        //
	write_data(fbdev, 0x05);
	write_command(fbdev, 0xA5);        //
	write_data(fbdev, 0x0C);
	write_command(fbdev, 0xA6);        //
	write_data(fbdev, 0x08);
	write_command(fbdev, 0xA7);        //
	write_data(fbdev, 0x06);
	write_command(fbdev, 0xA8);        //
	write_data(fbdev, 0x06);
	write_command(fbdev, 0xA9);        //
	write_data(fbdev, 0x0A);
	write_command(fbdev, 0xAA);        //
	write_data(fbdev, 0x0F);
	write_command(fbdev, 0xAB);        //
	write_data(fbdev, 0x06);
	write_command(fbdev, 0xAC);        //
	write_data(fbdev, 0x12);
	write_command(fbdev, 0xAD);        //
	write_data(fbdev, 0x18);
	write_command(fbdev, 0xAE);        //
	write_data(fbdev, 0x12);
	write_command(fbdev, 0xAF);        //
	write_data(fbdev, 0x0B);

	write_command(fbdev, 0xC0);        //Negative Gamma
	write_data(fbdev, 0x00);
	write_command(fbdev, 0xC1);        //
	write_data(fbdev, 0x0B);
	write_command(fbdev, 0xC2);        //
	write_data(fbdev, 0x13);
	write_command(fbdev, 0xC3);        //
	write_data(fbdev, 0x0C);
	write_command(fbdev, 0xC4);        //
	write_data(fbdev, 0x05);
	write_command(fbdev, 0xC5);        //
	write_data(fbdev, 0x0C);
	write_command(fbdev, 0xC6);        //
	write_data(fbdev, 0x08);
	write_command(fbdev, 0xC7);        //
	write_data(fbdev, 0x06);
	write_command(fbdev, 0xC8);        //
	write_data(fbdev, 0x06);
	write_command(fbdev, 0xC9);        //
	write_data(fbdev, 0x0A);
	write_command(fbdev, 0xCA);        //
	write_data(fbdev, 0x0F);
	write_command(fbdev, 0xCB);        //
	write_data(fbdev, 0x06);
	write_command(fbdev, 0xCC);        //
	write_data(fbdev, 0x12);
	write_command(fbdev, 0xCD);        //
	write_data(fbdev, 0x18);
	write_command(fbdev, 0xCE);        //
	write_data(fbdev, 0x12);
	write_command(fbdev, 0xCF);        //
	write_data(fbdev, 0x0B);

	write_command(fbdev, 0xFF); // Change to Page 6 CMD for GIP timing
	write_data(fbdev, 0xFF);
	write_data(fbdev, 0x98);
	write_data(fbdev, 0x06);
	write_data(fbdev, 0x04);
	write_data(fbdev, 0x06);

	write_command(fbdev, 0x00);        //
	write_data(fbdev, 0x21);
	write_command(fbdev, 0x01);        //
	write_data(fbdev, 0x0A);
	write_command(fbdev, 0x02);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x03);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x04);        //
	write_data(fbdev, 0x32);
	write_command(fbdev, 0x05);        //
	write_data(fbdev, 0x32);
	write_command(fbdev, 0x06);        //
	write_data(fbdev, 0x98);
	write_command(fbdev, 0x07);        //
	write_data(fbdev, 0x06);
	write_command(fbdev, 0x08);        //
	write_data(fbdev, 0x05);
	write_command(fbdev, 0x09);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x0A);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x0B);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x0C);        //
	write_data(fbdev, 0x32);
	write_command(fbdev, 0x0D);        //
	write_data(fbdev, 0x32);
	write_command(fbdev, 0x0E);        //
	write_data(fbdev, 0x01);
	write_command(fbdev, 0x0F);        //
	write_data(fbdev, 0x01);

	write_command(fbdev, 0x10);        //
	write_data(fbdev, 0xF0);
	write_command(fbdev, 0x11);        //
	write_data(fbdev, 0xF0);
	write_command(fbdev, 0x12);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x13);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x14);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x15);        //
	write_data(fbdev, 0x43);
	write_command(fbdev, 0x16);        //
	write_data(fbdev, 0x0B);
	write_command(fbdev, 0x17);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x18);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x19);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x1A);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x1B);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x1C);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x1D);        //
	write_data(fbdev, 0x00);

	write_command(fbdev, 0x20);        //
	write_data(fbdev, 0x01);
	write_command(fbdev, 0x21);        //
	write_data(fbdev, 0x23);
	write_command(fbdev, 0x22);        //
	write_data(fbdev, 0x45);
	write_command(fbdev, 0x23);        //
	write_data(fbdev, 0x67);
	write_command(fbdev, 0x24);        //
	write_data(fbdev, 0x01);
	write_command(fbdev, 0x25);        //
	write_data(fbdev, 0x23);
	write_command(fbdev, 0x26);        //
	write_data(fbdev, 0x45);
	write_command(fbdev, 0x27);        //
	write_data(fbdev, 0x67);

	write_command(fbdev, 0x30);        //
	write_data(fbdev, 0x01);
	write_command(fbdev, 0x31);        //
	write_data(fbdev, 0x11);
	write_command(fbdev, 0x32);        //
	write_data(fbdev, 0x00);
	write_command(fbdev, 0x33);        //
	write_data(fbdev, 0x22);
	write_command(fbdev, 0x34);        //
	write_data(fbdev, 0x22);
	write_command(fbdev, 0x35);        //
	write_data(fbdev, 0xcb);
	write_command(fbdev, 0x36);        //
	write_data(fbdev, 0xda);
	write_command(fbdev, 0x37);        //
	write_data(fbdev, 0xAD);
	write_command(fbdev, 0x38);        //
	write_data(fbdev, 0xbc);
	write_command(fbdev, 0x39);        //
	write_data(fbdev, 0x66);
	write_command(fbdev, 0x3A);        //
	write_data(fbdev, 0x77);
	write_command(fbdev, 0x3B);        //
	write_data(fbdev, 0x22);
	write_command(fbdev, 0x3C);        //
	write_data(fbdev, 0x22);
	write_command(fbdev, 0x3D);        //
	write_data(fbdev, 0x22);
	write_command(fbdev, 0x3E);        //
	write_data(fbdev, 0x22);
	write_command(fbdev, 0x3F);        //
	write_data(fbdev, 0x22);
	write_command(fbdev, 0x40);        //
	write_data(fbdev, 0x22);

	write_command(fbdev, 0x52);        //
	write_data(fbdev, 0x10);

	write_command(fbdev, 0xFF);        // Change to Page 7 CMD for GIP timing
	write_data(fbdev, 0xFF);
	write_data(fbdev, 0x98);
	write_data(fbdev, 0x06);
	write_data(fbdev, 0x04);
	write_data(fbdev, 0x07);

	write_command(fbdev, 0x18);        //
	write_data(fbdev, 0x1d);

	write_command(fbdev, 0x02);        //
	write_data(fbdev, 0x77);

	write_command(fbdev, 0xE1);        //
	write_data(fbdev, 0x79);

	write_command(fbdev, 0xFF);        // Change to Page 0 CMD for Normal command
	write_data(fbdev, 0xFF);
	write_data(fbdev, 0x98);
	write_data(fbdev, 0x06);
	write_data(fbdev, 0x04);
	write_data(fbdev, 0x00);

	write_command(fbdev, 0x36);
	write_data(fbdev, 0x01);

	write_command(fbdev, 0x3A);
	write_data(fbdev, 0x70); //16BIT

	write_command(fbdev, 0x11);
	msleep(120);
	write_command(fbdev, 0x29);
	msleep(25);
}

static int ax_lcd_encoder_probe(struct platform_device *pdev)
{
	int ret = -1;
	struct ax_lcd_encoder *ax_lcd;

	ax_lcd = devm_kzalloc(&pdev->dev, sizeof(*ax_lcd), GFP_KERNEL);
	if (!ax_lcd)
		return -ENOMEM;

	platform_set_drvdata(pdev, ax_lcd);


	ax_lcd->rst_gpio = of_get_named_gpio(pdev->dev.of_node, "rst-gpios", 0);
	if (!gpio_is_valid(ax_lcd->rst_gpio)) {
		dev_err(&pdev->dev, "Failed to get lcd reset gpio\n");
		ret = -ENODEV;
		return ret;
	}

	ret = devm_gpio_request_one(&pdev->dev, ax_lcd->rst_gpio, GPIOF_OUT_INIT_HIGH, "lcd_rst");
	if (ret < 0)
		return ret;

	ax_lcd->spi_cs = of_get_named_gpio(pdev->dev.of_node, "spi_cs", 0);
	if (!gpio_is_valid(ax_lcd->spi_cs)) {
		dev_err(&pdev->dev, "Failed to get spi_cs gpio\n");
		ret = -ENODEV;
		return ret;
	}

	ret = devm_gpio_request_one(&pdev->dev, ax_lcd->spi_cs, GPIOF_OUT_INIT_HIGH, "lcd_spi_cs");
	if (ret < 0)
		return ret;

	ax_lcd->spi_scl = of_get_named_gpio(pdev->dev.of_node, "spi_scl", 0);
	if (!gpio_is_valid(ax_lcd->spi_scl)) {
		dev_err(&pdev->dev, "Failed to get spi_scl gpio\n");
		ret = -ENODEV;
		return ret;
	}

	ret = devm_gpio_request_one(&pdev->dev, ax_lcd->spi_scl, GPIOF_OUT_INIT_HIGH, "lcd_spi_scl");
	if (ret < 0)
		return ret;

	ax_lcd->spi_sdi = of_get_named_gpio(pdev->dev.of_node, "spi_sdi", 0);
	if (!gpio_is_valid(ax_lcd->spi_sdi)) {
		dev_err(&pdev->dev, "Failed to get spi_sdi gpio\n");
		ret = -ENODEV;
		return ret;
	}

	ret = devm_gpio_request_one(&pdev->dev, ax_lcd->spi_sdi, GPIOF_OUT_INIT_HIGH, "lcd_spi_sdi");
	if (ret < 0)
		return ret;
	/*
	msleep(50);
	gpio_set_value_cansleep(rst_gpio, 0);
	msleep(20);
	gpio_set_value_cansleep(rst_gpio, 1);
	msleep(20);
	*/
	vdmafb_init_drm(ax_lcd);
	/* turn on the backlight */
	ax_lcd->bl_gpio = of_get_named_gpio(pdev->dev.of_node, "bl-gpios", 0);
	if (!gpio_is_valid(ax_lcd->bl_gpio)) {
		dev_err(&pdev->dev, "Failed to get lcd backlight gpio\n");
		ret = -ENODEV;
		return ret;
	}

	ret = devm_gpio_request_one(&pdev->dev, ax_lcd->bl_gpio, GPIOF_OUT_INIT_LOW, "lcd_bl");
	if (ret < 0) 
		return ret;

	gpio_set_value_cansleep(ax_lcd->bl_gpio, 1);  // turn on the backlight

	dev_info(&pdev->dev, "Initialized successful.\n");
	return 0;
}

static int ax_lcd_encoder_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id ax_lcd_encoder_of_match[] = {
	{ .compatible = "ax_lcd,drm-encoder", },
	{ /* end of table */ },
};
MODULE_DEVICE_TABLE(of, ax_lcd_encoder_of_match);

static struct drm_platform_encoder_driver ax_lcd_encoder_driver = {
	.platform_driver = {
		.probe			= ax_lcd_encoder_probe,
		.remove			= ax_lcd_encoder_remove,
		.driver			= {
			.owner		= THIS_MODULE,
			.name		= "ax_lcd-drm-enc",
			.of_match_table	= ax_lcd_encoder_of_match,
			},
	},

	.encoder_init = ax_lcd_encoder_encoder_init,
};

static int __init ax_lcd_encoder_init(void)
{
	return platform_driver_register(&ax_lcd_encoder_driver.platform_driver);
}

static void __exit ax_lcd_encoder_exit(void)
{
	platform_driver_unregister(&ax_lcd_encoder_driver.platform_driver);
}

module_init(ax_lcd_encoder_init);
module_exit(ax_lcd_encoder_exit);

MODULE_AUTHOR("");
MODULE_DESCRIPTION("DRM slave encoder for ALINX AN071");
MODULE_LICENSE("GPL v2");
