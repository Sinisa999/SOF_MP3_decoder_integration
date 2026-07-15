divert(-1)

dnl Define macro for MP3 decoder widget

dnl MP3_DECODER(name)
define(`N_MP3_DECODER', `MP3_DECODER'PIPELINE_ID`.'$1)

DECLARE_SOF_RT_UUID("mp3_decoder", mp3_decoder_uuid,
	0xc732b15b, 0x3f02, 0x4347,
	0xa4, 0xb6, 0x3a, 0x07, 0x76, 0x24, 0x6c, 0x14)

dnl W_MP3_DECODER(name, format, periods_sink, periods_source, core)
define(`W_MP3_DECODER',
`SectionVendorTuples."'N_MP3_DECODER($1)`_tuples_uuid" {'
`    tokens "sof_comp_tokens"'
`    tuples."uuid" {'
`            SOF_TKN_COMP_UUID "5b:b1:32:c7:02:3f:47:43:a4:b6:3a:07:76:24:6c:14"'
`    }'
`}'
`SectionData."'N_MP3_DECODER($1)`_data_uuid" {'
`    tuples "'N_MP3_DECODER($1)`_tuples_uuid"'
`}'
`SectionVendorTuples."'N_MP3_DECODER($1)`_tuples_w" {'
`    tokens "sof_comp_tokens"'
`    tuples."word" {'
`            SOF_TKN_COMP_PERIOD_SINK_COUNT'         STR($3)
`            SOF_TKN_COMP_PERIOD_SOURCE_COUNT'       STR($4)
`            SOF_TKN_COMP_CORE_ID'                   STR($5)
`    }'
`}'
`SectionData."'N_MP3_DECODER($1)`_data_w" {'
`    tuples "'N_MP3_DECODER($1)`_tuples_w"'
`}'
`SectionVendorTuples."'N_MP3_DECODER($1)`_tuples_str" {'
`    tokens "sof_comp_tokens"'
`    tuples."string" {'
`            SOF_TKN_COMP_FORMAT'    STR($2)
`    }'
`}'
`SectionData."'N_MP3_DECODER($1)`_data_str" {'
`    tuples "'N_MP3_DECODER($1)`_tuples_str"'
`}'
`SectionVendorTuples."'N_MP3_DECODER($1)`_tuples_str_type" {'
`    tokens "sof_process_tokens"'
`    tuples."string" {'
`            SOF_TKN_PROCESS_TYPE'   "MP3_DECODER"
`    }'
`}'
`SectionData."'N_MP3_DECODER($1)`_data_str_type" {'
`    tuples "'N_MP3_DECODER($1)`_tuples_str_type"'
`}'
`SectionWidget."'N_MP3_DECODER($1)`" {'
`    index "'PIPELINE_ID`"'
`    type "effect"'
`    no_pm "true"'
`    data ['
`            "'N_MP3_DECODER($1)`_data_uuid"'
`            "'N_MP3_DECODER($1)`_data_w"'
`            "'N_MP3_DECODER($1)`_data_str"'
`            "'N_MP3_DECODER($1)`_data_str_type"'
`    ]'
`}')

divert(0)dnl