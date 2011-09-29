/*
 * alc5623.c  --  alc562[123] ALSA Soc Audio driver
 *
 * Copyright 2008 Realtek Microelectronics
 * Author: flove <flove@realtek.com> Ethan <eku@marvell.com>
 *
 * Copyright 2010 Arnaud Patard <arnaud.patard@rtp-net.org>
 *
 *
 * Based on WM8753.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/module.h>
#include <linux/version.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include <sound/soc.h>
#include <sound/initval.h>
#include <sound/alc5623.h>
#include <sound/alc5623-registers.h>
#include <linux/gpio.h>
#include <linux/seq_file.h>

static int caps_charge = 2000;
module_param(caps_charge, int, 0);
MODULE_PARM_DESC(caps_charge, "ALC5623 cap charge time (msecs)");

/* codec private data */
struct alc5623 {

        struct mutex io_lock;
        struct mutex irq_lock;

        struct device *dev;

        struct snd_soc_codec codec;
        u16 reg_cache[REGISTER_COUNT];
        struct clk*     mclk;                   /* the master clock */
        unsigned int    linevdd_mv;             /* Line Vdd in millivolts */
        unsigned int	linevol_scale;
	enum snd_soc_control_type control_type;
        void *control_data;
        u8 id;
        unsigned int sysclk;
        int                     streams_running;/* Streams running ALC5623_STREAM_* */
        int using_pll;          /* If using PLL */
        unsigned int add_ctrl;
        unsigned int jack_det_ctrl;
        /* original R/W functions */
        unsigned int (*bus_hw_read)(struct snd_soc_codec *codec, unsigned int reg);
        int (*bus_hw_write)(void*,const char*, int);  /* codec->control_data(->struct i2c_client), pdata, datalen */
        void *bus_control_data;                 /* bus control_data to use when calling the original bus fns */
#ifdef CONFIG_GPIOLIB
	struct gpio_chip gpio_chip;
#endif
};


static const struct {
        u16 reg;        /* register */
        u16 val;        /* value */
} alc5623_reg_default[] = {
        {ALC5623_LINE_OUT_VOL                           , 0x8080 }, /* Muted */
        {ALC5623_HP_OUT_VOL                             , 0x3030 }, /* Unmute left and right channels, enable 0 cross detector, 0db volume */
        {ALC5623_MONO_AUX_OUT_VOL                       , 0x0000 }, 
        {ALC5623_AUX_IN_VOL                             	, 0xFF1F }, /* Mute Aux In volume */
        {ALC5623_LINE_IN_VOL                            , 0xFF1F }, /* Mute Line In volume */
        {ALC5623_STEREO_DAC_VOL                         , 0x0909 },
        {ALC5623_MIC_VOL                                , 0x0808 }, /* Mic volume = 0db */
        {ALC5623_MIC_ROUTING_CTRL                       , 0xF0F0 }, /* Mute mic volume to Headphone, Speaker and Mono mixers, Differential mode enabled */
        {ALC5623_ADC_REC_GAIN                           , 0xF58B },
        {ALC5623_ADC_REC_MIXER                          , 0x3F3F }, /* Mic1 as recording sources */
        {ALC5623_OUTPUT_MIXER_CTRL                      , 0x1740 }, 
        {ALC5623_MIC_CTRL                               , 0x0400 }, /* 1.8uA short current det, Bias volt =0.9Avdd, +40db gain boost */
        {ALC5623_DAI_CONTROL                            , 0x8010 }, /* Slave interfase */
        {ALC5623_STEREO_AD_DA_CLK_CTRL           	, 0x066d},
        {ALC5623_PWR_MANAG_ADD1                         , 0xCD33 }, 
        {ALC5623_PWR_MANAG_ADD2                         , 0x3730 }, 
        {ALC5623_PWR_MANAG_ADD3                         , 0xE63A }, 
        {ALC5623_ADD_CTRL_REG                           , 0xb300 }, 
        {ALC5623_GLOBAL_CLK_CTRL_REG                    , 0x0000 }, 
        {ALC5623_PLL_CTRL                               , 0x0000 },
        {ALC5623_GPIO_OUTPUT_PIN_CTRL                   , 0x0002 }, /* Drive High */
        {ALC5623_GPIO_PIN_CONFIG                        , 0x0400 },
        {ALC5623_GPIO_PIN_POLARITY                      , 0x1C0E }, /* All GPIOs high active */
        {ALC5623_GPIO_PIN_STICKY                        , 0x0000 }, /* No sticky ops */
        {ALC5623_GPIO_PIN_WAKEUP                        , 0x0000 }, /* No wakeups */
        {ALC5623_GPIO_PIN_SHARING                       , 0x0000 }, /* None */ 
        {ALC5623_JACK_DET_CTRL                          , 0x0000 }, /*jackdetect off */
        {ALC5623_MISC_CTRL                              , 0x8000 }, /* Slow Vref */
        {ALC5623_PSEUDO_SPATIAL_CTRL  		        , 0x0498 },
        {VIRTUAL_HPL_MIXER                                      , 0x0001 },
        {VIRTUAL_HPR_MIXER                                      , 0x0001 },
        {VIRTUAL_IDX_EQ_BAND0_COEFF                     , 0x1b1b },
        {VIRTUAL_IDX_EQ_BAND0_GAIN                      , 0xf510 },
        {VIRTUAL_IDX_EQ_BAND1_COEFF0            , 0xc10f },
        {VIRTUAL_IDX_EQ_BAND1_COEFF1            , 0x1ef6 },
        {VIRTUAL_IDX_EQ_BAND1_GAIN                      , 0xf65f },
        {VIRTUAL_IDX_EQ_BAND2_COEFF0            , 0xc159 },
        {VIRTUAL_IDX_EQ_BAND2_COEFF1            , 0x1eb3 },
        {VIRTUAL_IDX_EQ_BAND2_GAIN                      , 0xf510 },
        {VIRTUAL_IDX_EQ_BAND3_COEFF0            , 0xc386 },
        {VIRTUAL_IDX_EQ_BAND3_COEFF1            , 0x1cd0 },
        {VIRTUAL_IDX_EQ_BAND3_GAIN                      , 0x0adc },
        {VIRTUAL_IDX_EQ_BAND4_COEFF             , 0x0436 },
        {VIRTUAL_IDX_EQ_BAND4_GAIN                      , 0x2298 },
        {VIRTUAL_IDX_EQ_INPUT_VOL                       , 0x0000 }, /* 0db */
        {VIRTUAL_IDX_EQ_OUTPUT_VOL                      , 0x0001 }, /* 0db */
        {VIRTUAL_IDX_AUTO_VOL_CTRL1             , 0x2710 },
        {VIRTUAL_IDX_AUTO_VOL_CTRL2             , 0x0BB8 },
        {VIRTUAL_IDX_AUTO_VOL_CTRL3             , 0x01F4 },
        {VIRTUAL_IDX_AUTO_VOL_CTRL4             , 0x0190 },
        {VIRTUAL_IDX_AUTO_VOL_CTRL5             , 0x0200 },
        {VIRTUAL_IDX_DIG_INTERNAL                       , 0x9800 }, /* Strong drive */

};

#ifdef CONFIG_GPIOLIB
static inline struct alc5623 *to_alc5623(struct gpio_chip *chip)
{
        return container_of(chip, struct alc5623, gpio_chip);
}

static int alc5623_gpio_direction_in(struct gpio_chip *chip, unsigned offset)
{
        struct alc5623 *alc5623 = to_alc5623(chip);

        return snd_soc_update_bits(&alc5623->codec, ALC5623_GPIO_PIN_CONFIG,
                               ALC5623_GPIO_PIN_GPIO_MASK, ALC5623_GPIO_PIN_GPIO_MASK);
}

static int alc5623_gpio_get(struct gpio_chip *chip, unsigned offset)
{
        printk(KERN_INFO "%s++", __func__);
        struct alc5623 *alc5623 = to_alc5623(chip);
        int ret;

        ret = snd_soc_read(&alc5623->codec, ALC5623_GPIO_PIN_STATUS);
        if (ret < 0)
                return ret;

        if (ret & ALC5623_GPIO_PIN_GPIO_MASK)
                return 1;
        else
                return 0;
}

static int alc5623_gpio_direction_out(struct gpio_chip *chip,
                                     unsigned offset, int value)
{
        struct alc5623 *alc5623 = to_alc5623(chip);

        return snd_soc_update_bits(&alc5623->codec, ALC5623_GPIO_PIN_CONFIG,
                               ALC5623_GPIO_PIN_GPIO_MASK, 0);
}

static void alc5623_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
        printk(KERN_INFO "%s++", __func__);
        struct alc5623 *alc5623 = to_alc5623(chip);

        if (value)
                value = ALC5623_GPIO_PIN_GPIO_MASK;

        snd_soc_update_bits(&alc5623->codec, ALC5623_GPIO_OUTPUT_PIN_CTRL, ALC5623_GPIO_PIN_GPIO_MASK, value);
}

#ifdef CONFIG_DEBUG_FS
static void alc5623_gpio_dbg_show(struct seq_file *s, struct gpio_chip *chip)
{
        struct alc5623 *alc5623 = to_alc5623(chip);

        int gpio = chip->base;
        int reg;
        const char *label;

        /* We report the GPIO even if it's not requested since
         * we're also reporting things like alternate
         * functions which apply even when the GPIO is not in
         * use as a GPIO.
         */
        label = gpiochip_is_requested(chip, 0);
        if (!label)
                label = "Unrequested";
                seq_printf(s, " gpio-%-3d (%-20.20s) ", gpio, label);
                reg = snd_soc_read(&alc5623->codec, ALC5623_GPIO_PIN_STATUS);
        if (reg < 0) {
                dev_err(alc5623->dev,
                        "GPIO control %d read failed: %d\n",
                        gpio, reg);
                seq_printf(s, "\n");
        } else
                seq_printf(s, "(%x)\n", reg & ALC5623_GPIO_PIN_GPIO_MASK);

}
#else
#define alc5623_gpio_dbg_show NULL
#endif

static struct gpio_chip alc5623_gpio_chip = {
        .label                  = "alc5623",
        .owner                  = THIS_MODULE,
        .direction_input        = alc5623_gpio_direction_in,
        .get                    = alc5623_gpio_get,
        .direction_output       = alc5623_gpio_direction_out,
        .set                    = alc5623_gpio_set,
        .dbg_show               = alc5623_gpio_dbg_show,
        .can_sleep              = 1,
};

static void alc5623_init_gpio(struct snd_soc_codec* codec)
{
        struct alc5623* alc5623 = snd_soc_codec_get_drvdata(codec);
        struct alc5623_platform_data* pdata = dev_get_platdata(codec->dev);
        int ret;

        alc5623->gpio_chip = alc5623_gpio_chip;
        alc5623->gpio_chip.ngpio = 1;
        alc5623->gpio_chip.dev = codec->dev;

        if (pdata && pdata->gpio_base)
                alc5623->gpio_chip.base = pdata->gpio_base;
        else
                alc5623->gpio_chip.base = -1;

        ret = gpiochip_add(&alc5623->gpio_chip);
        if (ret != 0)
                dev_err(codec->dev,
                        "Failed to add GPIOs for alc5623: %d\n", ret);
}

static void alc5623_free_gpio(struct snd_soc_codec* codec)
{
        struct alc5623* alc5623 = snd_soc_codec_get_drvdata(codec);
        int ret;

        ret = gpiochip_remove(&alc5623->gpio_chip);
        if (ret != 0)
                dev_err(codec->dev,
                        "Failed to remove GPIOs for alc5623: %d\n", ret);
}
#else
static void alc5623_init_gpio(struct snd_soc_codec* codec)
{
}

static void alc5623_free_gpio(struct snd_soc_codec* codec)
{
}
#endif


static void alc5623_fill_cache(struct snd_soc_codec *codec)
{

#if LINUX_VERSION_CODE == KERNEL_VERSION(2,6,36)
	int i, step = codec->reg_cache_step, size = codec->reg_cache_size;
#else
	int i, step = codec->driver->reg_cache_step, size = codec->driver->reg_cache_size;
#endif
	u16 *cache = codec->reg_cache;

	for (i = 0 ; i < size ; i += step){
		cache[i] = codec->hw_read(codec, i);
	}
}

static inline int alc5623_reset(struct snd_soc_codec *codec)
{
        struct alc5623 *alc5623 = snd_soc_codec_get_drvdata(codec);
        u8 data[3];

        data[0] = ALC5623_RESET;
        data[1] = 0;
        data[2] = 0;
        return alc5623->bus_hw_write(alc5623->bus_control_data, data, 3);
}

static int amp_mixer_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	/* to power-on/off class-d amp generators/speaker */
	/* need to write to 'index-46h' register :        */
	/* so write index num (here 0x46) to reg 0x6a     */
	/* and then 0xffff/0 to reg 0x6c                  */
	snd_soc_write(w->codec, ALC5623_HID_CTRL_INDEX, 0x46);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_write(w->codec, ALC5623_HID_CTRL_DATA, 0xFFFF);
		break;
	case SND_SOC_DAPM_POST_PMD:
		snd_soc_write(w->codec, ALC5623_HID_CTRL_DATA, 0);
		break;
	}

	return 0;
}

/*
 * ALC5623 Controls
 */

static const DECLARE_TLV_DB_SCALE(vol_tlv, -3450, 150, 0);
static const DECLARE_TLV_DB_SCALE(hp_tlv, -4650, 150, 0);
static const DECLARE_TLV_DB_SCALE(adc_rec_tlv, -1650, 150, 0);
static const unsigned int boost_tlv[] = {
	TLV_DB_RANGE_HEAD(3),
	0, 0, TLV_DB_SCALE_ITEM(0, 0, 0),
	1, 1, TLV_DB_SCALE_ITEM(2000, 0, 0),
	2, 2, TLV_DB_SCALE_ITEM(3000, 0, 0),
};
static const DECLARE_TLV_DB_SCALE(dig_tlv, 0, 600, 0);

static const struct snd_kcontrol_new rt5621_vol_snd_controls[] = {
	SOC_DOUBLE_TLV("Speaker Playback Volume",
			ALC5623_LINE_OUT_VOL, 8, 0, 31, 1, hp_tlv),
	SOC_DOUBLE("Speaker Playback Switch",
			ALC5623_LINE_OUT_VOL, 15, 7, 1, 1),
	SOC_DOUBLE_TLV("Headphone Playback Volume",
			ALC5623_HP_OUT_VOL, 8, 0, 31, 1, hp_tlv),
	SOC_DOUBLE("Headphone Playback Switch",
			ALC5623_HP_OUT_VOL, 15, 7, 1, 1),
};

static const struct snd_kcontrol_new rt5622_vol_snd_controls[] = {
	SOC_DOUBLE_TLV("Speaker Playback Volume",
			ALC5623_LINE_OUT_VOL, 8, 0, 31, 1, hp_tlv),
	SOC_DOUBLE("Speaker Playback Switch",
			ALC5623_LINE_OUT_VOL, 15, 7, 1, 1),
	SOC_DOUBLE_TLV("Line Playback Volume",
			ALC5623_HP_OUT_VOL, 8, 0, 31, 1, hp_tlv),
	SOC_DOUBLE("Line Playback Switch",
			ALC5623_HP_OUT_VOL, 15, 7, 1, 1),
};

static const struct snd_kcontrol_new alc5623_vol_snd_controls[] = {
	SOC_DOUBLE_TLV("Line Out Playback Volume",
			ALC5623_LINE_OUT_VOL, 8, 0, 31, 1, hp_tlv),
	SOC_DOUBLE("Line Out Playback Switch",
			ALC5623_LINE_OUT_VOL, 15, 7, 1, 1),
	SOC_DOUBLE_TLV("Headphone Playback Volume",
			ALC5623_HP_OUT_VOL, 8, 0, 31, 1, hp_tlv),
	SOC_DOUBLE("Headphone Playback Switch",
			ALC5623_HP_OUT_VOL, 15, 7, 1, 1),
};

static const struct snd_kcontrol_new alc5623_snd_controls[] = {
	SOC_DOUBLE_TLV("Aux Out Playback Volume",
			ALC5623_MONO_AUX_OUT_VOL, 8, 0, 31, 1, hp_tlv),
	SOC_DOUBLE("Aux Out Playback Switch",
			ALC5623_MONO_AUX_OUT_VOL, 15, 7, 1, 1),
	SOC_DOUBLE_TLV("PCM Playback Volume",
			ALC5623_STEREO_DAC_VOL, 8, 0, 31, 1, vol_tlv),
	SOC_DOUBLE_TLV("Aux In Capture Volume",
			ALC5623_AUX_IN_VOL, 8, 0, 31, 1, vol_tlv),
	SOC_DOUBLE_TLV("Line In Capture Volume",
			ALC5623_LINE_IN_VOL, 8, 0, 31, 1, vol_tlv),
	SOC_SINGLE_TLV("Mic 1 Capture Volume",
			ALC5623_MIC_VOL, 8, 31, 1, vol_tlv),
	SOC_SINGLE_TLV("Mic 2 Capture Volume",
			ALC5623_MIC_VOL, 0, 31, 1, vol_tlv),
	SOC_DOUBLE_TLV("Rec Capture Volume",
			ALC5623_ADC_REC_GAIN, 7, 0, 31, 0, adc_rec_tlv),
	SOC_SINGLE_TLV("Mic 1 Boost Volume",
			ALC5623_MIC_CTRL, 10, 2, 0, boost_tlv),
	SOC_SINGLE_TLV("Mic 2 Boost Volume",
			ALC5623_MIC_CTRL, 8, 2, 0, boost_tlv),
	SOC_SINGLE_TLV("Digital Boost Volume",
			ALC5623_ADD_CTRL_REG, 4, 3, 0, dig_tlv),
};

/*
 * DAPM Controls
 */
/* Left Headphone Mixers */
static const struct snd_kcontrol_new alc5623_hpl_mixer_controls[] = {
SOC_DAPM_SINGLE("LI2HP Playback Switch", ALC5623_LINE_IN_VOL, 15, 1, 1),
SOC_DAPM_SINGLE("AUXI2HP Playback Switch", ALC5623_AUX_IN_VOL, 15, 1, 1),
//SOC_DAPM_SINGLE("MIC12HP Playback Switch", ALC5623_MIC_ROUTING_CTRL, 15, 1, 1),
//SOC_DAPM_SINGLE("MIC22HP Playback Switch", ALC5623_MIC_ROUTING_CTRL, 7, 1, 1),
SOC_DAPM_SINGLE("DAC2HP Playback Switch", ALC5623_STEREO_DAC_VOL, 15, 1, 1),
//SOC_DAPM_SINGLE("LI2HP Playback Switch", VIRTUAL_HPL_MIXER, 4, 1, 0),
//SOC_DAPM_SINGLE("AUXI2HP Playback Switch", VIRTUAL_HPL_MIXER, 3, 1, 0),
SOC_DAPM_SINGLE("MIC12HP Playback Switch", VIRTUAL_HPL_MIXER, 2, 1, 0),
SOC_DAPM_SINGLE("MIC22HP Playback Switch", VIRTUAL_HPL_MIXER, 1, 1, 0),
//SOC_DAPM_SINGLE("DAC2HP Playback Switch", VIRTUAL_HPL_MIXER, 0, 1, 1),
SOC_DAPM_SINGLE("ADCR2HP Playback Switch", ALC5623_ADC_REC_GAIN, 15, 1, 1),
};

/* Right Headphone Mixers */
static const struct snd_kcontrol_new alc5623_hpr_mixer_controls[] = {
SOC_DAPM_SINGLE("LI2HP Playback Switch", VIRTUAL_HPR_MIXER, 4, 1, 0),
SOC_DAPM_SINGLE("AUXI2HP Playback Switch", VIRTUAL_HPR_MIXER, 3, 1, 0),
SOC_DAPM_SINGLE("MIC12HP Playback Switch", VIRTUAL_HPR_MIXER, 2, 1, 0),
SOC_DAPM_SINGLE("MIC22HP Playback Switch", VIRTUAL_HPR_MIXER, 1, 1, 0),
SOC_DAPM_SINGLE("DAC2HP Playback Switch", VIRTUAL_HPR_MIXER, 0, 1, 0),
SOC_DAPM_SINGLE("ADCR2HP Playback Switch", ALC5623_ADC_REC_GAIN, 14, 1, 1),
};

static const struct snd_kcontrol_new alc5623_mono_mixer_controls[] = {
SOC_DAPM_SINGLE("ADC2MONOL Playback Switch", ALC5623_ADC_REC_GAIN, 13, 1, 1),
SOC_DAPM_SINGLE("ADC2MONOR Playback Switch", ALC5623_ADC_REC_GAIN, 12, 1, 1),
SOC_DAPM_SINGLE("LI2MONO Playback Switch", ALC5623_LINE_IN_VOL, 13, 1, 1),
SOC_DAPM_SINGLE("AUXI2MONO Playback Switch", ALC5623_AUX_IN_VOL, 13, 1, 1),
SOC_DAPM_SINGLE("MIC12MONO Playback Switch", ALC5623_MIC_ROUTING_CTRL, 13, 1, 1),
SOC_DAPM_SINGLE("MIC22MONO Playback Switch", ALC5623_MIC_ROUTING_CTRL, 5, 1, 1),
SOC_DAPM_SINGLE("DAC2MONO Playback Switch", ALC5623_STEREO_DAC_VOL, 13, 1, 1),
};

static const struct snd_kcontrol_new alc5623_speaker_mixer_controls[] = {
SOC_DAPM_SINGLE("LI2SPK Playback Switch", ALC5623_LINE_IN_VOL, 14, 1, 1),
SOC_DAPM_SINGLE("AUXI2SPK Playback Switch", ALC5623_AUX_IN_VOL, 14, 1, 1),
SOC_DAPM_SINGLE("MIC12SPK Playback Switch", ALC5623_MIC_ROUTING_CTRL, 14, 1, 1),
SOC_DAPM_SINGLE("MIC22SPK Playback Switch", ALC5623_MIC_ROUTING_CTRL, 6, 1, 1),
SOC_DAPM_SINGLE("DAC2SPK Playback Switch", ALC5623_STEREO_DAC_VOL, 14, 1, 1),
};

/* Left Record Mixer */
static const struct snd_kcontrol_new alc5623_captureL_mixer_controls[] = {
SOC_DAPM_SINGLE("MIC12ADCL Rec Switch", ALC5623_ADC_REC_MIXER, 14, 1, 1),
SOC_DAPM_SINGLE("MIC22ADCL Rec Switch", ALC5623_ADC_REC_MIXER, 13, 1, 1),
SOC_DAPM_SINGLE("LIL2ADCL Rec Switch", ALC5623_ADC_REC_MIXER, 12, 1, 1),
SOC_DAPM_SINGLE("AUXIL2ADCL Rec Switch", ALC5623_ADC_REC_MIXER, 11, 1, 1),
SOC_DAPM_SINGLE("HPL2ADCL Rec Switch", ALC5623_ADC_REC_MIXER, 10, 1, 1),
SOC_DAPM_SINGLE("SPK2ADCL Rec Switch", ALC5623_ADC_REC_MIXER, 9, 1, 1),
SOC_DAPM_SINGLE("AUXO2ADCL Rec Switch", ALC5623_ADC_REC_MIXER, 8, 1, 1),
};

/* Right Record Mixer */
static const struct snd_kcontrol_new alc5623_captureR_mixer_controls[] = {
SOC_DAPM_SINGLE("MIC12ADCR Rec Switch", ALC5623_ADC_REC_MIXER, 6, 1, 1),
SOC_DAPM_SINGLE("MIC22ADCR Rec Switch", ALC5623_ADC_REC_MIXER, 5, 1, 1),
SOC_DAPM_SINGLE("LIR2ADCR Rec Switch", ALC5623_ADC_REC_MIXER, 4, 1, 1),
SOC_DAPM_SINGLE("AUXIR2ADCR Rec Switch", ALC5623_ADC_REC_MIXER, 3, 1, 1),
SOC_DAPM_SINGLE("HPR2ADCR Rec Switch", ALC5623_ADC_REC_MIXER, 2, 1, 1),
SOC_DAPM_SINGLE("SPK2ADCR Rec Switch", ALC5623_ADC_REC_MIXER, 1, 1, 1),
SOC_DAPM_SINGLE("AUXO2ADCR Rec Switch", ALC5623_ADC_REC_MIXER, 0, 1, 1),
};

static const char *alc5623_spk_n_sour_sel[] = {
		"RN/-R", "RP/+R", "LN/-R", "VMID" };
static const char *alc5623_hpl_out_input_sel[] = {
		"VMID", "Left HP Mixer"};
static const char *alc5623_hpr_out_input_sel[] = {
		"VMID", "Right HP Mixer"};
static const char *alc5623_spkout_input_sel[] = {
		"VMID", "Left HP Mixer", "Speaker Mixer", "Mono Mixer"};
static const char *alc5623_aux_out_input_sel[] = {
		"VMID", "HP Mixer", "Speaker Mixer", "Mono Mixer"};

/* auxout output mux */
static const struct soc_enum alc5623_aux_out_input_enum =
SOC_ENUM_SINGLE(ALC5623_OUTPUT_MIXER_CTRL, 6, 4, alc5623_aux_out_input_sel);
static const struct snd_kcontrol_new alc5623_auxout_mux_controls =
SOC_DAPM_ENUM("Route", alc5623_aux_out_input_enum);

/* line output mux */
static const struct soc_enum alc5623_spkout_input_enum =
SOC_ENUM_SINGLE(ALC5623_OUTPUT_MIXER_CTRL, 10, 4, alc5623_spkout_input_sel);
static const struct snd_kcontrol_new alc5623_spkout_mux_controls =
SOC_DAPM_ENUM("Route", alc5623_spkout_input_enum);

/* headphone left output mux */
static const struct soc_enum alc5623_hpl_out_input_enum =
SOC_ENUM_SINGLE(ALC5623_OUTPUT_MIXER_CTRL, 9, 2, alc5623_hpl_out_input_sel);
static const struct snd_kcontrol_new alc5623_hpl_out_mux_controls =
SOC_DAPM_ENUM("Route", alc5623_hpl_out_input_enum);

/* headphone right output mux */
static const struct soc_enum alc5623_hpr_out_input_enum =
SOC_ENUM_SINGLE(ALC5623_OUTPUT_MIXER_CTRL, 8, 2, alc5623_hpr_out_input_sel);
static const struct snd_kcontrol_new alc5623_hpr_out_mux_controls =
SOC_DAPM_ENUM("Route", alc5623_hpr_out_input_enum);

/* line output N select */
static const struct soc_enum alc5623_spk_n_sour_enum =
SOC_ENUM_SINGLE(ALC5623_OUTPUT_MIXER_CTRL, 14, 4, alc5623_spk_n_sour_sel);
static const struct snd_kcontrol_new alc5623_spkoutn_mux_controls =
SOC_DAPM_ENUM("Route", alc5623_spk_n_sour_enum);

/* We have to create a fake left and right HP mixers because
 * the codec only has a single control that is shared by both channels.
 * This makes it impossible to determine the audio path using the current
 * register map, thus we add a new (virtual) register to help determine the
 * audio route within the device.
 */
static int mixer_event (struct snd_soc_dapm_widget *w,
        struct snd_kcontrol *kcontrol, int event)
{
        struct snd_soc_codec *codec = w->codec;

        u16 l, r, lineIn,mic1,mic2,auxIn, pcm;
        printk(KERN_INFO "%s++\n", __FUNCTION__);

        l = snd_soc_read(codec, VIRTUAL_HPL_MIXER);
        r = snd_soc_read(codec, VIRTUAL_HPR_MIXER);
        lineIn = snd_soc_read(codec, ALC5623_LINE_IN_VOL);
        mic1 = snd_soc_read(codec, ALC5623_MIC_ROUTING_CTRL);
        mic2 = snd_soc_read(codec, ALC5623_MIC_ROUTING_CTRL);
        auxIn = snd_soc_read(codec, ALC5623_AUX_IN_VOL);
        pcm = snd_soc_read(codec, ALC5623_STEREO_DAC_VOL);

        if (event & SND_SOC_DAPM_PRE_REG)
                return 0;

        if (l & 0x1 || r & 0x1)
                snd_soc_write(codec, ALC5623_STEREO_DAC_VOL, pcm & 0x7fff);
        else
                snd_soc_write(codec, ALC5623_STEREO_DAC_VOL, pcm | 0x8000);

        if (l & 0x2 || r & 0x2)
                snd_soc_write(codec, ALC5623_MIC_ROUTING_CTRL, mic2 & 0xff7f);
        else
                snd_soc_write(codec, ALC5623_MIC_ROUTING_CTRL, mic2 | 0x0080);

        if (l & 0x4 || r & 0x4)
                snd_soc_write(codec, ALC5623_MIC_ROUTING_CTRL, mic1 & 0x7fff);
        else
                snd_soc_write(codec, ALC5623_MIC_ROUTING_CTRL, mic1 | 0x8000);

        if (l & 0x8 || r & 0x8)
                snd_soc_write(codec, ALC5623_AUX_IN_VOL, auxIn & 0x7fff);
        else
                snd_soc_write(codec, ALC5623_AUX_IN_VOL, auxIn | 0x8000);

        if (l & 0x10 || r & 0x10)
                snd_soc_write(codec, ALC5623_LINE_IN_VOL, lineIn & 0x7fff);
        else
                snd_soc_write(codec, ALC5623_LINE_IN_VOL, lineIn | 0x8000);

        return 0;
}

static const struct snd_soc_dapm_widget alc5623_dapm_widgets[] = {
/* Muxes */
SND_SOC_DAPM_MUX("Aux Out Mux", SND_SOC_NOPM, 0, 0,
	&alc5623_auxout_mux_controls),
SND_SOC_DAPM_MUX("Line Out Mux", SND_SOC_NOPM, 0, 0,
	&alc5623_spkout_mux_controls),
SND_SOC_DAPM_MUX("Left Headphone Mux", SND_SOC_NOPM, 0, 0,
	&alc5623_hpl_out_mux_controls),
SND_SOC_DAPM_MUX("Right Headphone Mux", SND_SOC_NOPM, 0, 0,
	&alc5623_hpr_out_mux_controls),
SND_SOC_DAPM_MUX("Line Out N Mux", SND_SOC_NOPM, 0, 0,
	&alc5623_spkoutn_mux_controls),

/* output mixers */
SND_SOC_DAPM_MIXER_E("Left HP Mixer", ALC5623_PWR_MANAG_ADD2, 5, 0,
	&alc5623_hpl_mixer_controls[0],
	ARRAY_SIZE(alc5623_hpl_mixer_controls),
	mixer_event, SND_SOC_DAPM_POST_REG),
SND_SOC_DAPM_MIXER_E("Right HP Mixer", ALC5623_PWR_MANAG_ADD2, 4, 0,
	&alc5623_hpr_mixer_controls[0],
	ARRAY_SIZE(alc5623_hpr_mixer_controls),
	mixer_event, SND_SOC_DAPM_POST_REG),
SND_SOC_DAPM_MIXER("Mono Mixer", ALC5623_PWR_MANAG_ADD2, 2, 0,
	&alc5623_mono_mixer_controls[0],
	ARRAY_SIZE(alc5623_mono_mixer_controls)),
SND_SOC_DAPM_MIXER("Speaker Mixer", ALC5623_PWR_MANAG_ADD2, 3, 0,
	&alc5623_speaker_mixer_controls[0],
	ARRAY_SIZE(alc5623_speaker_mixer_controls)),

/* input mixers */
SND_SOC_DAPM_MIXER("Left Capture Mixer", ALC5623_PWR_MANAG_ADD2, 1, 0,
	&alc5623_captureL_mixer_controls[0],
	ARRAY_SIZE(alc5623_captureL_mixer_controls)),
SND_SOC_DAPM_MIXER("Right Capture Mixer", ALC5623_PWR_MANAG_ADD2, 0, 0,
	&alc5623_captureR_mixer_controls[0],
	ARRAY_SIZE(alc5623_captureR_mixer_controls)),

SND_SOC_DAPM_DAC("Left DAC", "Left HiFi Playback", ALC5623_PWR_MANAG_ADD2, 9, 0),
SND_SOC_DAPM_DAC("Right DAC", "Right HiFi Playback", ALC5623_PWR_MANAG_ADD2, 8, 0),

SND_SOC_DAPM_MIXER("Aux In Mixer", SND_SOC_NOPM, 0, 0, NULL, 0),
SND_SOC_DAPM_MIXER("Line In Mixer", SND_SOC_NOPM, 0, 0, NULL, 0),
SND_SOC_DAPM_MIXER("HP Mixer", SND_SOC_NOPM, 0, 0, NULL, 0),
SND_SOC_DAPM_MIXER("DAC Mixer", SND_SOC_NOPM, 0, 0, NULL, 0),

SND_SOC_DAPM_ADC("Left ADC", "Left HiFi Capture", ALC5623_PWR_MANAG_ADD2, 7, 0),
SND_SOC_DAPM_ADC("Right ADC", "Right HiFi Capture", ALC5623_PWR_MANAG_ADD2, 6, 0),

SND_SOC_DAPM_PGA("Left Headphone", ALC5623_PWR_MANAG_ADD3, 10, 0, NULL, 0),
SND_SOC_DAPM_PGA("Right Headphone", ALC5623_PWR_MANAG_ADD3, 9, 0, NULL, 0),
SND_SOC_DAPM_PGA("Line Out", ALC5623_PWR_MANAG_ADD3, 12, 0, NULL, 0),
SND_SOC_DAPM_PGA("Left Aux Out", ALC5623_PWR_MANAG_ADD3, 14, 0, NULL, 0),
SND_SOC_DAPM_PGA("Right Aux Out", ALC5623_PWR_MANAG_ADD3, 13, 0, NULL, 0),
SND_SOC_DAPM_PGA("Left Line In", ALC5623_PWR_MANAG_ADD3, 7, 0, NULL, 0),
SND_SOC_DAPM_PGA("Right Line In", ALC5623_PWR_MANAG_ADD3, 6, 0, NULL, 0),
SND_SOC_DAPM_PGA("Left Aux In", ALC5623_PWR_MANAG_ADD3, 5, 0, NULL, 0),
SND_SOC_DAPM_PGA("Right Aux In", ALC5623_PWR_MANAG_ADD3, 4, 0, NULL, 0),
SND_SOC_DAPM_PGA("Mic 1 PGA", ALC5623_PWR_MANAG_ADD3, 3, 0, NULL, 0),
SND_SOC_DAPM_PGA("Mic 2 PGA", ALC5623_PWR_MANAG_ADD3, 2, 0, NULL, 0),
SND_SOC_DAPM_PGA("Mic 1 Pre Amp", ALC5623_PWR_MANAG_ADD3, 1, 0, NULL, 0),
SND_SOC_DAPM_PGA("Mic 2 Pre Amp", ALC5623_PWR_MANAG_ADD3, 0, 0, NULL, 0),

SND_SOC_DAPM_MICBIAS("Mic 1 Bias", ALC5623_PWR_MANAG_ADD1, 11, 0),

SND_SOC_DAPM_SUPPLY("I2S", ALC5623_PWR_MANAG_ADD1, 15, 0, NULL, 0),

SND_SOC_DAPM_OUTPUT("AUXOUTL"),
SND_SOC_DAPM_OUTPUT("AUXOUTR"),
SND_SOC_DAPM_OUTPUT("HPL"),
SND_SOC_DAPM_OUTPUT("HPR"),
SND_SOC_DAPM_OUTPUT("LINEOUT"),
SND_SOC_DAPM_OUTPUT("LINEOUTN"),
SND_SOC_DAPM_INPUT("LINEINL"),
SND_SOC_DAPM_INPUT("LINEINR"),
SND_SOC_DAPM_INPUT("AUXINL"),
SND_SOC_DAPM_INPUT("AUXINR"),
SND_SOC_DAPM_INPUT("MIC1"),
SND_SOC_DAPM_INPUT("MIC2"),
SND_SOC_DAPM_VMID("VMID"),

//SND_SOC_DAPM_OUTPUT("Internal I2S Sink"),
//SND_SOC_DAPM_INPUT("Internal I2S Source"),
};

static const char *alc5623_amp_names[] = {"AB Amp", "D Amp"};
static const struct soc_enum alc5623_amp_enum =
	SOC_ENUM_SINGLE(ALC5623_OUTPUT_MIXER_CTRL, 13, 2, alc5623_amp_names);
static const struct snd_kcontrol_new alc5623_amp_mux_controls =
	SOC_DAPM_ENUM("Route", alc5623_amp_enum);

static const struct snd_soc_dapm_widget alc5623_dapm_amp_widgets[] = {
SND_SOC_DAPM_PGA_E("D Amp", ALC5623_PWR_MANAG_ADD2, 14, 0, NULL, 0,
	amp_mixer_event, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_PGA("AB Amp", ALC5623_PWR_MANAG_ADD2, 15, 0, NULL, 0),
SND_SOC_DAPM_MUX("AB-D Amp Mux", SND_SOC_NOPM, 0, 0,
	&alc5623_amp_mux_controls),
};

static const struct snd_soc_dapm_route audio_map[] = {
	/* virtual mixer - mixes left & right channels */
	{"DAC Mixer", 		NULL,		"Left DAC"},
	{"DAC Mixer", 		NULL,		"Right DAC"},
	{"Line In Mixer", 	NULL,		"Right Line In"},
	{"Line In Mixer", 	NULL,		"Left Line In"},
	{"Aux In Mixer", 	NULL,		"Left Aux In"},
	{"Aux In Mixer", 	NULL,		"Right Aux In"},
	{"HP Mixer", 		NULL, 		"Left HP Mixer"},
        {"HP Mixer", 		NULL, 		"Right HP Mixer"},

        /* left HP mixer */
        {"Left HP Mixer", "LI2HP Playback Switch", "Left Line In"},
        {"Left HP Mixer", "AUXI2HP Playback Switch","Left Aux In"},
        {"Left HP Mixer", "MIC12HP Playback Switch","MIC1"},
        {"Left HP Mixer", "MIC22HP Playback Switch","MIC2"},
        {"Left HP Mixer", "DAC2HP Playback Switch","Left DAC"},
        {"Left HP Mixer", "ADCR2HP Playback Switch","Left Capture Mixer"},

        /* right HP mixer */
        {"Right HP Mixer", "LI2HP Playback Switch", "Right Line In"},
        {"Right HP Mixer", "AUXI2HP Playback Switch","Right Aux In"},
        {"Right HP Mixer", "MIC12HP Playback Switch","MIC1"},
        {"Right HP Mixer", "MIC22HP Playback Switch","MIC2"},
        {"Right HP Mixer", "DAC2HP Playback Switch","Right DAC"},
        {"Right HP Mixer", "ADCR2HP Playback Switch","Right Capture Mixer"},

	/* speaker mixer */
	{"Speaker Mixer", "LI2SPK Playback Switch",	"Line In Mixer"},
	{"Speaker Mixer", "AUXI2SPK Playback Switch",	"Aux In Mixer"},
	{"Speaker Mixer", "MIC12SPK Playback Switch",	"MIC1"},
	{"Speaker Mixer", "MIC22SPK Playback Switch",	"MIC2"},
	{"Speaker Mixer", "DAC2SPK Playback Switch",	"DAC Mixer"},

	/* mono mixer */
	{"Mono Mixer", "ADC2MONOL Playback Switch",	"Left Capture Mixer"},
	{"Mono Mixer", "ADC2MONOR Playback Switch",	"Right Capture Mixer"},
	{"Mono Mixer", "LI2MONO Playback Switch",	"Line In Mixer"},
	{"Mono Mixer", "AUXI2MONO Playback Switch",	"Aux In Mixer"},
	{"Mono Mixer", "MIC12MONO Playback Switch",	"MIC1"},
	{"Mono Mixer", "MIC22MONO Playback Switch",	"MIC2"},
	{"Mono Mixer", "DAC2MONO Playback Switch",	"DAC Mixer"},

	/* Left record mixer */
	{"Left Capture Mixer", "MIC12ADCL Rec Switch","Mic 1 Pre Amp"},
	{"Left Capture Mixer", "MIC22ADCL Rec Switch", "Mic 2 Pre Amp"},
	{"Left Capture Mixer", "LIL2ADCL Rec Switch", "Left Line In"},
	{"Left Capture Mixer", "AUXIL2ADCL Rec Switch", "Left Aux In"},
	{"Left Capture Mixer", "HPL2ADCL Rec Switch", "Left HP Mixer"},
	{"Left Capture Mixer", "SPK2ADCL Rec Switch", "Speaker Mixer"},
	{"Left Capture Mixer", "AUXO2ADCL Rec Switch", "Mono Mixer"},

	/*Right record mixer */
	{"Right Capture Mixer", "MIC12ADCR Rec Switch","Mic 1 Pre Amp"},
	{"Right Capture Mixer", "MIC22ADCR Rec Switch", "Mic 2 Pre Amp"},
	{"Right Capture Mixer", "LIR2ADCR Rec Switch", "Right Line In"},
	{"Right Capture Mixer", "AUXIR2ADCR Rec Switch", "Right Aux In"},
	{"Right Capture Mixer", "HPR2ADCR Rec Switch", "Right HP Mixer"},
	{"Right Capture Mixer", "SPK2ADCR Rec Switch", "Speaker Mixer"},
	{"Right Capture Mixer", "AUXO2ADCR Rec Switch", "Mono Mixer"},

	/* headphone left mux */
	{"Left Headphone Mux", "Left HP Mixer",		"Left HP Mixer"},
//	{"Left Headphone Mux", "Vmid",			"Vmid"},

	/* headphone right mux */
	{"Right Headphone Mux", "Right HP Mixer",	"Right HP Mixer"},
//	{"Right Headphone Mux", "Vmid",			"Vmid"},

	/* speaker out mux */
//	{"Line Out Mux", 	"Vmid",			"Vmid"},
	{"Line Out Mux", 	"Left HP Mixer",	"Left HP Mixer"},
	{"Line Out Mux", 	"Speaker Mixer",	"Speaker Mixer"},
	{"Line Out Mux", 	"Mono Mixer",		"Mono Mixer"},

	{"Line Out N Mux", 	"RN/-R",		"Line Out"},
	{"Line Out N Mux", 	"RP/+R",		"Line Out"},
	{"Line Out N Mux", 	"LN/-R",		"Line Out"},
//	{"Line Out N Mux", 	"Vmid",			"Vmid"},

	/* Mono/Aux Out mux */
//	{"Aux Out Mux", 	"Vmid",			"Vmid"},
	{"Aux Out Mux", 	"HP Mixer",		"HP Mixer"},
	{"Aux Out Mux", 	"Speaker Mixer",	"Speaker Mixer"},
	{"Aux Out Mux", 	"Mono Mixer",		"Mono Mixer"},

	/* output pga */
        {"HPL", 		NULL, 			"Left Headphone"},
        {"Left Headphone", 	NULL, 			"Left Headphone Mux"},
        {"HPR", 		NULL,	 		"Right Headphone"},
        {"Right Headphone", 	NULL,			"Right Headphone Mux"},
	{"LINEOUT", 		NULL, 			"Line Out"},
        {"LINEOUTN", 		NULL, 			"Line Out N Mux"},
	{"AUXOUTL", 		NULL,			"Left Aux Out"},
	{"Left Aux Out",	NULL,			"Aux Out Mux"},
	{"AUXOUTR", 		NULL,			"Right Aux Out"},
	{"Right Aux Out",	NULL,			"Aux Out Mux"},

	/* input pga */
	{"Left Line In", 	NULL,			"LINEINL"},
	{"Right Line In", 	NULL,			"LINEINR"},
	{"Left Aux In", 	NULL,			"AUXINL"},
	{"Right Aux In", 	NULL,			"AUXINR"},
	{"Mic 1 Pre Amp", 	NULL,			"MIC1"},
	{"Mic 2 Pre Amp", 	NULL,			"MIC2"},
	{"Mic 1 PGA", 		NULL,			"Mic 1 Pre Amp"},
	{"Mic 2 PGA", 		NULL,			"Mic 2 Pre Amp"},

	{"Left ADC", 		NULL,		"Left Capture Mixer"},
	{"Left ADC", 		NULL,		"I2S"},
	{"Right ADC", 		NULL,		"Right Capture Mixer"},
	{"Right ADC", 		NULL,		"I2S"},

	{"Left DAC", 		NULL,		"I2S"},
	{"Right DAC", 		NULL,		"I2S"},
};

static const struct snd_soc_dapm_route intercon_spk[] = {
	{"Line Out", NULL,				"Line Out Mux"},
};

static const struct snd_soc_dapm_route intercon_amp_spk[] = {
	{"AB Amp", NULL,				"Line Out Mux"},
	{"D Amp", NULL,					"Line Out Mux"},
	{"AB-D Amp Mux", "AB Amp",			"AB Amp"},
	{"AB-D Amp Mux", "D Amp",			"D Amp"},
	{"Line Out", NULL,				"AB-D Amp Mux"},
};

/* PLL divisors */
struct _pll_div {
	u32 pll_in;
	u32 pll_out;
	u16 regvalue;
};

/* Note : pll code from original alc5623 driver. Not sure of how good it is */
/* useful only for master mode */
static const struct _pll_div codec_master_pll_div[] = {

/*	{  2048000,  8192000,	0x0ea0},
	{  3686400,  8192000,	0x4e27},
	{ 12000000,  8192000,	0x456b},
	{ 13000000,  8192000,	0x495f},
	{ 13100000,  8192000,	0x0320},
	{  2048000,  11289600,	0xf637},
	{  3686400,  11289600,	0x2f22},
	{ 12000000,  11289600,	0x3e2f},
	{ 13000000,  11289600,	0x4d5b},
	{ 13100000,  11289600,	0x363b},
	{  2048000,  16384000,	0x1ea0},
	{  3686400,  16384000,	0x9e27},
	{ 12000000,  16384000,	0x452b},
	{ 13000000,  16384000,	0x542f},
	{ 13100000,  16384000,	0x03a0},
	{  2048000,  16934400,	0xe625},
	{  3686400,  16934400,	0x9126},
	{ 12000000,  16934400,	0x4d2c},
	{ 13000000,  16934400,	0x742f},
	{ 13100000,  16934400,	0x3c27},
	{  2048000,  22579200,	0x2aa0},
	{  3686400,  22579200,	0x2f20},
	{ 12000000,  22579200,	0x7e2f},
	{ 13000000,  22579200,	0x742f},
	{ 13100000,  22579200,	0x3c27},
	{  2048000,  24576000,	0x2ea0},
	{  3686400,  24576000,	0xee27},
	{ 12000000,  24576000,	0x2915},
	{ 13000000,  24576000,	0x772e},
	{ 13100000,  24576000,	0x0d20},
*/
};

static const struct _pll_div codec_slave_pll_div[] = {
/*
	{  1024000,  16384000,  0x3ea0},
	{  1411200,  22579200,	0x3ea0},
	{  1536000,  24576000,	0x3ea0},
	{  2048000,  16384000,  0x1ea0},
	{  2822400,  22579200,	0x1ea0},
	{  3072000,  24576000,	0x1ea0},
*/
};

static int alc5623_dai_startup(struct snd_pcm_substream *substream,
                              struct snd_soc_dai *dai)
{
        int is_play = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
        struct snd_soc_codec *codec = dai->codec;
        struct alc5623 *alc5623 = snd_soc_codec_get_drvdata(codec);

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,36)
        enum snd_soc_bias_level level = codec->dapm.bias_level;
#else
        enum snd_soc_bias_level level = codec->bias_level;
#endif

        dev_dbg(codec->dev, "%s(): is_play:%d, bias level:%d\n", __FUNCTION__,is_play, level);


        /* Power up the required parts */
        if (is_play) {

                /* If codec is fully powered up, do the programming inmediately -
                   Otherwise, it will be deferred until we actually transition
                   to full power up */
                if (level == SND_SOC_BIAS_ON) {

//TODO
                        /* Enable class AB power amplifier and thermal detect */
//                        snd_soc_update_bits(codec,ALC5624_PWR_MANAG_ADD2,0xC000,0xC000);

                        /* Power up speaker amplifier, Headphone out,Mono out, and Stereo DAC */
//                        snd_soc_update_bits(codec,ALC5624_PD_CTRL_STAT,0xC200,0x0000);
                }

                /* Remember we are playing */
                alc5623->streams_running |= ALC5623_STREAM_PLAYBACK;

        } else {

                /* If codec is fully powered up, do the programming inmediately -
                   Otherwise, it will be deferred until we actually transition
                   to full power up */
                if (level == SND_SOC_BIAS_ON) {

                        /* Power up Stereo ADC */
//                        snd_soc_update_bits(codec,ALC5624_PD_CTRL_STAT,0x0100,0x0000);
                }

                /* Remember we are recording */
                alc5623->streams_running |= ALC5623_STREAM_RECORD;

        }

        return 0;
}

static void alc5623_dai_shutdown(struct snd_pcm_substream *substream,
                               struct snd_soc_dai *dai)
{
        int is_play = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
        struct snd_soc_codec *codec = dai->codec;
        struct alc5623 *alc5623 = snd_soc_codec_get_drvdata(codec);

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,36)
        enum snd_soc_bias_level level = codec->dapm.bias_level;
#else
        enum snd_soc_bias_level level = codec->bias_level;
#endif

        dev_dbg(codec->dev, "%s(): is_play:%d, bias level: %d\n", __FUNCTION__,is_play, level);

        if (is_play) {

                /* If codec is not off, do the programming inmediately -
                   Otherwise, it will be deferred until we actually power
                   it up (because we don't have an active clock while off
                   and we need it to access the registers) */
                if (level != SND_SOC_BIAS_OFF) {

                        /* Disable class AB power amplifier and thermal detect */
//                        snd_soc_update_bits(codec,ALC5624_PWR_MANAG_ADD2,0xC000,0x0000);

                        /* Power down speaker amplifier, Headphone out,Mono out, and Stereo DAC */
//                        snd_soc_update_bits(codec,ALC5624_PD_CTRL_STAT,0xC200,0xC200);
                }

                /* Remember we are not playing anymore */
                alc5623->streams_running &= ~ALC5623_STREAM_PLAYBACK;

        } else {

                /* If codec is not off, do the programming inmediately -
                   Otherwise, it will be deferred until we actually power
                   it up (because we don't have an active clock while off
                   and we need it to access the registers) */
                if (level != SND_SOC_BIAS_OFF) {

                        /* Power down Stereo ADC */
//                        snd_soc_update_bits(codec,ALC5624_PD_CTRL_STAT,0x0100,0x0100);
                }

                /* Remember we are not recording anymore */
                alc5623->streams_running &= ~ALC5623_STREAM_RECORD;

        }
}

/* Greatest common divisor */
static unsigned int gcd(unsigned int u, unsigned int v)
{
	int shift;

	/* GCD(0,x) := x */
	if (u == 0 || v == 0)
		return u | v;

	/* Let shift := lg K, where K is the greatest power of 2
	   dividing both u and v. */
	for (shift = 0; ((u | v) & 1) == 0; ++shift) {
		u >>= 1;
		v >>= 1;
	}

	while ((u & 1) == 0)
		u >>= 1;

	/* From here on, u is always odd. */
	do {
		while ((v & 1) == 0)  /* Loop X */
			v >>= 1;

		/* Now u and v are both odd, so diff(u, v) is even.
		   Let u = min(u, v), v = diff(u, v)/2. */
		if (u < v) {
			v -= u;
		} else {
		
			unsigned int diff = u - v;
			u = v;
			v = diff;
			}
			v >>= 1;
	} while (v != 0);

	return u << shift;
}

static int alc5623_set_dai_pll(struct snd_soc_dai *codec_dai, int pll_id,
		int source, unsigned int freq_in, unsigned int freq_out)
{
	int i;
	struct snd_soc_codec *codec = codec_dai->codec;
	struct alc5623 *alc5623 = snd_soc_codec_get_drvdata(codec);
	unsigned int rin, rout, cd, m, finkhz, msel, ksel, psel, fvcodifsel;
	unsigned int fvcosel;
	u16 reg;

        dev_dbg(codec->dev, "%s(): freq_in:%d, freq_out:%d\n", __FUNCTION__,freq_in,freq_out);

	/* Codec sys-clock from MCLK */
	snd_soc_update_bits(codec,ALC5623_GLOBAL_CLK_CTRL_REG,0x8000, 0x0000);

	/* Disable PLL power */
	snd_soc_update_bits(codec, ALC5623_PWR_MANAG_ADD2, ALC5623_PWR_ADD2_PLL, 0);

	/* pll is not used in slave mode */
	reg = snd_soc_read(codec, ALC5623_DAI_CONTROL);
	if (reg & ALC5623_DAI_SDP_SLAVE_MODE)
		return 0;

	alc5623->using_pll = 0;

	/* If input and output frequency are the same, or no freq specified, disable PLL */
	if ((!freq_in || !freq_out) || freq_in == freq_out) {
		dev_dbg(codec->dev, "%s(): disabling PLL\n", __FUNCTION__);
		return 0;
	}
	
	/* Now, given input and output frequencies, we must find the 
	   values for the PLL */
	
	/* 
	  According to datasheet:
	  	  Fout = (MCLK * (N+2)) / ((M + 2) * (K + 2)) 
		  
	  Where:
		K < 8
	    M < 16
	    N < 256
	    P < 2
	 */

	/* First, get the maximum common divider between input and 
	   output frequencies */
	   
	/* Get the greatest common divisor */
	cd = gcd(freq_in,freq_out);
	
	/* Reduce frequencies using it */
	rin = freq_in / cd;
	rout = freq_out / cd;
	
	/* To synthetize fout from fin, we must multiply fin by rout, then 
	   divide by rin. but, in the process, we must try to make sure the 
	   Fvco to stay between 90 ... 100Mhz.
	   
	   Input frequency is first divided by (M+2)*(P+1), then multiplied by N
	   (that is the Fvco), then divided by (K+2) 
	   
	   rout = (N+2)
	   rin  = ((M + 2) * (K + 2) * (P+1))
	   */
	   
	/* Adjust rout and rin to place them in range, if needed */
	if (rout < 2 || rin < 2) {
		rout <<= 1;
		rin <<= 1;
		cd >>= 1;
	}
	   
	/* Check that rout can be achieved */
	if (rout < 2 || rout > 257) {
		dev_err(codec->dev, "N:%d outside of bounds of PLL (1 < N < 258), rin:%d, cd:%d\n", rout,rin,cd);
		return -EINVAL;
	}
	
	/* Get the input frequency in khz, even if losing precision. We will only
	   use it to calculate the approximate VCO frequency and avoid overflows
	   during calculations */
	finkhz = freq_in;
	   
	/* By default, no m selected */
	msel = 0;
	ksel = 0;
	psel = 0;
	fvcodifsel = 0;
	fvcosel = 0;
	   
	/* Iterate through all possible values for M, checking if we can reach
	   the requested ratio */
	for (m = 1; m < 18; m++) {
		unsigned int kdp,fvco,fvcodif;	
		
		/* Check if we can divide without remainder rin by this m value. 
		   This would mean this m value is usable */
		if ((rin % m) != 0) 
			continue;
		
		/* Calculate K * P */
		kdp = rin / m;
		
		/* Filter out impossible K values ... */
		if (kdp > 18 || kdp < 2 || ((kdp & 1) != 0 && kdp > 9)) 
			continue;
		
		/* Calculate PLL Fvco assuming P = 1 */
		fvco = finkhz * rout / m;
		
		/* Calculate distance to optimum fvco */
		fvcodif = (fvco > 95000) ? (fvco - 95000) : (95000 - fvco);
		
		/* If the fvcodif is less than the previously selected one, or no
		   previous selected one... */
		if (!msel || fvcodif < fvcodifsel) {
		
			/* Keep it. Assumes P = 1 */
			msel = m;
			ksel = kdp;
			psel = 1;
			fvcodifsel = fvcodif;
			fvcosel = fvco;
		}
		
		/* If kdp is even, then we can try another configuration with P = 2.
			This means halving the Fvco */
		if (!(kdp & 1) && kdp > 3) {
		
			/* Scale frequency down */
			fvco >>= 1;

			/* Calculate distance to optimum fvco */
			fvcodif = (fvco > 95000) ? (fvco - 95000) : (95000 - fvco);
		
			/* If the fvcodif is less than the previously selected one, or no
				previous selected one... */
			if (!msel || fvcodif < fvcodifsel) {
		
				/* Keep it - P = 2*/
				msel = m;
				ksel = kdp >> 1;
				psel = 2;
				fvcodifsel = fvcodif;
				fvcosel = fvco;
			}
		}
	}

	/* Well, check if there was a valid config */
	if (!msel) {
		dev_err(codec->dev, "No valid M,K,P coefficients gives the needed divider %d for PLL\n", rin);
		return -EINVAL;
	}
	   
	/* Well. we got it. */
	dev_dbg(codec->dev, "Using M:%d, N:%d, K:%d, P:%d coefficients for PLL (div:%d=%d), fvco:%d khz\n", msel,rout,ksel,psel,rin,msel*ksel*psel,fvcosel);

	/* Set the PLL predivider */
	snd_soc_update_bits(codec,ALC5623_GLOBAL_CLK_CTRL_REG, 0x0001,(ksel > 1) ? 0x0001 : 0x0000);
	
	/* set PLL parameter */
 	snd_soc_write(codec,ALC5623_PLL_CTRL,
		( (msel - 2) & 0xF) |			/* M coefficient */
		(((ksel - 2) & 0x7) << 4) |		/* K coefficient */
		((msel == 1) ? 0x80 : 0x00) | 	/* M bypass */
		(((rout - 2) & 0xFF) << 8)		/* N coefficient */
		);

	/* enable PLL power */
	snd_soc_update_bits(codec,ALC5623_PWR_MANAG_ADD2, 0x0100,0x0100);	
	
	/* Codec sys-clock from PLL */
	snd_soc_update_bits(codec,ALC5623_GLOBAL_CLK_CTRL_REG,0x8000,0x8000);

	/* We are using the PLL */
	alc5623->using_pll = 1;

	return 0;
}

struct _coeff_div {
	u32 mclk;
	u32 rate;
	u16 regvalue;
};

/* codec hifi mclk (after PLL) clock divider coefficients */
/* values inspired from column BCLK=32Fs of Appendix A table */
static const struct _coeff_div coeff_div[] = {

        /* 44.1k */
        {11289600, 44100, 0x0A69},
        {22579200, 44100, 0x1A69},

        /* 48k */
        {12288000, 48000, 0x0A69},
        {24576000, 48000, 0x1A69},

};

static int get_coeff(int mclk, int rate)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(coeff_div); i++) {
		if (coeff_div[i].rate == rate && coeff_div[i].mclk == mclk)
			return i;
	}
	return -EINVAL;
}

/*
 * Clock after PLL and dividers
 */
static int alc5623_set_dai_sysclk(struct snd_soc_dai *codec_dai,
		int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	struct alc5623 *alc5623 = snd_soc_codec_get_drvdata(codec);

	switch (freq) {
	case  8192000:
	case 11289600:
	case 12288000:
	case 16384000:
	case 16934400:
	case 18432000:
	case 22579200:
	case 24576000:
		alc5623->sysclk = freq;
		return 0;
	}
	return -EINVAL;
}

static int alc5623_set_dai_fmt(struct snd_soc_dai *codec_dai,
		unsigned int fmt)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	u16 iface = 0;

	/* set master/slave audio interface */
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		iface = ALC5623_DAI_SDP_MASTER_MODE;
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		iface = ALC5623_DAI_SDP_SLAVE_MODE;
		break;
	default:
		return -EINVAL;
	}

	/* interface format */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		iface |= ALC5623_DAI_I2S_DF_I2S;
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		iface |= ALC5623_DAI_I2S_DF_RIGHT;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		iface |= ALC5623_DAI_I2S_DF_LEFT;
		break;
	case SND_SOC_DAIFMT_DSP_A:
		iface |= ALC5623_DAI_I2S_DF_PCM;
		break;
	case SND_SOC_DAIFMT_DSP_B:
		iface |= ALC5623_DAI_I2S_DF_PCM | ALC5623_DAI_I2S_PCM_MODE;
		break;
	default:
		return -EINVAL;
	}

	/* clock inversion */
	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	case SND_SOC_DAIFMT_IB_IF:
		iface |= ALC5623_DAI_MAIN_I2S_BCLK_POL_CTRL;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		iface |= ALC5623_DAI_MAIN_I2S_BCLK_POL_CTRL;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		break;
	default:
		return -EINVAL;
	}

	return snd_soc_write(codec, ALC5623_DAI_CONTROL, iface);
}

static int alc5623_pcm_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
        printk(KERN_INFO "%s++\n", __FUNCTION__);
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
#if LINUX_VERSION_CODE == KERNEL_VERSION(2,6,36)
	struct snd_soc_device *socdev = rtd->socdev;
	struct snd_soc_codec *codec = socdev->card->codec;
#else	
	struct snd_soc_codec *codec = rtd->codec;
#endif
	struct alc5623 *alc5623 = snd_soc_codec_get_drvdata(codec);
	int coeff, rate;
	u16 iface;

	iface = snd_soc_read(codec, ALC5623_DAI_CONTROL);
	iface &= ~ALC5623_DAI_I2S_DL_MASK;

	/* bit size */
	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		iface |= ALC5623_DAI_I2S_DL_16;
		break;
	case SNDRV_PCM_FORMAT_S20_3LE:
		iface |= ALC5623_DAI_I2S_DL_20;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		iface |= ALC5623_DAI_I2S_DL_24;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		iface |= ALC5623_DAI_I2S_DL_32;
		break;
	default:
		return -EINVAL;
	}

	/* set iface & srate */
	snd_soc_write(codec, ALC5623_DAI_CONTROL, iface);
	rate = params_rate(params);
	coeff = get_coeff(alc5623->sysclk, rate);
	WARN_ON(coeff < 0);
	if (coeff < 0)
		return -EINVAL;

	coeff = coeff_div[coeff].regvalue;
	dev_dbg(codec->dev, "%s: sysclk=%d,rate=%d,coeff=0x%04x\n",
		__func__, alc5623->sysclk, rate, coeff);
	snd_soc_write(codec, ALC5623_STEREO_AD_DA_CLK_CTRL, coeff);

	return 0;
}

static int alc5623_digital_mute(struct snd_soc_dai *dai, int mute)
{
        struct snd_soc_codec *codec = dai->codec;
        dev_dbg(codec->dev, "%s(): mute: %d\n", __FUNCTION__,mute);

        if (mute) {
                snd_soc_update_bits(codec, ALC5623_MISC_CTRL, (0x3<<2), (0x3<<2));
        } else {
                snd_soc_update_bits(codec, ALC5623_MISC_CTRL, (0x3<<2), 0);
        }
        return 0;
}

/* write to the alc5623 register space */
static int alc5623_hw_write(void* control_data,const char* data_in_s,int len)
{
        struct alc5623 *alc5623 = control_data;
        u8* data_in = (u8*)data_in_s;
        printk(KERN_INFO "%s(%x)++\n", __FUNCTION__,data_in[0]);

        /* If dealing with the main volume, scale it as requested */
        if (data_in[0] == ALC5623_LINE_OUT_VOL) {

                /* write the volume to the hw regs */
                unsigned int voll,volr;
                u8 data[3];

                /* Get left and right volumes */
                voll = (data_in[1] & 0x1F); /* hi */
                volr = (data_in[2] & 0x1F); /* lo */

                /* Scale them */
                voll = voll * alc5623->linevol_scale / 100;
                volr = volr * alc5623->linevol_scale / 100;

                /* Limit values */
                if (voll > 0x1F) voll = 0x1F;
                if (volr > 0x1F) volr = 0x1F;

                /* Recreate the value */
                data[0] = ALC5623_LINE_OUT_VOL;
                data[1] = (data_in[1] & 0xE0) | voll;
                data[2] = (data_in[2] & 0xE0) | volr;

                /* And write the main speaker volume */
                return alc5623->bus_hw_write(alc5623->bus_control_data,data,3);
        }

        /* If it is a real register, call the original bus access function */
        if (data_in[0] <= ALC5623_VENDOR_ID2)
                return alc5623->bus_hw_write(alc5623->bus_control_data,data_in_s,len);

        /* If dealing with one of the virtual mixers, discard the value, as there
           is no real hw register. This value will also be stored in the register
           cache, and that value will be used instead of the actual value of the
           nonexistant register, when read */
        if (data_in[0] == VIRTUAL_HPL_MIXER ||
                data_in[0] == VIRTUAL_HPR_MIXER)
                return 0;

        /* Dealing with one of the indexed registers. Perform the access */
        if (data_in[0] >= VIRTUAL_IDX_BASE &&
                data_in[0] < REGISTER_COUNT) {

                u8 data[3];
                int ret;

                /* Access the indexed register */
                data[0] = ALC5623_HID_CTRL_INDEX;
                data[1] = 0; /* hi */
                data[2] = data_in[0]-VIRTUAL_IDX_BASE; /* lo */
                if ((ret = alc5623->bus_hw_write(alc5623->bus_control_data,data,3)) < 0)
                        return ret;

                /* Set its value and return */
                data[0] = ALC5623_HID_CTRL_DATA;
                data[1] = data_in[1];
                data[2] = data_in[2];
                return alc5623->bus_hw_write(alc5623->bus_control_data,data,3);
        }

        /* Register does not exist */
        return -EIO;
}

static void alc5623_sync_cache(struct snd_soc_codec *codec)
{

#if LINUX_VERSION_CODE == KERNEL_VERSION(2,6,36)
	int i, step = 2, size = REGISTER_COUNT;
#else
	int i, step = codec->driver->reg_cache_step, size = codec->driver->reg_cache_size;
#endif
	u16 *cache = codec->reg_cache;
	u8 data[3];

	/* Already synchronized, no need to resync again */
	if (!codec->cache_sync)
		return;

	codec->cache_only = 0;

	/* Sync back cached values if they're different from the
	 * hardware default.
	 */
	for (i = 2 ; i < size ; i += step) {
		data[0] = i;
		data[1] = cache[i] >> 8;
		data[2] = cache[i];
		alc5623_hw_write(codec->control_data, data, 3);
	}

	codec->cache_sync = 0;
};

#define ALC5623_ADD2_POWER_EN (ALC5623_PWR_ADD2_VREF \
	| ALC5623_PWR_ADD2_DAC_REF_CIR)

#define ALC5623_ADD3_POWER_EN (ALC5623_PWR_ADD3_MAIN_BIAS \
	| ALC5623_PWR_ADD3_MIC1_BOOST_AD)

#define ALC5623_ADD1_POWER_EN \
	(ALC5623_PWR_ADD1_SHORT_CURR_DET_EN | ALC5623_PWR_ADD1_SOFTGEN_EN \
	| ALC5623_PWR_ADD1_DEPOP_BUF_HP | ALC5623_PWR_ADD1_HP_OUT_AMP \
	| ALC5623_PWR_ADD1_HP_OUT_ENH_AMP)

#define ALC5623_ADD1_POWER_EN_5622 \
	(ALC5623_PWR_ADD1_SHORT_CURR_DET_EN \
	| ALC5623_PWR_ADD1_HP_OUT_AMP)

#if 1
static void enable_power_depop(struct snd_soc_codec *codec)
{
	struct alc5623 *alc5623 = snd_soc_codec_get_drvdata(codec);

	snd_soc_update_bits(codec, ALC5623_PWR_MANAG_ADD3,
				ALC5623_PWR_ADD3_MAIN_BIAS,
				ALC5623_PWR_ADD3_MAIN_BIAS);

	snd_soc_update_bits(codec, ALC5623_PWR_MANAG_ADD1,
				ALC5623_PWR_ADD1_SOFTGEN_EN,
				ALC5623_PWR_ADD1_SOFTGEN_EN);

	snd_soc_update_bits(codec, ALC5623_PWR_MANAG_ADD2,
				ALC5623_ADD2_POWER_EN,
				ALC5623_ADD2_POWER_EN);

}
#else
static void enable_power_depop(struct snd_soc_codec *codec)
{
	struct alc5623 *alc5623 = snd_soc_codec_get_drvdata(codec);

	snd_soc_update_bits(codec, ALC5623_PWR_MANAG_ADD1,
				ALC5623_PWR_ADD1_SOFTGEN_EN,
				ALC5623_PWR_ADD1_SOFTGEN_EN);

	snd_soc_write(codec, ALC5623_PWR_MANAG_ADD3, ALC5623_ADD3_POWER_EN);

	snd_soc_update_bits(codec, ALC5623_MISC_CTRL,
				ALC5623_MISC_HP_DEPOP_MODE2_EN,
				ALC5623_MISC_HP_DEPOP_MODE2_EN);

	msleep(500);

	snd_soc_write(codec, ALC5623_PWR_MANAG_ADD2, ALC5623_ADD2_POWER_EN);

	/* avoid writing '1' into 5622 reserved bits */
	if (alc5623->id == 0x22)
		snd_soc_write(codec, ALC5623_PWR_MANAG_ADD1,
			ALC5623_ADD1_POWER_EN_5622);
	else
		snd_soc_write(codec, ALC5623_PWR_MANAG_ADD1, ALC5623_ADD1_POWER_EN);

	/* disable HP Depop2 */
	snd_soc_update_bits(codec, ALC5623_MISC_CTRL,
				ALC5623_MISC_HP_DEPOP_MODE2_EN,
				0);

}
#endif

static int alc5623_set_bias_level(struct snd_soc_codec *codec,
				      enum snd_soc_bias_level level)
{
	struct alc5623 *alc5623 = snd_soc_codec_get_drvdata(codec);
	
	dev_dbg(codec->dev, "%s++: level: %d\n", __FUNCTION__,level);

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,36)
	if (codec->dapm.bias_level == level)
#else
	if (codec->bias_level == level)
#endif
	{
		return 0;
	}

	switch (level) {
	case SND_SOC_BIAS_ON:
		enable_power_depop(codec);
		break;
	case SND_SOC_BIAS_PREPARE:
//		enable_power_depop(codec);
		break;
	case SND_SOC_BIAS_STANDBY:

		/* If resuming operation from stop ... */
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,36)
		if (codec->dapm.bias_level == SND_SOC_BIAS_OFF)
#else
		if (codec->bias_level == SND_SOC_BIAS_OFF)
#endif
		{
		
			/* Enable the codec MCLK */
			clk_enable(alc5623->mclk);
	
			/* Reset the codec */
			alc5623_reset(codec);
			mdelay(1);
		
			/* Sync registers to cache */
			alc5623_sync_cache(codec);
		}

		/* everything off except vref/vmid, */
		snd_soc_write(codec, ALC5623_PWR_MANAG_ADD2,
				ALC5623_PWR_ADD2_VREF);
		snd_soc_write(codec, ALC5623_PWR_MANAG_ADD3,
				ALC5623_PWR_ADD3_MAIN_BIAS);

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,36)
		if (codec->dapm.bias_level == SND_SOC_BIAS_OFF)
#else
		if (codec->bias_level == SND_SOC_BIAS_OFF)
#endif
		{
			/* Enable fast Vref */
			snd_soc_update_bits(codec, ALC5623_MISC_CTRL, 0x8000, 0x0000);
			
			/* Let it stabilize */
			mdelay(10);
			
			/* Disable fast Vref */
			snd_soc_update_bits(codec, ALC5623_MISC_CTRL, 0x8000, 0x8000);
		}


		break;
	case SND_SOC_BIAS_OFF:

		/* everything off, dac mute, inactive */
		snd_soc_write(codec, ALC5623_PWR_MANAG_ADD2, 0);
		snd_soc_write(codec, ALC5623_PWR_MANAG_ADD3, 0);
		snd_soc_write(codec, ALC5623_PWR_MANAG_ADD1, 0);


		/* Make sure all writes from now on will be resync when resuming */
		codec->cache_sync = 1;
		
		/* Disable the codec MCLK */
		clk_disable(alc5623->mclk);

		break;
	}
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,36)
	codec->dapm.bias_level = level;
#else
	codec->bias_level = level;
#endif
	dev_dbg(codec->dev, "%s--: level: %d\n", __FUNCTION__,level);
	return 0;
}

#define ALC5623_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE \
			| SNDRV_PCM_FMTBIT_S24_LE \
			| SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_ops alc5623_dai_ops = {
                .startup	= alc5623_dai_startup,
                .shutdown	= alc5623_dai_shutdown,
		.hw_params 	= alc5623_pcm_hw_params,
		.digital_mute 	= alc5623_digital_mute,
		.set_fmt 	= alc5623_set_dai_fmt,
		.set_sysclk 	= alc5623_set_dai_sysclk,
		.set_pll 	= alc5623_set_dai_pll,
};

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,36)
static struct snd_soc_dai_driver alc5623_dai = {
#else
struct snd_soc_dai alc5623_dai = {
#endif
	.name = "alc5623-hifi",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rate_min =	8000,
		.rate_max =	48000,
		.rates = SNDRV_PCM_RATE_8000_48000,
		.formats = ALC5623_FORMATS,},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rate_min =	8000,
		.rate_max =	48000,
		.rates = SNDRV_PCM_RATE_8000_48000,
		.formats = ALC5623_FORMATS,},
	.ops = &alc5623_dai_ops,
        .symmetric_rates = 1,
};

#if LINUX_VERSION_CODE == KERNEL_VERSION(2,6,36)
EXPORT_SYMBOL_GPL(alc5623_dai);
#endif

/* Check if a register is volatile or not to forbid or not caching its value */
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,36)
static int alc5623_volatile_register(struct snd_soc_codec *codec,
	unsigned int reg)
#else
static int alc5623_volatile_register(unsigned int reg)
#endif
{
	if (
		reg == ALC5623_GPIO_PIN_STATUS ||
		reg == ALC5623_OVER_CURR_STATUS ||
		reg == ALC5623_HID_CTRL_INDEX ||
		reg == ALC5623_HID_CTRL_DATA)
		return 1;
	return 0;
}

/* read alc5623 hw register */
static unsigned int alc5623_hw_read(struct snd_soc_codec *codec,
        unsigned int reg)
{
        printk(KERN_INFO "%s(%x)++\n", __FUNCTION__, reg);
        struct alc5623 *alc5623 = snd_soc_codec_get_drvdata(codec);

        /* If dealing with the main volume, scale it as requested */
        if (reg == ALC5623_LINE_OUT_VOL) {

                /* Read the volume from the hw regs */
                unsigned int reg2v,voll,volr;

                /* bus_hw_read expects that codec->control_data is pointing to
                   the original control_data.That is the only field accessed. Create
                   a temporary struct with the required data */
                struct snd_soc_codec tmpcodec;
                tmpcodec.control_data = alc5623->bus_control_data;
                reg2v = alc5623->bus_hw_read(&tmpcodec,reg);

                /* Get left and right volumes */
                voll = (reg2v & 0x1F00) >> 8;
                volr = (reg2v & 0x001F);

                /* Inverse scale them */
                voll = voll * 100 / alc5623->linevol_scale;
                volr = volr * 100 / alc5623->linevol_scale;

                /* Limit values */
                if (voll > 0x1F) voll = 0x1F;
                if (volr > 0x1F) volr = 0x1F;

                /* Recreate the value */
                reg2v = (reg2v & 0xE0E0) | (volr) | (voll << 8);

                /* And return the inversely scaled volume */
                return reg2v;
        }

        /* If it is a real register, call the original bus access function */
        if (reg <= ALC5623_VENDOR_ID2) {
                /* bus_hw_read expects that codec->control_data is pointing to
                   the original control_data.That is the only field accessed. Create
                   a temporary struct with the required data */
                struct snd_soc_codec tmpcodec;
                tmpcodec.control_data = alc5623->bus_control_data;
                return alc5623->bus_hw_read(&tmpcodec,reg);
        }

        /* If dealing with one of the virtual mixers, return 0. This fn
           won't be called anymore, as the cache will hold the written
           value, and that value will be used instead of the actual value
           of the nonexistant register */
        if (reg == VIRTUAL_HPL_MIXER ||
                reg == VIRTUAL_HPR_MIXER)
                return 0;

        /* Dealing with one of the indexed registers. Perform the access */
        if (reg >= VIRTUAL_IDX_BASE &&
                reg < REGISTER_COUNT) {

                struct snd_soc_codec tmpcodec;
                u8 data[3];
                int ret;

                /* Access the indexed register */
                data[0] = ALC5623_HID_CTRL_INDEX;
                data[1] = (u8)((reg-VIRTUAL_IDX_BASE) >> 8);
                data[2] = (u8)(reg-VIRTUAL_IDX_BASE);
                if ((ret = alc5623->bus_hw_write(alc5623->bus_control_data,data,3)) < 0)
                        return ret;

                /* Get its value and return it:  */
                /* bus_hw_read expects that codec->control_data is pointing to
                   the original control_data.That is the only field accessed. Create
                   a temporary struct with the required data */
                tmpcodec.control_data = alc5623->bus_control_data;
                return alc5623->bus_hw_read(&tmpcodec,ALC5623_HID_CTRL_DATA);
        }

        /* Register does not exist */
        return -EIO;
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,36)
static int alc5623_suspend(struct snd_soc_codec *codec, pm_message_t mesg)
{
#else
static int alc5623_suspend(struct platform_device *pdev, pm_message_t mesg)
{
	struct snd_soc_device *socdev = platform_get_drvdata(pdev);
	struct snd_soc_codec *codec = socdev->card->codec;
#endif

	alc5623_set_bias_level(codec, SND_SOC_BIAS_OFF);
	return 0;
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,36)
static int alc5623_resume(struct snd_soc_codec *codec)
{
#else
static int alc5623_resume(struct platform_device *pdev)
{
	struct snd_soc_device *socdev = platform_get_drvdata(pdev);
	struct snd_soc_codec *codec = socdev->card->codec;
#endif
	alc5623_set_bias_level(codec, SND_SOC_BIAS_STANDBY);

	/* charge alc5623 caps */
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,36)
	if (codec->dapm.suspend_bias_level == SND_SOC_BIAS_ON) {
		alc5623_set_bias_level(codec, SND_SOC_BIAS_STANDBY);
		codec->dapm.bias_level = SND_SOC_BIAS_ON;
		alc5623_set_bias_level(codec, codec->dapm.bias_level);
	}
#else
	if (codec->suspend_bias_level == SND_SOC_BIAS_ON) {
		alc5623_set_bias_level(codec, SND_SOC_BIAS_STANDBY);
		codec->bias_level = SND_SOC_BIAS_ON;
		alc5623_set_bias_level(codec, codec->bias_level);
	}
#endif
	return 0;
}

static int alc5623_probe(struct snd_soc_codec *codec)
{
        printk(KERN_INFO "%s++", __func__);

	struct alc5623 *alc5623 = snd_soc_codec_get_drvdata(codec);
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,36)	
	struct snd_soc_dapm_context *dapm = &codec->dapm;
#endif
	int ret,i;
	int linebias;
	unsigned long reg;

	ret = snd_soc_codec_set_cache_io(codec, 8, 16, alc5623->control_type);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to set cache I/O: %d\n", ret);
		return ret;
	}
	
        /* Get the original hw R/W functions */
        alc5623->bus_hw_read = codec->hw_read;
        alc5623->bus_hw_write = codec->hw_write;
        alc5623->bus_control_data = codec->control_data;

        /* And install our own functions to be able to provide virtual registers */
        codec->hw_read = alc5623_hw_read;
        codec->hw_write = alc5623_hw_write;
        codec->control_data = alc5623;

	/* Enable the codec MCLK ... Otherwise, we can't read or write registers */
	clk_enable(alc5623->mclk);

	alc5623_reset(codec);
	mdelay(1);

	alc5623_fill_cache(codec);

	/* Modify the default values to properly config the CODEC */
	for (i = 0; i < ARRAY_SIZE(alc5623_reg_default); i++) {
		snd_soc_write(codec,alc5623_reg_default[i].reg,alc5623_reg_default[i].val);
	}
	
	/* Configure amplifier bias voltages based on voltage supplies */
	linebias = alc5623->linevdd_mv >> 1;
	reg = 0;
	
	/* Line Out amplifier bias */
	if (linebias <  1250) {
		reg |= (5 << 12); // SPK class AB bias: 1.0Vdd
	} else
	if (linebias <  1500) {
		reg |= (4 << 12); // SPK class AB bias: 1.25Vdd
	} else
	if (linebias <  1750) {
		reg |= (3 << 12); // SPK class AB bias: 1.5Vdd
	} else
	if (linebias <  2000) {
		reg |= (2 << 12); // SPK class AB bias: 1.75Vdd
	} else
	if (linebias <  2250) {
		reg |= (1 << 12); // SPK class AB bias: 2.0Vdd
	} /* else 0=2.25v bias */
	
	/* Set the amplifier biases */
	snd_soc_update_bits(codec,ALC5623_ADD_CTRL_REG, 0x7000, reg);
	
	/* power on device */
	//alc5623_set_bias_level(codec, SND_SOC_BIAS_STANDBY);

	if (alc5623->add_ctrl) {
		snd_soc_write(codec, ALC5623_ADD_CTRL_REG,
				alc5623->add_ctrl);
	}

	if (alc5623->jack_det_ctrl) {
		snd_soc_write(codec, ALC5623_JACK_DET_CTRL,
				alc5623->jack_det_ctrl);
	}

	/* Disable the codec MCLK */
	clk_disable(alc5623->mclk);

	/* On linux 2.6.38+ we need to register controls here */
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,36)
	switch (alc5623->id) {
	case 0x21:
		snd_soc_add_controls(codec, rt5621_vol_snd_controls,
			ARRAY_SIZE(rt5621_vol_snd_controls));
		break;
	case 0x22:
		snd_soc_add_controls(codec, rt5622_vol_snd_controls,
			ARRAY_SIZE(rt5622_vol_snd_controls));
		break;
	case 0x23:
		snd_soc_add_controls(codec, alc5623_vol_snd_controls,
			ARRAY_SIZE(alc5623_vol_snd_controls));
		break;
	default:
		return -EINVAL;
	}

	snd_soc_add_controls(codec, alc5623_snd_controls,
			ARRAY_SIZE(alc5623_snd_controls));

	snd_soc_dapm_new_controls(dapm, alc5623_dapm_widgets,
					ARRAY_SIZE(alc5623_dapm_widgets));

	/* set up audio path interconnects */
	snd_soc_dapm_add_routes(dapm, audio_map, ARRAY_SIZE(audio_map));

	switch (alc5623->id) {
	case 0x21:
	case 0x22:
		snd_soc_dapm_new_controls(dapm, alc5623_dapm_amp_widgets,
					ARRAY_SIZE(alc5623_dapm_amp_widgets));
		snd_soc_dapm_add_routes(dapm, intercon_amp_spk,
					ARRAY_SIZE(intercon_amp_spk));
		break;
	case 0x23:
		snd_soc_dapm_add_routes(dapm, intercon_spk,
					ARRAY_SIZE(intercon_spk));
		break;
	default:
		return -EINVAL;
	}

        /* make sure to register the dapm widgets */
        snd_soc_dapm_new_widgets(dapm);
#endif

	alc5623_init_gpio(codec);

	return ret;
}

/* power down chip */
static int alc5623_remove(struct snd_soc_codec *codec)
{
        if (codec->control_data)
                alc5623_set_bias_level(codec, SND_SOC_BIAS_OFF);

        alc5623_free_gpio(codec);

        return 0;
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,36)
static struct snd_soc_codec_driver soc_codec_device_alc5623 = {
	.probe = alc5623_probe,
	.remove = alc5623_remove,
	.suspend = alc5623_suspend,
	.resume = alc5623_resume,
        .volatile_register      = alc5623_volatile_register,
	.set_bias_level = alc5623_set_bias_level,
	.reg_cache_size = REGISTER_COUNT,
	.reg_word_size = sizeof(u16),
	.reg_cache_step = 2,
};
#endif

#if LINUX_VERSION_CODE == KERNEL_VERSION(2,6,36)	
static struct snd_soc_codec *alc5623_codec = NULL;
#endif

/*
 * ALC5623 2 wire address is determined by A1 pin
 * state during powerup.
 *    low  = 0x1a
 *    high = 0x1b
 */
static int alc5623_i2c_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
        printk(KERN_INFO "%s++", __func__);
	struct alc5623_platform_data *pdata;
	struct alc5623 *alc5623;
#if LINUX_VERSION_CODE == KERNEL_VERSION(2,6,36)		
	struct snd_soc_codec *codec;
#endif
	struct clk* mclk;

	int ret, vid1, vid2;

	pdata = client->dev.platform_data;
	if (!pdata) {
		dev_err(&client->dev, "Missing platform data\n");	
		return -ENODEV;
	}

	/* Get the MCLK */
	mclk = clk_get(NULL, pdata->mclk);
	if (IS_ERR(mclk)) {
		dev_err(&client->dev, "Unable to get MCLK(%s)\n",pdata->mclk);
		return -ENODEV;
	} 
	
	/* Enable it to be able to access codec registers */
	clk_enable(mclk);
	
	/* Read chip ids */
	vid1 = i2c_smbus_read_word_data(client, ALC5623_VENDOR_ID1);
	if (vid1 < 0) {
		dev_err(&client->dev, "failed to read I2C\n");
		clk_disable(mclk);
		clk_put(mclk);
		return -EIO;
	}
	vid1 = ((vid1 & 0xff) << 8) | (vid1 >> 8);

	vid2 = i2c_smbus_read_byte_data(client, ALC5623_VENDOR_ID2);
	if (vid2 < 0) {
		dev_err(&client->dev, "failed to read I2C\n");
		clk_disable(mclk);
		clk_put(mclk);
		return -EIO;
	}

	/* Disable the clock */
	clk_disable(mclk);

	if ((vid1 != 0x10ec) || (vid2 != id->driver_data)) {
		dev_err(&client->dev, "unknown or wrong codec\n");
		dev_err(&client->dev, "Expected %x:%lx, got %x:%x\n",
				0x10ec, id->driver_data,
				vid1, vid2);
		clk_put(mclk);
		return -ENODEV;
	}

	dev_dbg(&client->dev, "Found codec id : alc56%02x\n", vid2);

	alc5623 = kzalloc(sizeof(struct alc5623), GFP_KERNEL);
	if (alc5623 == NULL){
		clk_put(mclk);
		return -ENOMEM;
	}

	alc5623->mclk = mclk;

	alc5623->dev = &client->dev;

	/* Store the supply voltages used for amplifiers */
	alc5623->linevdd_mv = pdata->linevdd_mv;	/* Line Vdd in millivolts */
	alc5623->linevol_scale = pdata->linevol_scale ? pdata->linevol_scale : 100; /* store maximum volume scale */

	alc5623->add_ctrl = pdata->add_ctrl;
	alc5623->jack_det_ctrl = pdata->jack_det_ctrl;

	alc5623->id = vid2;
	switch (alc5623->id) {
	case 0x21:
		alc5623_dai.name = "alc5621-hifi";
		break;
	case 0x22:
		alc5623_dai.name = "alc5622-hifi";
		break;
	case 0x23:
		alc5623_dai.name = "alc5623-hifi";
		break;
	default:
		kfree(alc5623);
		return -EINVAL;
	}

	i2c_set_clientdata(client, alc5623);
	alc5623->control_data = client;
	alc5623->control_type = SND_SOC_I2C;

#if LINUX_VERSION_CODE == KERNEL_VERSION(2,6,36)	
	/* linux 2.6.36 version setup is quite by hand */

	codec = &alc5623->codec;
	snd_soc_codec_set_drvdata(codec, alc5623);

	mutex_init(&codec->mutex);
	INIT_LIST_HEAD(&codec->dapm_widgets);
	INIT_LIST_HEAD(&codec->dapm_paths);

	codec->name = "ALC5623";
	codec->owner = THIS_MODULE;
	codec->control_data = client;	
//	codec->bias_level = SND_SOC_BIAS_OFF;
	codec->set_bias_level = alc5623_set_bias_level;	
	codec->dai = &alc5623_dai;
	codec->num_dai = 1;
	codec->reg_cache_size = REGISTER_COUNT;
	codec->reg_cache_step = 2;
	codec->reg_cache = &alc5623->reg_cache[0];
	codec->volatile_register = alc5623_volatile_register;	
	codec->cache_sync = 1;
	codec->idle_bias_off = 1;
	
	codec->dev = &client->dev;
	alc5623_dai.dev = &client->dev;
	alc5623_codec = codec; /* so later probe can attach the codec to the card */
	
	/* call the codec probe function */
	ret = alc5623_probe(codec);
	if (ret != 0) {
		dev_err(&client->dev, "Failed to probe codec: %d\n", ret);	
		return ret;
	}

	ret = snd_soc_register_codec(codec);
	if (ret != 0) {
		dev_err(&client->dev, "Failed to register codec: %d\n", ret);	
		kfree(alc5623);
		return ret;
	}

	ret = snd_soc_register_dai(&alc5623_dai);
	if (ret != 0) {
		dev_err(&client->dev, "Failed to register DAI: %d\n", ret);	
		snd_soc_unregister_codec(codec);
		kfree(alc5623);
		return ret;
	}
	
#else
	/* linux 2.6.38 setup is very streamlined :) */
	ret =  snd_soc_register_codec(&client->dev,
		&soc_codec_device_alc5623, &alc5623_dai, 1);
	if (ret != 0) {
		dev_err(&client->dev, "Failed to register codec: %d\n", ret);
		kfree(alc5623);
		return ret;
	}

#endif

	return ret;
}

static int alc5623_i2c_remove(struct i2c_client *client)
{
	struct alc5623 *alc5623 = i2c_get_clientdata(client);

#if LINUX_VERSION_CODE == KERNEL_VERSION(2,6,36)	
	/* linux 2.6.36 version device removal is quite by hand */
	
	alc5623_remove(&alc5623->codec);
	
	snd_soc_unregister_dai(&alc5623_dai);
	snd_soc_unregister_codec(&alc5623->codec);

	alc5623_dai.dev = NULL;
	alc5623_codec = NULL;

#else
	/* linux 2.6.38 device removal is very streamlined :) */
	snd_soc_unregister_codec(&client->dev);
	
#endif
        /* Release clock */
        clk_put(alc5623->mclk);

	kfree(alc5623);
	return 0;
}

static const struct i2c_device_id alc5623_i2c_table[] = {
	{"alc5621", 0x21},
	{"alc5622", 0x22},
	{"alc5623", 0x23},
	{}
};
MODULE_DEVICE_TABLE(i2c, alc5623_i2c_table);

/*  i2c codec control layer */
static struct i2c_driver alc5623_i2c_driver = {
	.driver = {
		.name = "alc562x-codec",
		.owner = THIS_MODULE,
	},
	.probe = alc5623_i2c_probe,
	.remove =  __devexit_p(alc5623_i2c_remove),
	.id_table = alc5623_i2c_table,
};

#if LINUX_VERSION_CODE == KERNEL_VERSION(2,6,36)	
static int alc5623_plat_probe(struct platform_device *pdev)
{
        printk(KERN_INFO "%s++", __func__);
	struct snd_soc_device *socdev = platform_get_drvdata(pdev);
	int ret = 0;

	if (!alc5623_codec) {
		dev_err(&pdev->dev, "I2C client not yet instantiated\n");
		return -ENODEV;
	}
	
	struct alc5623 *alc5623;
	alc5623 = snd_soc_codec_get_drvdata(alc5623_codec);

	/* Associate the codec to the card */
	socdev->card->codec = alc5623_codec;
	//alc5623_codec->card = socdev->card;

        /* Enable the codec MCLK ... Otherwise, we can't read or write registers */
        clk_enable(alc5623->mclk);

	/* Register pcms */
	ret = snd_soc_new_pcms(socdev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to register new PCMs\n");
	}

//        /* call the codec probe function */
//        ret = alc5623_probe(alc5623_codec);
//        if (ret != 0) {
//                dev_err(&pdev->dev, "Failed to probe codec: %d\n", ret);
//                return ret;
//        }
        printk(KERN_INFO "%s boodles", __func__);

	/* Register the controls and widgets */
	switch (alc5623->id) {
	case 0x21:
		snd_soc_add_controls(alc5623_codec, rt5621_vol_snd_controls,
			ARRAY_SIZE(rt5621_vol_snd_controls));
		break;
	case 0x22:
		snd_soc_add_controls(alc5623_codec, rt5622_vol_snd_controls,
			ARRAY_SIZE(rt5622_vol_snd_controls));
		break;
	case 0x23:
		snd_soc_add_controls(alc5623_codec, alc5623_vol_snd_controls,
			ARRAY_SIZE(alc5623_vol_snd_controls));
		break;
	default:
		return -EINVAL;
	}

	snd_soc_add_controls(alc5623_codec, alc5623_snd_controls,
					ARRAY_SIZE(alc5623_snd_controls));

	snd_soc_dapm_new_controls(alc5623_codec, alc5623_dapm_widgets,
					ARRAY_SIZE(alc5623_dapm_widgets));

	/* set up audio path interconnects */
	snd_soc_dapm_add_routes(alc5623_codec, audio_map, ARRAY_SIZE(audio_map));
	
	switch (alc5623->id) {
	case 0x21:
	case 0x22:
		snd_soc_dapm_new_controls(alc5623_codec, alc5623_dapm_amp_widgets,
					ARRAY_SIZE(alc5623_dapm_amp_widgets));
		snd_soc_dapm_add_routes(alc5623_codec, intercon_amp_spk,
					ARRAY_SIZE(intercon_amp_spk));
		break;
	case 0x23:
		snd_soc_dapm_add_routes(alc5623_codec, intercon_spk,
					ARRAY_SIZE(intercon_spk));
		break;
	default:
		return -EINVAL;
	}

	/* make sure to register the dapm widgets */
	snd_soc_dapm_new_widgets(alc5623_codec);
	
        clk_disable(alc5623->mclk);
	
	return ret;
}

/* power down chip */
static int alc5623_plat_remove(struct platform_device *pdev)
{
	struct snd_soc_device *socdev = platform_get_drvdata(pdev);

	/* Release PCMs and DAPM controls */
	snd_soc_free_pcms(socdev);
	snd_soc_dapm_free(socdev);

	return 0;
}

struct snd_soc_codec_device soc_codec_dev_alc5623 = {
	.probe = 	alc5623_plat_probe,
	.remove = 	alc5623_plat_remove,
	.suspend = 	alc5623_suspend,
	.resume =	alc5623_resume,
};
EXPORT_SYMBOL_GPL(soc_codec_dev_alc5623);
#endif

static int __init alc5623_modinit(void)
{
        printk(KERN_INFO "%s++", __func__);
	int ret;

	ret = i2c_add_driver(&alc5623_i2c_driver);
	if (ret != 0) {
		printk(KERN_ERR "%s: can't add i2c driver", __func__);
		return ret;
	}

	return ret;
}
module_init(alc5623_modinit);

static void __exit alc5623_modexit(void)
{
	i2c_del_driver(&alc5623_i2c_driver);
}
module_exit(alc5623_modexit);

MODULE_DESCRIPTION("ASoC alc5621/2/3 driver");
MODULE_AUTHOR("Arnaud Patard <arnaud.patard@rtp-net.org>");
MODULE_LICENSE("GPL");
