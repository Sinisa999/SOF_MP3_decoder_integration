# Low Latency Playback Pipeline with MP3 decoder + volume
#
# Pipeline Endpoints:
#
# host PCM_P --> B0 --> MP3_DECODER --> B1 --> sink DAI0

include(`utils.m4')
include(`buffer.m4')
include(`pcm.m4')
include(`pga.m4')
include(`dai.m4')
include(`mixercontrol.m4')
include(`bytecontrol.m4')
include(`pipeline.m4')
include(`mp3_decoder.m4')



#
# Components
#

# Host playback PCM
W_PCM_PLAYBACK(PCM_ID, Passthrough Playback, 2, 0, SCHEDULE_CORE)

# MP3 decoder component
W_MP3_DECODER(0, PIPELINE_FORMAT, 8, 8, SCHEDULE_CORE)



#
# Buffers
#

# Host -> MP3 decoder
W_BUFFER(0, COMP_BUFFER_SIZE(8,
	COMP_SAMPLE_SIZE(PIPELINE_FORMAT), PIPELINE_CHANNELS, COMP_PERIOD_FRAMES(PCM_MAX_RATE, SCHEDULE_PERIOD)),
	PLATFORM_HOST_MEM_CAP, SCHEDULE_CORE)

# MP3 decoder -> output
W_BUFFER(1, COMP_BUFFER_SIZE(32,
	COMP_SAMPLE_SIZE(PIPELINE_FORMAT), PIPELINE_CHANNELS, COMP_PERIOD_FRAMES(PCM_MAX_RATE, SCHEDULE_PERIOD)),
	PLATFORM_HOST_MEM_CAP, SCHEDULE_CORE)


#
# Pipeline Graph
#
# host PCM_P --> B0 --> MP3_DECODER --> B1 --> DAI

P_GRAPH(pipe-mp3-decoder-volume-playback, PIPELINE_ID,
	LIST(`		',
	`dapm(N_BUFFER(0), N_PCMP(PCM_ID))',
	`dapm(N_MP3_DECODER(0), N_BUFFER(0))',
	`dapm(N_BUFFER(1), N_MP3_DECODER(0))'))

#
# Pipeline Source and PCM
#
indir(`define', concat(`PIPELINE_SOURCE_', PIPELINE_ID), N_BUFFER(1))
indir(`define', concat(`PIPELINE_PCM_', PIPELINE_ID), Passthrough Playback PCM_ID)

ifdef(`CHANNELS_MIN',`define(`LOCAL_CHANNELS_MIN', `CHANNELS_MIN')',
`define(`LOCAL_CHANNELS_MIN', `2')')

#
# PCM Configuration
#
PCM_CAPABILITIES(Passthrough Playback PCM_ID,
	CAPABILITY_FORMAT_NAME(PIPELINE_FORMAT),
	PCM_MIN_RATE, PCM_MAX_RATE,
	LOCAL_CHANNELS_MIN, PIPELINE_CHANNELS,
	2, 16, 192, 16384, 65536, 65536)

undefine(`LOCAL_CHANNELS_MIN')
undefine(`DEF_PGA_TOKENS')
undefine(`DEF_PGA_CONF')