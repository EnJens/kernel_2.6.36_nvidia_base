/*
 * tegra_soc_alc5623.c  --  SoC audio for tegra (glue logic)
 *
 * (c) 2010-2011 Nvidia Graphics Pvt. Ltd.
 *  http://www.nvidia.com
 * (C) 2011 Eduardo José Tagle <ejtagle@tutopia.com>
 *
 * Copyright 2007 Wolfson Microelectronics PLC.
 * Author: Graeme Gregory
 *         graeme.gregory@wolfsonmicro.com or linux@wolfsonmicro.com
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */
 
/* #define DEBUG */
 
#include "tegra_soc.h"
#include <sound/alc5623-registers.h>
#include <sound/soc-dapm.h>
#include <linux/regulator/consumer.h>

#include <linux/types.h>
#include <sound/jack.h>
#include <linux/switch.h>
#include <mach/gpio.h>
#include <mach/audio.h>
#include <linux/delay.h>

#define DRV_NAME "tegra-snd-alc5623"

static struct platform_device *tegra_snd_device;
static struct regulator* alc5623_reg;

extern struct snd_soc_dai tegra_i2s_dai[];
extern struct snd_soc_dai tegra_spdif_dai;
extern struct snd_soc_dai tegra_generic_codec_dai[];
extern struct snd_soc_platform tegra_soc_platform;
extern struct wired_jack_conf tegra_wired_jack_conf;

/* exported by the ALC5623 codec */
extern struct snd_soc_dai alc5623_dai;
extern struct snd_soc_codec_device soc_codec_dev_alc5623;

/* mclk required for each sampling frequency */
static const struct {
	unsigned int mclk;
	unsigned short srate;
} clocktab[] = {
        /* 8k */
        { 8192000,  8000},
        {12288000,  8000},
        {24576000,  8000},

        /* 11.025k */
        {11289600, 11025},
        {16934400, 11025},
        {22579200, 11025},

        /* 16k */
        {12288000, 16000},
        {16384000, 16000},
        {24576000, 16000},

        /* 22.05k */
        {11289600, 22050},
        {16934400, 22050},
        {22579200, 22050},

        /* 32k */
        {12288000, 32000},
        {16384000, 32000},
        {24576000, 32000},

        /* 44.1k */
        {11289600, 44100},
        {22579200, 44100},

        /* 48k */
        {12288000, 48000},
        {24576000, 48000},
};


/* --------- Digital audio interfase ------------ */

static int tegra_hifi_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	pr_info("%s++", __func__);
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai 	= rtd->dai->codec_dai;
	struct snd_soc_dai *cpu_dai 	= rtd->dai->cpu_dai;
	struct snd_soc_codec *codec = codec_dai->codec;	
	int dai_flag = 0, sys_clk, codec_is_master;
        unsigned int srate, value;
        int i, err;
        enum dac_dap_data_format data_fmt;

        /* Get the requested sampling rate */
        srate = params_rate(params);

	/* I2S <-> DAC <-> DAS <-> DAP <-> CODEC
	   -If DAP is master, codec will be slave */
	codec_is_master = !tegra_das_is_port_master(tegra_audio_codec_type_hifi);
	if (codec_is_master)
                dai_flag |= SND_SOC_DAIFMT_CBS_CFS;
        else
                dai_flag |= SND_SOC_DAIFMT_CBM_CFM;

        data_fmt = tegra_das_get_codec_data_fmt(tegra_audio_codec_type_hifi);

	/* We are supporting DSP and I2s format for now */
	if (data_fmt & dac_dap_data_format_i2s)
		dai_flag |= SND_SOC_DAIFMT_I2S;
	else
		dai_flag |= SND_SOC_DAIFMT_DSP_A;
	
	pr_debug("%s(): format: 0x%08x\n", __FUNCTION__,params_format(params));

	err = snd_soc_dai_set_fmt(codec_dai, dai_flag);
	if (err < 0) {
		pr_err("codec_dai fmt not set \n");
		return err;
	}

	/* Set the CPU dai format. This will also set the clock rate in master mode */
	err = snd_soc_dai_set_fmt(cpu_dai, dai_flag);
	if (err < 0) {
		pr_err("cpu_dai fmt not set \n");
		return err;
	}

        sys_clk = tegra_das_get_mclk_rate();
        err = snd_soc_dai_set_sysclk(codec_dai, 0, sys_clk, SND_SOC_CLOCK_IN);
        if (err < 0) {
                pr_err("codec_dai clock not set\n");
                return err;
        }

	if (codec_is_master) {
		pr_debug("%s(): codec in master mode\n",__FUNCTION__);
		
		/* If using port as slave (=codec as master), then we can use the
		   codec PLL to get the other sampling rates */
		
		/* Try each one until success */
		for (i = 0; i < ARRAY_SIZE(clocktab); i++) {
		
			if (clocktab[i].srate != srate) 
				continue;
				
			if (snd_soc_dai_set_pll(codec_dai, 0, 0, sys_clk, clocktab[i].mclk) >= 0) {
				/* Codec PLL is synthetizing this new clock */
				sys_clk = clocktab[i].mclk;
				break;
			}
		}
		
		if (i >= ARRAY_SIZE(clocktab)) {
			pr_err("%s(): unable to set required MCLK for SYSCLK of %d, sampling rate: %d\n",__FUNCTION__,sys_clk,srate);
			return -EINVAL;
		}
		
	} else {
		pr_debug("%s(): codec in slave mode\n",__FUNCTION__);

		/* Disable codec PLL */
		err = snd_soc_dai_set_pll(codec_dai, 0, 0, sys_clk, sys_clk);
		if (err < 0) {
			pr_err("%s(): unable to disable codec PLL\n",__FUNCTION__);
			return err;
		}
		
		/* Check this sampling rate can be achieved with this sysclk */
		for (i = 0; i < ARRAY_SIZE(clocktab); i++) {
		
			if (clocktab[i].srate != srate) 
				continue;
				
			if (sys_clk == clocktab[i].mclk)
				break;
		}
		
		if (i >= ARRAY_SIZE(clocktab)) {
			pr_err("%s(): unable to get required %d hz sampling rate of %d hz SYSCLK\n",__FUNCTION__,srate,sys_clk);
			return -EINVAL;
		}
	}

	/* Set CODEC sysclk */
	err = snd_soc_dai_set_sysclk(codec_dai, 0, sys_clk, SND_SOC_CLOCK_IN);
	if (err < 0) {
		pr_err("codec_dai clock not set\n");
		return err;
	}
	
	return 0;
}

static int tegra_voice_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	pr_info("%s++", __func__);
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai 	= rtd->dai->codec_dai;
	struct snd_soc_dai *cpu_dai 	= rtd->dai->cpu_dai;
	int dai_flag = 0, sys_clk;
	int err;

	/* Get DAS dataformat and master flag */
	int codec_is_master = !tegra_das_is_port_master(tegra_audio_codec_type_bluetooth);
	enum dac_dap_data_format data_fmt = tegra_das_get_codec_data_fmt(tegra_audio_codec_type_bluetooth);

	/* We are supporting DSP and I2s format for now */
	if (data_fmt & dac_dap_data_format_dsp)
		dai_flag |= SND_SOC_DAIFMT_DSP_A;
	else
		dai_flag |= SND_SOC_DAIFMT_I2S;

	if (codec_is_master)
		dai_flag |= SND_SOC_DAIFMT_CBM_CFM; /* codec is master */
	else
		dai_flag |= SND_SOC_DAIFMT_CBS_CFS;


	pr_debug("%s(): format: 0x%08x\n", __FUNCTION__,params_format(params));

	/* Set the CPU dai format. This will also set the clock rate in master mode */
	err = snd_soc_dai_set_fmt(cpu_dai, dai_flag);
	if (err < 0) {
		pr_err("cpu_dai fmt not set \n");
		return err;
	}

	/* Bluetooth Codec is always slave here */
	err = snd_soc_dai_set_fmt(codec_dai, dai_flag);
	if (err < 0) {
		pr_err("codec_dai fmt not set \n");
		return err;
	}
	
	/* Get system clock */
	sys_clk = tegra_das_get_mclk_rate();

	/* Set CPU sysclock as the same - in Tegra, seems to be a NOP */
	err = snd_soc_dai_set_sysclk(cpu_dai, 0, sys_clk, SND_SOC_CLOCK_IN);
	if (err < 0) {
		pr_err("cpu_dai clock not set\n");
		return err;
	}
	
	/* Set CODEC sysclk */
	err = snd_soc_dai_set_sysclk(codec_dai, 0, sys_clk, SND_SOC_CLOCK_IN);
	if (err < 0) {
		pr_err("cpu_dai clock not set\n");
		return err;
	}
	
	return 0;
}

static int tegra_spdif_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	pr_debug("%s(): format: 0x%08x\n", __FUNCTION__,params_format(params));
	return 0;
}

static int tegra_codec_startup(struct snd_pcm_substream *substream)
{
	pr_info("%s++", __func__);
	tegra_das_power_mode(true);

	return 0;
}

static void tegra_codec_shutdown(struct snd_pcm_substream *substream)
{
	pr_info("%s++", __func__);
	tegra_das_power_mode(false);
}

static int tegra_soc_suspend_pre(struct platform_device *pdev, pm_message_t state)
{
	pr_info("%s++", __func__);
	tegra_jack_suspend();
	return 0;
}

static int tegra_soc_suspend_post(struct platform_device *pdev, pm_message_t state)
{
	pr_info("%s++", __func__);
	tegra_das_disable_mclk();

	return 0;
}

static int tegra_soc_resume_pre(struct platform_device *pdev)
{
	pr_info("%s++", __func__);
	tegra_das_enable_mclk();

	return 0;
}

static int tegra_soc_resume_post(struct platform_device *pdev)
{
	pr_info("%s++", __func__);
	tegra_jack_resume();
	return 0;
}

static struct snd_soc_ops tegra_hifi_ops = {
	.hw_params = tegra_hifi_hw_params,
	.startup = tegra_codec_startup,
	.shutdown = tegra_codec_shutdown,
};

static struct snd_soc_ops tegra_voice_ops = {
	.hw_params = tegra_voice_hw_params,
	.startup = tegra_codec_startup,
	.shutdown = tegra_codec_shutdown,
};

static struct snd_soc_ops tegra_spdif_ops = {
	.hw_params = tegra_spdif_hw_params,
};

/* ------- Tegra audio routing using DAS -------- */

void tegra_ext_control(struct snd_soc_codec *codec, int new_con)
{
        struct tegra_audio_data *audio_data = codec->socdev->codec_data;

        pr_info("%s: new_con 0x%X #####################\n", __func__, new_con);

        /* Disconnect old codec routes and connect new routes*/
        if (new_con & TEGRA_HEADPHONE)
                snd_soc_dapm_enable_pin(codec, "Headphone Jack");
        else
                snd_soc_dapm_disable_pin(codec, "Headphone Jack");

        if (new_con & (TEGRA_SPK | TEGRA_EAR_SPK))
                snd_soc_dapm_enable_pin(codec, "Internal Speaker");
        else
                snd_soc_dapm_disable_pin(codec, "Internal Speaker");

        if (new_con & TEGRA_INT_MIC)
                snd_soc_dapm_enable_pin(codec, "MIC1");
        else
                snd_soc_dapm_disable_pin(codec, "MIC1");

        /* signal a DAPM event */
        snd_soc_dapm_sync(codec);
        audio_data->codec_con = new_con;

}

static int tegra_dapm_event_int_spk(struct snd_soc_dapm_widget* w,
                                    struct snd_kcontrol* k, int event)
{
	pr_info("%s++", __func__);
        if (tegra_wired_jack_conf.en_spkr != -1) {
                if (tegra_wired_jack_conf.amp_reg) {
                        if (SND_SOC_DAPM_EVENT_ON(event) &&
                                !tegra_wired_jack_conf.amp_reg_enabled) {
                                regulator_enable(tegra_wired_jack_conf.amp_reg);
                                tegra_wired_jack_conf.amp_reg_enabled = 1;
                        }
                        else if (!SND_SOC_DAPM_EVENT_ON(event) &&
                                tegra_wired_jack_conf.amp_reg_enabled) {
                                regulator_disable(tegra_wired_jack_conf.amp_reg);
                                tegra_wired_jack_conf.amp_reg_enabled = 0;
                        }
                }

                gpio_set_value_cansleep(tegra_wired_jack_conf.en_spkr,
                        SND_SOC_DAPM_EVENT_ON(event) ? 1 : 0);

                /* the amplifier needs 5ms to enable. wait 5ms after
                 * gpio EN triggered */
                if (SND_SOC_DAPM_EVENT_ON(event))
                        msleep(5);
        }

        return 0;
}

static int tegra_dapm_event_int_mic(struct snd_soc_dapm_widget* w,
                                    struct snd_kcontrol* k, int event)
{
	pr_info("%s++", __func__);
        if (tegra_wired_jack_conf.en_mic_int != -1)
                gpio_set_value_cansleep(tegra_wired_jack_conf.en_mic_int,
                        SND_SOC_DAPM_EVENT_ON(event) ? 1 : 0);

        if (tegra_wired_jack_conf.en_mic_ext != -1)
                gpio_set_value_cansleep(tegra_wired_jack_conf.en_mic_ext,
                        SND_SOC_DAPM_EVENT_ON(event) ? 0 : 1);

        return 0;
}

static int tegra_dapm_event_ext_mic(struct snd_soc_dapm_widget* w,
                                    struct snd_kcontrol* k, int event)
{
	pr_info("%s++", __func__);
        if (tegra_wired_jack_conf.en_mic_ext != -1)
                gpio_set_value_cansleep(tegra_wired_jack_conf.en_mic_ext,
                        SND_SOC_DAPM_EVENT_ON(event) ? 1 : 0);

        if (tegra_wired_jack_conf.en_mic_int != -1)
                gpio_set_value_cansleep(tegra_wired_jack_conf.en_mic_int,
                        SND_SOC_DAPM_EVENT_ON(event) ? 0 : 1);

        return 0;
}

/*tegra machine dapm widgets */
static const struct snd_soc_dapm_widget tegra_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Internal Speaker", tegra_dapm_event_int_spk),
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Internal Mic", tegra_dapm_event_int_mic),
};

/* Tegra machine audio map (connections to the codec pins) */
static const struct snd_soc_dapm_route audio_map[] = {
	{"Headphone Jack", NULL, "HPL"},
	{"Headphone Jack", NULL, "HPR"},
        {"Internal Speaker", NULL, "AUXOUTL"},
        {"Internal Speaker", NULL, "AUXOUTR"},
        {"Mic 1 Bias", NULL, "Internal Mic"},
        {"MIC1", NULL, "Mic 1 Bias"},
};

static int tegra_codec_init(struct snd_soc_codec *codec)
{
	pr_info("%s++", __func__);
	struct tegra_audio_data* audio_data = codec->socdev->codec_data;

	int ret = 0;

	if (!audio_data->init_done) {
		
                audio_data->dap_mclk = tegra_das_get_dap_mclk();
                if (!audio_data->dap_mclk) {
                        pr_err("Failed to get dap mclk \n");
                        ret = -ENODEV;
                        goto alc5623_init_fail;
                }
		
		ret = tegra_das_open();
                if (ret) {
                        pr_err(" Failed get dap mclk \n");
                        ret = -ENODEV;
                        goto alc5623_init_fail;
                }

                ret = tegra_das_enable_mclk();
                if (ret) {
                        pr_err(" Failed to enable dap mclk \n");
                        ret = -ENODEV;
                        goto alc5623_init_fail;
                }

		/* Add tegra specific widgets */
		snd_soc_dapm_new_controls(codec, tegra_dapm_widgets,
					ARRAY_SIZE(tegra_dapm_widgets));

		/* Set up tegra specific audio path audio_map */
		snd_soc_dapm_add_routes(codec, audio_map,
					ARRAY_SIZE(audio_map));

                /* Set endpoints to not connected */
                snd_soc_dapm_nc_pin(codec, "LINEOUT");
                snd_soc_dapm_nc_pin(codec, "LINEOUTN");
                snd_soc_dapm_nc_pin(codec, "LINEINL");
                snd_soc_dapm_nc_pin(codec, "LINEINR");
                snd_soc_dapm_nc_pin(codec, "AUXINL");
                snd_soc_dapm_nc_pin(codec, "AUXINR");
                snd_soc_dapm_nc_pin(codec, "MIC2");

                /* Set endpoints to default off mode */
                snd_soc_dapm_enable_pin(codec, "Internal Speaker");
                snd_soc_dapm_enable_pin(codec, "Internal Mic");
                snd_soc_dapm_disable_pin(codec, "Headphone Jack");

                ret = snd_soc_dapm_sync(codec);
                if (ret) {
                        pr_err("Failed to sync\n");
                        return ret;
                }

		/* Add jack detection */
                ret = tegra_jack_init(codec);
                if (ret < 0) {
                        pr_err("Failed in jack init \n");
                        goto alc5623_init_fail;
                }

                /* Default to OFF */
                tegra_ext_control(codec, TEGRA_AUDIO_OFF);

                ret = tegra_controls_init(codec);
                if (ret < 0) {
                        pr_err("Failed in controls init \n");
                        goto alc5623_init_fail;
                }
	
		audio_data->codec = codec;
		audio_data->init_done = 1;
	}

	return ret;

alc5623_init_fail:
        tegra_das_disable_mclk();
        tegra_das_close();
        return ret;

}

#define TEGRA_CREATE_SOC_DAI_LINK(xname, xstreamname,   \
                        xcpudai, xcodecdai, xops)       \
{                                                       \
        .name = xname,                                  \
        .stream_name = xstreamname,                     \
        .cpu_dai = xcpudai,                             \
        .codec_dai = xcodecdai,                         \
        .init = tegra_codec_init,                       \
        .ops = xops,                                    \
}


static struct snd_soc_dai_link tegra_soc_dai[] = {
#if defined(CONFIG_ARCH_TEGRA_2x_SOC)
        TEGRA_CREATE_SOC_DAI_LINK("ALC5623", "ALC5623 HiFi",
                &tegra_i2s_dai[0], &alc5623_dai,
                &tegra_hifi_ops),
        TEGRA_CREATE_SOC_DAI_LINK("Tegra-generic", "",
                &tegra_i2s_dai[1], &tegra_generic_codec_dai[0],
                &tegra_voice_ops),
        TEGRA_CREATE_SOC_DAI_LINK("Tegra-spdif", "Tegra Spdif",
                &tegra_spdif_dai, &tegra_generic_codec_dai[1],
                &tegra_spdif_ops),
#endif
};

static struct tegra_audio_data audio_data = {
        .init_done = 0,
        .play_device = TEGRA_AUDIO_DEVICE_NONE,
        .capture_device = TEGRA_AUDIO_DEVICE_NONE,
        .is_call_mode = false,
        .codec_con = TEGRA_AUDIO_OFF,
};


/* The Tegra card definition */
static struct snd_soc_card tegra_snd_soc = {
	.name = "tegra-alc5623",
	.platform 	= &tegra_soc_platform,
	.dai_link 	= tegra_soc_dai,
	.num_links 	= ARRAY_SIZE(tegra_soc_dai),
	.suspend_pre = tegra_soc_suspend_pre,
	.suspend_post = tegra_soc_suspend_post,
	.resume_pre = tegra_soc_resume_pre,
	.resume_post = tegra_soc_resume_post,
};

/* A sound device is composed of a card (in this case, Tegra, and
   a codec, in this case ALC5623 */
static struct snd_soc_device tegra_snd_devdata = {
	.card = &tegra_snd_soc,
	.codec_dev = &soc_codec_dev_alc5623,
	.codec_data = &audio_data,
};


static int __init tegra_init(void)
{
	pr_info("%s++", __func__);
        int ret = 0;

        tegra_snd_device = platform_device_alloc("soc-audio", -1);
        if (!tegra_snd_device) {
                pr_err("failed to allocate soc-audio \n");
                return ENOMEM;
        }

        platform_set_drvdata(tegra_snd_device, &tegra_snd_devdata);
        tegra_snd_devdata.dev = &tegra_snd_device->dev;

        ret = platform_device_add(tegra_snd_device);
        if (ret) {
                pr_err("audio device could not be added \n");
                goto fail;
        }

        return 0;

fail:
        if (tegra_snd_device) {
                platform_device_put(tegra_snd_device);
                tegra_snd_device = 0;
        }

        return ret;

err_put_regulator:
        regulator_put(alc5623_reg);
        return ret;
}


static void __exit tegra_exit(void)
{
	tegra_jack_exit();
        platform_device_unregister(tegra_snd_device);
}

module_init(tegra_init);
module_exit(tegra_exit);

MODULE_DESCRIPTION("Adam machine ASoC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
