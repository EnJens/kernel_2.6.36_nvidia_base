/*
 * tegra_pcm.c  --  ALSA Soc Audio Layer
 *
 * Copyright (c) 2009-2011, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include "tegra_soc.h"

#define PLAYBACK_STARTED true
#define PLAYBACK_STOPPED false


static const struct snd_pcm_hardware tegra_pcm_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED |
			SNDRV_PCM_INFO_PAUSE |
			SNDRV_PCM_INFO_RESUME |
			SNDRV_PCM_INFO_MMAP |
			SNDRV_PCM_INFO_MMAP_VALID ,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.channels_min = 1,
	.channels_max = 2,
	.buffer_bytes_max = (PAGE_SIZE * 8),
	.period_bytes_min = 128,
	.period_bytes_max = (PAGE_SIZE),
	.periods_min = 2,
	.periods_max = 8,
	.fifo_size   = 4,
};

/* The Latency (Period Size) for TDM can be changed below */
static const struct snd_pcm_hardware tegra_pcm_tdm_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED |
			SNDRV_PCM_INFO_PAUSE |
			SNDRV_PCM_INFO_RESUME |
			SNDRV_PCM_INFO_MMAP |
			SNDRV_PCM_INFO_MMAP_VALID ,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.channels_min = 8,
	.channels_max = 16,
	.buffer_bytes_max = 1024 * 16 * 4,
	.period_bytes_min = 1024 * 16,
	.period_bytes_max = 1024 * 16,
	.periods_min = 4,
	.periods_max = 4,
	.fifo_size   = 4,
};

static void tegra_pcm_queue_dma(struct tegra_runtime_data *prtd)
{
	struct snd_pcm_substream *substream = prtd->substream;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_dma_buffer *buf = &substream->dma_buffer;
	struct tegra_dma_req *dma_req;

	if (runtime->dma_addr && prtd->dma_chan) {
		prtd->size = frames_to_bytes(runtime, runtime->period_size);

		if (prtd->dma_state != STATE_ABORT) {
			prtd->dma_tail_idx = (prtd->dma_tail_idx + 1) %
						DMA_REQ_QCOUNT;
			dma_req = &prtd->dma_req[prtd->dma_tail_idx];

			if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
				dma_req->source_addr = buf->addr +
					frames_to_bytes(runtime,prtd->dma_pos);
			} else {
				dma_req->dest_addr = buf->addr +
					frames_to_bytes(runtime,prtd->dma_pos);
			}

			dma_req->size = prtd->size;
			tegra_dma_enqueue_req(prtd->dma_chan, dma_req);
		}
	}

	prtd->dma_pos += runtime->period_size;
	if (prtd->dma_pos >= runtime->buffer_size) {
		prtd->dma_pos = 0;
	}

}

static void dma_complete_callback (struct tegra_dma_req *req)
{
	struct tegra_runtime_data *prtd = (struct tegra_runtime_data *)req->dev;
	struct snd_pcm_substream *substream = prtd->substream;
	struct snd_pcm_runtime *runtime = substream->runtime;

	if (++prtd->period_index >= runtime->periods) {
		prtd->period_index = 0;
	}

	if (prtd->dma_state != STATE_ABORT) {
		prtd->dma_head_idx = (prtd->dma_head_idx + 1) %
						DMA_REQ_QCOUNT;
		snd_pcm_period_elapsed(substream);
		tegra_pcm_queue_dma(prtd);
	}
}

static int tegra_pcm_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	snd_pcm_set_runtime_buffer(substream, &substream->dma_buffer);
	return 0;
}

static int tegra_pcm_hw_free(struct snd_pcm_substream *substream)
{
	snd_pcm_set_runtime_buffer(substream, NULL);
	return 0;
}

static int tegra_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct tegra_runtime_data *prtd = substream->runtime->private_data;

	prtd->dma_pos = 0;
	prtd->period_index = 0;
	prtd->dma_head_idx = 0;
	prtd->dma_tail_idx = DMA_REQ_QCOUNT - 1;

	return 0;
}

static int tegra_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct tegra_runtime_data *prtd = substream->runtime->private_data;
	int i, ret = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:

#ifdef CONFIG_HAS_WAKELOCK
		wake_lock(&prtd->wake_lock);
#endif
		prtd->dma_state = STATE_INIT;
		tegra_pcm_queue_dma(prtd); /* dma enqueue req1 */
		tegra_pcm_queue_dma(prtd); /* dma enqueue req2 */
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:

		prtd->dma_state = STATE_ABORT;

		if (prtd->dma_chan) {
			tegra_dma_cancel(prtd->dma_chan);
			for (i = 0; i < DMA_REQ_QCOUNT; i++)
				tegra_dma_dequeue_req(prtd->dma_chan,
						&prtd->dma_req[i]);
			prtd->dma_head_idx = 0;
			prtd->dma_tail_idx = DMA_REQ_QCOUNT - 1;
		}
#ifdef CONFIG_HAS_WAKELOCK
		wake_unlock(&prtd->wake_lock);
#endif
		break;

	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static snd_pcm_uframes_t tegra_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct tegra_runtime_data *prtd = runtime->private_data;
	int size = prtd->period_index * runtime->period_size;

	if (prtd->dma_chan)
		size += bytes_to_frames(runtime,
				tegra_dma_get_transfer_count(
					prtd->dma_chan,
					&prtd->dma_req[prtd->dma_head_idx],
					false));

	return (size);
}

static int tegra_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->dai->cpu_dai;
	struct tegra_runtime_data *prtd = 0;
	struct tegra_i2s_info *info = cpu_dai->private_data;
	int i, ret = 0;
	int dma_mode;
	int need_dma_ch = 0;

	pr_debug("%s: cpu_dai %s, codec_dai %s, Device %d, Stream %s\n",
		 __func__, cpu_dai->name, rtd->dai->codec_dai->name,
		substream->pcm->device,
		(substream->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		"Playback" : "Capture");

	/* Ensure period size is multiple of minimum DMA step size */
	ret = snd_pcm_hw_constraint_step(runtime, 0,
		SNDRV_PCM_HW_PARAM_PERIOD_BYTES, DMA_STEP_SIZE_MIN);
	if (ret < 0) {
		pr_err("%s:snd_pcm_hw_constraint_step failed: %d\n",
			__func__, ret);
		goto fail;
	}

	/* Ensure buffer size is multiple of period size */
	ret = snd_pcm_hw_constraint_integer(runtime,
						SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0) {
		pr_err("%s:snd_pcm_hw_constraint_integer failed: %d\n",
			__func__, ret);
		goto fail;
	}

	prtd = kzalloc(sizeof(struct tegra_runtime_data), GFP_KERNEL);
	if (prtd == NULL)
		return -ENOMEM;

	runtime->private_data = prtd;
	prtd->substream = substream;

	prtd->dma_state = STATE_INVALID;

	if (!strcmp(cpu_dai->name, "tegra-spdif"))
	{
		for (i = 0; i < DMA_REQ_QCOUNT; i++) {
			setup_spdif_dma_request(substream,
					&prtd->dma_req[i],
					dma_complete_callback,
					prtd);
		}
		need_dma_ch = 1;
	} else if (strstr(cpu_dai->name, "tegra-i2s")) {
		for (i = 0; i < DMA_REQ_QCOUNT; i++) {
			setup_i2s_dma_request(substream,
					&prtd->dma_req[i],
					dma_complete_callback,
					prtd);
		}
		need_dma_ch = 1;
	}

	if (need_dma_ch) {
		if (info && info->pdata->tdm_enable)
			dma_mode = TEGRA_DMA_MODE_CONTINUOUS_DOUBLE;
		else
			dma_mode = TEGRA_DMA_MODE_CONTINUOUS_SINGLE;

		prtd->dma_chan = tegra_dma_allocate_channel(dma_mode, "pcm");
		if (prtd->dma_chan == NULL) {
			pr_err("%s: could not allocate DMA channel for PCM:\n",
				__func__);
			ret = -ENOMEM;
			goto fail;
		}
	}

#ifdef CONFIG_HAS_WAKELOCK
	snprintf(prtd->wake_lock_name, sizeof(prtd->wake_lock_name),
		"tegra-pcm-%s-%d",
		(substream->stream == SNDRV_PCM_STREAM_PLAYBACK) ? "out" : "in",
		substream->pcm->device);
	wake_lock_init(&prtd->wake_lock, WAKE_LOCK_SUSPEND,
		prtd->wake_lock_name);
#endif

	/* Set HW params now that initialization is complete */
	if (info && info->pdata->tdm_enable)
		snd_soc_set_runtime_hwparams(substream,
					&tegra_pcm_tdm_hardware);
	else
		snd_soc_set_runtime_hwparams(substream, &tegra_pcm_hardware);

	goto end;

fail:
	if (prtd) {
		prtd->dma_state = STATE_EXIT;

		if (prtd->dma_chan) {
			tegra_dma_flush(prtd->dma_chan);
			tegra_dma_free_channel(prtd->dma_chan);
			prtd->dma_chan = NULL;
		}
		kfree(prtd);
	}

end:
	return ret;
}

static int tegra_pcm_close(struct snd_pcm_substream *substream)
{
	int i;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct tegra_runtime_data *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->dai->cpu_dai;

	pr_debug("%s: cpu_dai %s, codec_dai %s, Device %d, Stream %s\n",
		 __func__, cpu_dai->name, rtd->dai->codec_dai->name,
		substream->pcm->device,
		(substream->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		"Playback" : "Capture");

	if (!prtd) {
		printk(KERN_ERR "tegra_pcm_close called with prtd == NULL\n");
		return 0;
	}

#ifdef CONFIG_HAS_WAKELOCK
	wake_lock_destroy(&prtd->wake_lock);
#endif
	if (prtd->dma_chan) {
		prtd->dma_state = STATE_EXIT;
		for (i = 0; i < DMA_REQ_QCOUNT; i++) {
			tegra_dma_dequeue_req(prtd->dma_chan, &prtd->dma_req[i]);
			if (strcmp(cpu_dai->name, "tegra-spdif") == 0)
				free_spdif_dma_request(substream);
			else
				free_i2s_dma_request(substream);
		}
		tegra_dma_flush(prtd->dma_chan);
		tegra_dma_free_channel(prtd->dma_chan);
		prtd->dma_chan = NULL;
		prtd->dma_head_idx = 0;
		prtd->dma_tail_idx = DMA_REQ_QCOUNT - 1;
	}
	kfree(prtd);

	return 0;
}

static int tegra_pcm_mmap(struct snd_pcm_substream *substream,
				struct vm_area_struct *vma)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	return dma_mmap_writecombine(substream->pcm->card->dev, vma,
					runtime->dma_area,
					runtime->dma_addr,
					runtime->dma_bytes);
}

static struct snd_pcm_ops tegra_pcm_ops = {
	.open       = tegra_pcm_open,
	.close      = tegra_pcm_close,
	.ioctl      = snd_pcm_lib_ioctl,
	.hw_params  = tegra_pcm_hw_params,
	.hw_free    = tegra_pcm_hw_free,
	.prepare    = tegra_pcm_prepare,
	.trigger    = tegra_pcm_trigger,
	.pointer    = tegra_pcm_pointer,
	.mmap       = tegra_pcm_mmap,
};

static int tegra_pcm_preallocate_dma_buffer(struct snd_pcm *pcm, int stream)
{
	struct snd_pcm_substream *substream = pcm->streams[stream].substream;
	struct snd_dma_buffer *buf = &substream->dma_buffer;
	size_t size;

	size = max(tegra_pcm_hardware.buffer_bytes_max,
				tegra_pcm_tdm_hardware.buffer_bytes_max);

	buf->area = dma_alloc_writecombine(pcm->card->dev, size,
						&buf->addr, GFP_KERNEL);

	if (!buf->area)
		return -ENOMEM;

	buf->dev.type = SNDRV_DMA_TYPE_DEV;
	buf->dev.dev = pcm->card->dev;
	buf->private_data = NULL;
	buf->bytes = size;

	return 0;
}

static void tegra_pcm_deallocate_dma_buffer(struct snd_pcm *pcm, int stream)
{
	struct snd_pcm_substream *substream = pcm->streams[stream].substream;
	struct snd_dma_buffer *buf = &substream->dma_buffer;

	for (stream = 0; stream < 2; stream++) {
		substream = pcm->streams[stream].substream;
		if (!substream)
			continue;

		buf = &substream->dma_buffer;
		if (!buf->area)
			continue;

		dma_free_writecombine(pcm->card->dev, buf->bytes,
					buf->area, buf->addr);
		buf->area = NULL;
	}
}

static void tegra_pcm_free(struct snd_pcm *pcm)
{
	struct snd_pcm_substream *substream;
	struct snd_dma_buffer *buf;
	int stream;

	for (stream = 0; stream < 2; stream++) {
		substream = pcm->streams[stream].substream;
		buf = &substream->dma_buffer;
		if (!buf) {
			printk(KERN_ERR "no buffer %d \n",stream);
			continue;
		}
		tegra_pcm_deallocate_dma_buffer(pcm ,stream);
	}
}

static u64 tegra_dma_mask = DMA_BIT_MASK(32);

static int tegra_pcm_new(struct snd_card *card,
				struct snd_soc_dai *dai, struct snd_pcm *pcm)
{
	int ret = 0;

	if (!card->dev->dma_mask)
		card->dev->dma_mask = &tegra_dma_mask;
	if (!card->dev->coherent_dma_mask)
		card->dev->coherent_dma_mask = 0xffffffff;

	if (dai->playback.channels_min) {
		ret = tegra_pcm_preallocate_dma_buffer(pcm,
						SNDRV_PCM_STREAM_PLAYBACK);
		if (ret)
			goto err;
	}

	if (dai->capture.channels_min) {
		ret = tegra_pcm_preallocate_dma_buffer(pcm,
						SNDRV_PCM_STREAM_CAPTURE);
		if (ret)
			goto err_free_play;
	}

	return 0;

err_free_play:
	tegra_pcm_deallocate_dma_buffer(pcm, SNDRV_PCM_STREAM_PLAYBACK);
err:
	return ret;
}

struct snd_soc_platform tegra_soc_platform = {
	.name     = "tegra-pcm-audio",
	.pcm_ops  = &tegra_pcm_ops,
	.pcm_new  = tegra_pcm_new,
	.pcm_free = tegra_pcm_free,
};
EXPORT_SYMBOL_GPL(tegra_soc_platform);

static int __init tegra_soc_platform_init(void)
{
	return snd_soc_register_platform(&tegra_soc_platform);
}
module_init(tegra_soc_platform_init);

static void __exit tegra_soc_platform_exit(void)
{
	snd_soc_unregister_platform(&tegra_soc_platform);
}
module_exit(tegra_soc_platform_exit);

MODULE_DESCRIPTION("Tegra PCM ASoC module");
MODULE_LICENSE("GPL");
