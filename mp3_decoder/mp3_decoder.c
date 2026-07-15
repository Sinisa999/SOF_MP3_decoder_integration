// SPDX-License-Identifier: BSD-3-Clause

#include <sof/audio/component_ext.h>
#include <sof/audio/module_adapter/module/generic.h>
#include <sof/audio/audio_stream.h>
#include <sof/audio/format.h>
#include <sof/lib/uuid.h>
#include <sof/trace/trace.h>
#include <rtos/alloc.h>
#include <errno.h>
#include <rtos/init.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>

#define DR_MP3_IMPLEMENTATION

#include "dr_mp3.h"

LOG_MODULE_REGISTER(mp3_decoder, CONFIG_SOF_LOG_LEVEL);

/* UUID: c732b15b-3f02-4347-a4b6-3a0776246c14 */
SOF_DEFINE_REG_UUID(mp3_decoder);

DECLARE_TR_CTX(mp3_decoder_tr, SOF_UUID(mp3_decoder_uuid), LOG_LEVEL_INFO);

/* -------------------------------------------------------------------------- */
/* Constants */
/* -------------------------------------------------------------------------- */

#define MP3_INPUT_BUFFER_SIZE 4096
#define MP3_INPUT_FILL_TARGET 2048
#define MP3_INPUT_DECODE_MIN 512
#define PCM_RING_FRAMES 8192

#define MP3_MAX_PCM_FRAMES DRMP3_MAX_PCM_FRAMES_PER_MP3_FRAME
#define MP3_MAX_PCM_SAMPLES DRMP3_MAX_SAMPLES_PER_FRAME

/* -------------------------------------------------------------------------- */
/* Private data */
/* -------------------------------------------------------------------------- */

struct mp3_decoder_data {
    drmp3dec dec;
    drmp3dec_frame_info info;

    /* Kompresovani MP3 Input buffer */
    drmp3_uint8 input_buffer[MP3_INPUT_BUFFER_SIZE];
    uint32_t input_bytes_available;

    /* Lokalni PCM kruzni bafer */
    int16_t pcm_ring[PCM_RING_FRAMES * 2];
    uint32_t pcm_read_pos;
    uint32_t pcm_write_pos;
    uint32_t pcm_frames_available;

    /* Privremeni buffer dekodovanih PCM odbiraka*/
    drmp3_int16 pcm_buffer[MP3_MAX_PCM_SAMPLES];

    /*dekodovani PCM podaci koji treba da budu upisani u SOF sink*/
    uint32_t pending_pcm_frames;
    uint32_t pending_pcm_frame_offset;
    uint32_t pending_pcm_channels;

    /* Informacije o strimu dekodovane iz mp3 bitstream-a*/
    uint32_t sample_rate;
    uint32_t channels;


    uint32_t decoded_mp3_frames_total;
    uint32_t decoded_pcm_frames_total;
    uint32_t consumed_mp3_bytes_total;
    uint32_t produced_pcm_frames_total;
    uint32_t decode_failures_total;
    uint32_t process_calls_total;
    uint32_t copied_mp3_bytes_total;

    bool initialized;

    FILE *drmp3_pcm_dump;
    FILE *sink_pcm_dump;

};

/* -------------------------------------------------------------------------- */
/* init */
/* -------------------------------------------------------------------------- */

static int mp3_decoder_init(struct processing_module *mod)
{
    struct mp3_decoder_data *md;

    md = rzalloc(SOF_MEM_FLAG_USER, sizeof(*md));
    if(!md) {
        return -ENOMEM;
    }

    /* Inicijalizacija dr_mp3 dekodera */
    drmp3dec_init(&md->dec);

    /* Ciscenje informacija prethodnih frejmova i buffera*/
    memset(&md->info, 0, sizeof(md->info));
    memset(md->input_buffer, 0, sizeof(md->input_buffer));
    memset(md->pcm_buffer, 0, sizeof(md->pcm_buffer));
    memset(md->pcm_ring, 0, sizeof(md->pcm_ring));

    md->input_bytes_available = 0;

    md->pcm_read_pos = 0;
    md->pcm_write_pos = 0;
    md->pcm_frames_available = 0;

    md->pending_pcm_frames = 0;
    md->pending_pcm_frame_offset = 0;
    md->pending_pcm_channels = 0;

    md->sample_rate = 0;
    md->channels = 0;
    md->initialized = true;

    md->decoded_mp3_frames_total = 0;
    md->decoded_pcm_frames_total = 0;
    md->consumed_mp3_bytes_total = 0;
    md->produced_pcm_frames_total = 0;
    md->decode_failures_total = 0;
    md->process_calls_total = 0;
    md->copied_mp3_bytes_total = 0;

    md->drmp3_pcm_dump = fopen("/tmp/mp3_drmp3_decoded.raw", "wb");
    md->sink_pcm_dump = fopen("/tmp/mp3_written_to_sink.raw", "wb");

    if (!md->drmp3_pcm_dump)
        comp_err(mod->dev, "failed to open /tmp/mp3_drmp3_decoded.raw");

    if (!md->sink_pcm_dump)
        comp_err(mod->dev, "failed to open /tmp/mp3_written_to_sink.raw");

    module_set_private_data(mod, md);

    comp_dbg(mod->dev, "mp3_decoder_init");

    return 0;

}

/* -------------------------------------------------------------------------- */
/* prepare */
/* -------------------------------------------------------------------------- */

static int mp3_decoder_prepare(struct processing_module *mod, struct sof_source **sources, int num_sources, struct sof_sink **sinks, int num_sinks)
{
    struct comp_dev *dev = mod->dev;
    struct comp_buffer *sourceb;
    struct comp_buffer *sinkb;
    uint32_t source_period_bytes;
	uint32_t sink_period_bytes;

    comp_dbg(dev, "mp3_decoder_prepare");

    sourceb = comp_dev_get_first_data_producer(dev);
    sinkb = comp_dev_get_first_data_consumer(dev);

    if (!sourceb || !sinkb) {
        comp_err(dev, "mp3_decoder_prepare: no source or sink buffer");
        return -ENOTCONN;
    }

    //mod->priv.mpd.in_buff_size = MP3_INPUT_FILL_TARGET;

    /* U najgorem slucaju jedan dekodovani odbirak jednog MP3 frejma:
        1152 PCM frejmova * 2 kanala * 2 bajta = 4608 bajtova
    */
    //mod->priv.mpd.out_buff_size = MP3_MAX_PCM_FRAMES * 2 * sizeof(drmp3_int16);

    source_period_bytes = audio_stream_period_bytes(&sourceb->stream, dev->frames);
	sink_period_bytes = audio_stream_period_bytes(&sinkb->stream, dev->frames);

    mod->priv.mpd.in_buff_size = source_period_bytes;
	mod->priv.mpd.out_buff_size = sink_period_bytes;

    comp_info(dev,
		"mp3_decoder_prepare: source_period_bytes=%d sink_period_bytes=%d dev_frames=%d",
		source_period_bytes, sink_period_bytes, dev->frames);

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Pomocna funkcija za preuzimanje kompresovanih bajtova iz lokalnog mp3 bafera */
/* -------------------------------------------------------------------------- */

static void mp3_decoder_consume_input_bytes(struct mp3_decoder_data *md, uint32_t bytes_to_consume)
{
    uint32_t remaining_bytes;

    if (!md || bytes_to_consume == 0)
    {
        return;
    }

    if (bytes_to_consume > md->input_bytes_available)
    {
        bytes_to_consume = md->input_bytes_available;
    }

    remaining_bytes = md->input_bytes_available - bytes_to_consume;

    if (remaining_bytes > 0)
    {
        memmove(md->input_buffer, md->input_buffer + bytes_to_consume, remaining_bytes);
    }

    md->input_bytes_available = remaining_bytes;
}


/* -------------------------------------------------------------------------- */
/* Pomocna funkcija za kopiranje kompresovanih bajtova iz SOF source bafera u lokalni mp3 input bafer */
/* -------------------------------------------------------------------------- */
static uint32_t mp3_decoder_fill_input_buffer(struct audio_stream *source, 
                            struct mp3_decoder_data *md,
                            uint32_t source_bytes_available)
{
    uint8_t *src;
    uint32_t copied = 0;

    if (!source || !md)
    {
        return 0;
    }

    src = audio_stream_get_rptr(source);

    while (source_bytes_available > 0 &&
       md->input_bytes_available < MP3_INPUT_BUFFER_SIZE)
        {

            md->input_buffer[md->input_bytes_available] = *src;

            src = audio_stream_wrap(source, src + 1);
            

            md->input_bytes_available++;
            copied++;
            source_bytes_available--;
        }

        return copied;
}

static uint32_t mp3_decoder_fill_input_buffer_to_target(struct audio_stream *source,
							struct mp3_decoder_data *md,
							uint32_t source_bytes_available)
{
	uint32_t bytes_needed;
	uint32_t copy_limit;

	if (!source || !md)
		return 0;

	if (md->input_bytes_available >= MP3_INPUT_FILL_TARGET)
		return 0;

	bytes_needed = MP3_INPUT_FILL_TARGET - md->input_bytes_available;
	copy_limit = MIN(source_bytes_available, bytes_needed);

	return mp3_decoder_fill_input_buffer(source, md, copy_limit);
}

/* -------------------------------------------------------------------------- */
/* Pomocna funkcija za upisivanje dekodovanih PCM odbiraka u SOF sink */
/* -------------------------------------------------------------------------- */
/*static uint32_t mp3_decoder_write_pending_pcm(struct audio_stream *sink,
					      struct mp3_decoder_data *md,
					      uint32_t sink_frames_available)
{
	int16_t *dst;
	int16_t left;
	int16_t right;
	uint32_t frames_written = 0;
	uint32_t frame_index;

	if (!sink || !md)
		return 0;

    dst = audio_stream_get_wptr(sink);

	while (sink_frames_available > 0 &&
	       md->pending_pcm_frame_offset < md->pending_pcm_frames) {
		frame_index = md->pending_pcm_frame_offset;

		if (md->pending_pcm_channels == 1) {
			left = md->pcm_buffer[frame_index];
			right = left;
		} else {
			left = md->pcm_buffer[frame_index * md->pending_pcm_channels + 0];
			right = md->pcm_buffer[frame_index * md->pending_pcm_channels + 1];
		}

        //Temp debug dump
        if (md->sink_pcm_dump) {
            int16_t stereo_pair[2];

            stereo_pair[0] = left;
            stereo_pair[1] = right;

            fwrite(stereo_pair, sizeof(int16_t), 2, md->sink_pcm_dump);
        }

		*dst = left;
		dst = audio_stream_wrap(sink, dst + 1);

		*dst = right;
		dst = audio_stream_wrap(sink, dst + 1);

		md->pending_pcm_frame_offset++;
		frames_written++;
		sink_frames_available--;
	}

	if (md->pending_pcm_frame_offset >= md->pending_pcm_frames) {
		md->pending_pcm_frames = 0;
		md->pending_pcm_frame_offset = 0;
		md->pending_pcm_channels = 0;
	}

    if (md->sink_pcm_dump)
	    fflush(md->sink_pcm_dump);

	return frames_written;
}*/

/* Temp pomocna funckija koja nam pomazaze za pokretanje sa testbenchom da bi mu pruzali neki output dok
    modul akumulira dovoljno podataka */
static uint32_t mp3_decoder_write_silence(struct audio_stream *sink,
					  uint32_t sink_frames_available)
{
	int16_t *dst;
	uint32_t frames_written = 0;

	if (!sink)
		return 0;

	dst = audio_stream_get_wptr(sink);

	while (sink_frames_available > 0) {
		/* stereo silence */
		*dst = 0;
		dst = audio_stream_wrap(sink, dst + 1);

		*dst = 0;
		dst = audio_stream_wrap(sink, dst + 1);

		frames_written++;
		sink_frames_available--;
	}

	return frames_written;
}


static uint32_t mp3_decoder_pcm_ring_free_frames(struct mp3_decoder_data *md)
{
	if (!md)
		return 0;

	return PCM_RING_FRAMES - md->pcm_frames_available;
}


static uint32_t mp3_decoder_pcm_ring_write(struct mp3_decoder_data *md,
					   int16_t *src,
					   uint32_t frames,
					   uint32_t channels)
{
	uint32_t written = 0;
	uint32_t pos;
	int16_t left;
	int16_t right;

	if (!md || !src)
		return 0;

	while (written < frames &&
	       md->pcm_frames_available < PCM_RING_FRAMES) {
		if (channels == 1) {
			left = src[written];
			right = left;
		} else {
			left = src[written * channels + 0];
			right = src[written * channels + 1];
		}

		pos = md->pcm_write_pos;

		md->pcm_ring[pos * 2 + 0] = left;
		md->pcm_ring[pos * 2 + 1] = right;

		md->pcm_write_pos++;
		if (md->pcm_write_pos >= PCM_RING_FRAMES)
			md->pcm_write_pos = 0;

		md->pcm_frames_available++;
		written++;
	}

	return written;
}


static uint32_t mp3_decoder_pcm_ring_read_to_sink(struct audio_stream *sink,
						  struct mp3_decoder_data *md,
						  uint32_t sink_frames_available)
{
	int16_t *dst;
	uint32_t frames_written = 0;
	uint32_t pos;
	int16_t left;
	int16_t right;

	if (!sink || !md)
		return 0;

	dst = audio_stream_get_wptr(sink);

	while (sink_frames_available > 0 &&
	       md->pcm_frames_available > 0) {
		pos = md->pcm_read_pos;

		left = md->pcm_ring[pos * 2 + 0];
		right = md->pcm_ring[pos * 2 + 1];

		if (md->sink_pcm_dump) {
			int16_t stereo_pair[2];

			stereo_pair[0] = left;
			stereo_pair[1] = right;

			fwrite(stereo_pair, sizeof(int16_t), 2, md->sink_pcm_dump);
		}

		*dst = left;
		dst = audio_stream_wrap(sink, dst + 1);

		*dst = right;
		dst = audio_stream_wrap(sink, dst + 1);

		md->pcm_read_pos++;
		if (md->pcm_read_pos >= PCM_RING_FRAMES)
			md->pcm_read_pos = 0;

		md->pcm_frames_available--;
		sink_frames_available--;
		frames_written++;
	}

	if (md->sink_pcm_dump)
		fflush(md->sink_pcm_dump);

	return frames_written;
}



/* -------------------------------------------------------------------------- */
/* process */
/* -------------------------------------------------------------------------- */

static int mp3_decoder_process(struct processing_module *mod,
                struct input_stream_buffer *input_buffers,
                int num_input_buffers,
                struct output_stream_buffer *output_buffers,
                int num_output_buffers)
{
    struct mp3_decoder_data *md = module_get_private_data(mod);
    struct audio_stream *source;
    struct audio_stream *sink;
	//uint32_t source_channels;
	uint32_t source_bytes_available;
	uint32_t sink_channels;
	uint32_t sink_frames_available;
	uint32_t copied_bytes;
	uint32_t written_frames;
	int decoded_frames;
    uint32_t frames_to_write;


    if (!md)
        return -EINVAL;

    md->process_calls_total++;

    if (!input_buffers || !output_buffers)
        return -EINVAL;

    if (num_input_buffers < 1 || num_output_buffers < 1)
        return -EINVAL;

    if (!input_buffers[0].data || !output_buffers[0].data)
        return -EINVAL;

    source = input_buffers[0].data;
    sink = output_buffers[0].data;


    sink_channels = audio_stream_get_channels(sink);



	
	if (sink_channels != 2) {
		comp_err(mod->dev, "mp3_decoder_process: expected stereo sink");
		return -EINVAL;
	}



    
    /* 1. Get SOF stream state */
    source_bytes_available = audio_stream_get_avail_bytes(source);
    sink_frames_available = audio_stream_get_free_frames(sink);
    //frames_to_write = MIN(sink_frames_available, mod->dev->frames);
    frames_to_write = sink_frames_available;

    comp_info(mod->dev,
	  "mp3 state: in_size=%d out_size=%d src_avail=%d sink_free=%d md_input=%d pcm_avail=%d pcm_free=%d pcm_r=%d pcm_w=%d",
	  input_buffers[0].size,
	  output_buffers[0].size,
	  source_bytes_available,
	  sink_frames_available,
	  md->input_bytes_available,
	  md->pcm_frames_available,
	  mp3_decoder_pcm_ring_free_frames(md),
	  md->pcm_read_pos,
	  md->pcm_write_pos);

    /* 2. Copy compressed bytes from SOF source into local MP3 input buffer */
    copied_bytes = mp3_decoder_fill_input_buffer_to_target(source, md,
						       source_bytes_available);

    if (copied_bytes > 0) {
        md->copied_mp3_bytes_total += copied_bytes;
        audio_stream_consume(source, copied_bytes);
    }

    /* 3. Decode compressed MP3 frames into private PCM ring */
    while (md->input_bytes_available >= MP3_INPUT_DECODE_MIN &&
            mp3_decoder_pcm_ring_free_frames(md) >= MP3_MAX_PCM_FRAMES) {
        memset(&md->info, 0, sizeof(md->info));

        decoded_frames = drmp3dec_decode_frame(&md->dec,
                            md->input_buffer,
                            md->input_bytes_available,
                            md->pcm_buffer,
                            &md->info);

        comp_info(mod->dev,
            "mp3 decode: avail=%d decoded=%d frame_bytes=%d ch=%d sr=%d",
            md->input_bytes_available,
            decoded_frames,
            md->info.frame_bytes,
            md->info.channels,
            md->info.sample_rate);

        if (decoded_frames <= 0) {
            md->decode_failures_total++;

            if (md->info.frame_bytes > 0) {
                md->consumed_mp3_bytes_total += md->info.frame_bytes;
                mp3_decoder_consume_input_bytes(md, md->info.frame_bytes);
            } else if (md->input_bytes_available >= MP3_INPUT_FILL_TARGET) {
                mp3_decoder_consume_input_bytes(md, 1);
                md->consumed_mp3_bytes_total++;
            }

            break;
        }

        md->decoded_mp3_frames_total++;
        md->decoded_pcm_frames_total += decoded_frames;

        if (md->info.sample_rate > 0)
            md->sample_rate = md->info.sample_rate;

        if (md->info.channels > 0)
            md->channels = md->info.channels;

        mp3_decoder_pcm_ring_write(md,
                    md->pcm_buffer,
                    decoded_frames,
                    md->info.channels);

        if (md->info.frame_bytes > 0) {
            md->consumed_mp3_bytes_total += md->info.frame_bytes;
            mp3_decoder_consume_input_bytes(md, md->info.frame_bytes);
        } else {
            break;
        }
    }

    /* 4. Write one period from PCM ring to SOF sink */
    written_frames = mp3_decoder_pcm_ring_read_to_sink(sink, md, frames_to_write);

    if (written_frames > 0) {
        md->produced_pcm_frames_total += written_frames;
        audio_stream_produce(sink, written_frames * sink_channels * sizeof(int16_t));
        return 0;
    }

    /* 5. Optional temporary silence if no decoded PCM exists yet */
    if (md->input_bytes_available < MP3_INPUT_DECODE_MIN) {
        written_frames = mp3_decoder_write_silence(sink, frames_to_write);

        if (written_frames > 0)
            audio_stream_produce(sink, written_frames * sink_channels * sizeof(int16_t));
    }

    return 0;

}


/* Helper funkcija za dekodovanje preostalih frejmova (trenutno za debag) */
/*static void mp3_decoder_debug_drain_remaining_frames(struct processing_module *mod,
						     struct mp3_decoder_data *md)
{
	int decoded_frames;

	if (!md)
		return;

	//Ignore any pending PCM for this debug-only drain.
	md->pending_pcm_frames = 0;
	md->pending_pcm_frame_offset = 0;
	md->pending_pcm_channels = 0;

	while (md->input_bytes_available >= MP3_INPUT_FILL_TARGET) {
		memset(&md->info, 0, sizeof(md->info));

		decoded_frames = drmp3dec_decode_frame(&md->dec,
						       md->input_buffer,
						       md->input_bytes_available,
						       md->pcm_buffer,
						       &md->info);

		comp_info(mod->dev,
			  "mp3 drain decode: avail=%d decoded=%d frame_bytes=%d ch=%d sr=%d",
			  md->input_bytes_available,
			  decoded_frames,
			  md->info.frame_bytes,
			  md->info.channels,
			  md->info.sample_rate);

		if (decoded_frames <= 0) {
			md->decode_failures_total++;

			if (md->info.frame_bytes > 0) {
				md->consumed_mp3_bytes_total += md->info.frame_bytes;
				mp3_decoder_consume_input_bytes(md, md->info.frame_bytes);
			} else {
				mp3_decoder_consume_input_bytes(md, 1);
			}

			continue;
		}

		md->decoded_mp3_frames_total++;
		md->decoded_pcm_frames_total += decoded_frames;

		if (md->info.frame_bytes > 0) {
			md->consumed_mp3_bytes_total += md->info.frame_bytes;
			mp3_decoder_consume_input_bytes(md, md->info.frame_bytes);
		} else {
			// Safety fallback to avoid infinite loop. 
			mp3_decoder_consume_input_bytes(md, 1);
		}

		if (md->info.sample_rate > 0)
			md->sample_rate = md->info.sample_rate;

		if (md->info.channels > 0)
			md->channels = md->info.channels;
	}
}*/

/* -------------------------------------------------------------------------- */
/* reset */
/* -------------------------------------------------------------------------- */

static int mp3_decoder_reset(struct processing_module *mod)
{

    struct mp3_decoder_data *md = module_get_private_data(mod);



    if (!md)
        return 0;

    //mp3_decoder_debug_drain_remaining_frames(mod, md);

    comp_info(mod->dev,
        "mp3 summary before reset: process_calls=%d decoded_mp3_frames=%d decoded_pcm_frames=%d consumed_mp3_bytes=%d produced_pcm_frames=%d decode_failures=%d last_sr=%d last_ch=%d input_left=%d pcm_ring_available=%d pcm_read=%d pcm_write=%d copied_mp3_bytes=%d",
        md->process_calls_total,
        md->decoded_mp3_frames_total,
        md->decoded_pcm_frames_total,
        md->consumed_mp3_bytes_total,
        md->produced_pcm_frames_total,
        md->decode_failures_total,
        md->sample_rate,
        md->channels,
        md->input_bytes_available,
        md->pcm_frames_available,
        md->pcm_read_pos,
        md->pcm_write_pos,
        md->copied_mp3_bytes_total);

    drmp3dec_init(&md->dec);


	memset(&md->info, 0, sizeof(md->info));
	memset(md->input_buffer, 0, sizeof(md->input_buffer));
	memset(md->pcm_buffer, 0, sizeof(md->pcm_buffer));
    memset(md->pcm_ring, 0, sizeof(md->pcm_ring));


    md->pcm_read_pos = 0;
    md->pcm_write_pos = 0;
    md->pcm_frames_available = 0;

    md->input_bytes_available = 0;
    md->sample_rate = 0;
    md->channels = 0;

    md->pending_pcm_frames = 0;
    md->pending_pcm_frame_offset = 0;
    md->pending_pcm_channels = 0;

    
    md->decoded_mp3_frames_total = 0;
    md->decoded_pcm_frames_total = 0;
    md->consumed_mp3_bytes_total = 0;
    md->produced_pcm_frames_total = 0;
    md->decode_failures_total = 0;
    md->process_calls_total = 0;

    comp_dbg(mod->dev, "mp3_decoder_reset");

    return 0;
}

/* -------------------------------------------------------------------------- */
/* free */
/* -------------------------------------------------------------------------- */

static int mp3_decoder_free(struct processing_module *mod)
{
    struct mp3_decoder_data *md = module_get_private_data(mod);

    if (md){

        if (md->drmp3_pcm_dump) {
		fclose(md->drmp3_pcm_dump);
		md->drmp3_pcm_dump = NULL;
	    }

        if (md->sink_pcm_dump) {
            fclose(md->sink_pcm_dump);
            md->sink_pcm_dump = NULL;
        }

    /*comp_info(mod->dev,
	  "mp3 summary: process_calls=%d decoded_mp3_frames=%d decoded_pcm_frames=%d consumed_mp3_bytes=%d produced_pcm_frames=%d decode_failures=%d last_sr=%d last_ch=%d input_left=%d pending_frames=%d copied_mp3_bytes=%d pending_offset=%d",
	  md->process_calls_total,
	  md->decoded_mp3_frames_total,
	  md->decoded_pcm_frames_total,
	  md->consumed_mp3_bytes_total,
	  md->produced_pcm_frames_total,
	  md->decode_failures_total,
	  md->sample_rate,
	  md->channels,
	  md->input_bytes_available,
	  md->pending_pcm_frames,
      md->copied_mp3_bytes_total,
	  md->pending_pcm_frame_offset);*/

        rfree(md);
    }

    module_set_private_data(mod, NULL);

    comp_dbg(mod->dev, "mp3_decoder_free");

    return 0;
}

/* -------------------------------------------------------------------------- */
/* interface */
/* -------------------------------------------------------------------------- */

static const struct module_interface mp3_decoder_interface = {
    .init = mp3_decoder_init,
    .prepare = mp3_decoder_prepare,
    .process_audio_stream = mp3_decoder_process,
    .reset = mp3_decoder_reset,
    .free = mp3_decoder_free,
};

/* -------------------------------------------------------------------------- */
/* registration */
/* -------------------------------------------------------------------------- */

UT_STATIC void sys_comp_module_mp3_decoder_interface_init(void);

DECLARE_MODULE_ADAPTER(mp3_decoder_interface, mp3_decoder_uuid, mp3_decoder_tr);
SOF_MODULE_INIT(mp3_decoder, sys_comp_module_mp3_decoder_interface_init);




