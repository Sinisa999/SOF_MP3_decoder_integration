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

    /* Privremeni buffer dekodovanih PCM odbiraka*/
    drmp3_int16 pcm_buffer[MP3_MAX_PCM_SAMPLES];

    /*dekodovani PCM podaci koji treba da budu upisani u SOF sink*/
    uint32_t pending_pcm_frames;
    uint32_t pending_pcm_frame_offset;
    uint32_t pending_pcm_channels;

    /* Informacije o strimu dekodovane iz mp3 bitstream-a*/
    uint32_t sample_rate;
    uint32_t channels;

    bool initialized;

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

    md->input_bytes_available = 0;

    md->pending_pcm_frames = 0;
    md->pending_pcm_frame_offset = 0;
    md->pending_pcm_channels = 0;

    md->sample_rate = 0;
    md->channels = 0;
    md->initialized = true;

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

    comp_dbg(dev, "mp3_decoder_prepare");

    sourceb = comp_dev_get_first_data_producer(dev);
    sinkb = comp_dev_get_first_data_consumer(dev);

    if (!sourceb || !sinkb) {
        comp_err(dev, "mp3_decoder_prepare: no source or sink buffer");
        return -ENOTCONN;
    }

    mod->priv.mpd.in_buff_size = MP3_INPUT_FILL_TARGET;

    /* U najgorem slucaju jedan dekodovani odbirak jednog MP3 frejma:
        1152 PCM frejmova * 2 kanala * 2 bajta = 4608 bajtova
    */
    mod->priv.mpd.out_buff_size = MP3_MAX_PCM_FRAMES * 2 * sizeof(drmp3_int16);

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

    while (source_bytes_available > 0 && 
            md->input_bytes_available < MP3_INPUT_FILL_TARGET &&
            md->input_bytes_available < MP3_INPUT_BUFFER_SIZE)
        {
            src = audio_stream_get_rptr(source);

            md->input_buffer[md->input_bytes_available] = *src;

            src = audio_stream_wrap(source, src + 1);
            audio_stream_set_rptr(source, src);

            md->input_bytes_available++;
            copied++;
            source_bytes_available--;
        }

        return copied;
}

/* -------------------------------------------------------------------------- */
/* Pomocna funkcija za upisivanje dekodovanih PCM odbiraka u SOF sink */
/* -------------------------------------------------------------------------- */
static uint32_t mp3_decoder_write_pending_pcm(struct audio_stream *sink,
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

		dst = audio_stream_get_wptr(sink);
		*dst = left;
		dst = audio_stream_wrap(sink, dst + 1);

		*dst = right;
		dst = audio_stream_wrap(sink, dst + 1);

		audio_stream_set_wptr(sink, dst);

		md->pending_pcm_frame_offset++;
		frames_written++;
		sink_frames_available--;
	}

	if (md->pending_pcm_frame_offset >= md->pending_pcm_frames) {
		md->pending_pcm_frames = 0;
		md->pending_pcm_frame_offset = 0;
		md->pending_pcm_channels = 0;
	}

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
	uint32_t source_channels;
	uint32_t source_bytes_available;
	uint32_t sink_channels;
	uint32_t sink_frames_available;
	uint32_t copied_bytes;
	uint32_t written_frames;
	int decoded_frames;

    if (!md)
        return -EINVAL;

    if (!input_buffers || !output_buffers)
        return -EINVAL;

    if (num_input_buffers < 1 || num_output_buffers < 1)
        return -EINVAL;

    if (!input_buffers[0].data || !output_buffers[0].data)
        return -EINVAL;

    source = input_buffers[0].data;
    sink = output_buffers[0].data;

    /* Jos uvek preostaje da se doda pravi processing.

        Sledeci koraci:
        1, Kopirati kompresovane bajtove iz source-a u md->input_buffer.
        2. Dekodovati jedan MP3 frejm sa drmp3dec_decode_frame().
        3. Preuzeti info.frame_bytes iz md->input_buffer.
        4. Upisati dekodovane PCM odbirke iz md->pcm_buffer u sink.
        5. audio_stream_consume(source, compressed_bytes_consumed)
        6. audio_stream_produce(sink, pcm_bytes_produced)
    */

    source_channels = audio_stream_get_channels(source);
    source_bytes_available = input_buffers[0].size * source_channels * sizeof(int16_t);

    sink_channels = audio_stream_get_channels(sink);



	
	if (sink_channels != 2) {
		comp_err(mod->dev, "mp3_decoder_process: expected stereo sink");
		return -EINVAL;
	}

	sink_frames_available = output_buffers[0].size;


     /* Ako smo vec dekodovali MP3 frejm ranije ali nismo sve upisali u bafer, preuzeti ostatak prvo */

	if (md->pending_pcm_frames > 0) {
		written_frames = mp3_decoder_write_pending_pcm(sink, md,
							       sink_frames_available);

		if (written_frames > 0)
			audio_stream_produce(sink, written_frames * sink_channels * sizeof(int16_t));

		if (md->pending_pcm_frames > 0)
			return 0;
	}


    /* Kopirati kompresovane MP3 bajtove iz SOF source bafera u lokalni input buffer */

	copied_bytes = mp3_decoder_fill_input_buffer(source, md,
						     source_bytes_available);

	if (copied_bytes > 0)
		audio_stream_consume(source, copied_bytes);


    /* Potrebna bar 4 bajta za MP3 frejm header */

	if (md->input_bytes_available < 4)
		return 0;

	memset(&md->info, 0, sizeof(md->info));

	
     /* Dekodovanje jednog MP3 frejma */

	decoded_frames = drmp3dec_decode_frame(&md->dec,
					       md->input_buffer,
					       md->input_bytes_available,
					       md->pcm_buffer,
					       &md->info);

	comp_dbg(mod->dev,
		 "mp3 decode: avail=%d decoded=%d frame_bytes=%d ch=%d sr=%d",
		 md->input_bytes_available,
		 decoded_frames,
		 md->info.frame_bytes,
		 md->info.channels,
		 md->info.sample_rate);


    /*
        Ako ni jedan frejm nije detektovan, preuzeti junk/skipped frejm bajtove.
        Ako bafer nije dovoljno popunjen, preuzeti jedan bajt da bi moglo da se
        nastavi kretannje kroz metadata/junk.
    */

	if (decoded_frames <= 0) {
		if (md->info.frame_bytes > 0) {
			mp3_decoder_consume_input_bytes(md, md->info.frame_bytes);
		} else if (md->input_bytes_available >= MP3_INPUT_FILL_TARGET) {
			mp3_decoder_consume_input_bytes(md, 1);
		}

		return 0;
	}


     /* Preuzeti info.frame_bytes bajtova */

	if (md->info.frame_bytes > 0)
		mp3_decoder_consume_input_bytes(md, md->info.frame_bytes);


     /* Preuzeti i postaviti dekodovan stream info */

	if (md->info.sample_rate > 0)
		md->sample_rate = md->info.sample_rate;

	if (md->info.channels > 0)
		md->channels = md->info.channels;


    /* Preostali PCM odbirci (u slucaju da nismo uspeli da ih upisemo sve)*/
    
	md->pending_pcm_frames = decoded_frames;
	md->pending_pcm_frame_offset = 0;
	md->pending_pcm_channels = md->info.channels;


    /* Upisati onoliko dekodovanih PCM odbiraka, koliko staje u output buffer */

	written_frames = mp3_decoder_write_pending_pcm(sink, md,
						       sink_frames_available);

	if (written_frames > 0)
		audio_stream_produce(sink, written_frames * sink_channels * sizeof(int16_t));

	return 0;

}

/* -------------------------------------------------------------------------- */
/* reset */
/* -------------------------------------------------------------------------- */

static int mp3_decoder_reset(struct processing_module *mod)
{
    struct mp3_decoder_data *md = module_get_private_data(mod);

    if (!md)
        return 0;

    drmp3dec_init(&md->dec);


	memset(&md->info, 0, sizeof(md->info));
	memset(md->input_buffer, 0, sizeof(md->input_buffer));
	memset(md->pcm_buffer, 0, sizeof(md->pcm_buffer));

    md->input_bytes_available = 0;
    md->sample_rate = 0;
    md->channels = 0;

    comp_dbg(mod->dev, "mp3_decoder_reset");

    return 0;
}

/* -------------------------------------------------------------------------- */
/* free */
/* -------------------------------------------------------------------------- */

static int mp3_decoder_free(struct processing_module *mod)
{
    struct mp3_decoder_data *md = module_get_private_data(mod);

    if (md)
        rfree(md);

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



