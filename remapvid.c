#include <getopt.h>
#include <sys/time.h>
#include <interface/vcsm/user-vcsm.h>

#include "remapvid.h"
#include "vcsm_util.h"
#include "qpu_util.h"
#include "mailbox.h"
#include "kernel.h"

uint32_t next_pow2(uint32_t x) {
	x--;
	x |= x>>1;
	x |= x>>2;
	x |= x>>4;
	x |= x>>8;
	x |= x>>16;
	x++;
	return x;
}

volatile bool is_running = true;

void send_all_buffers_in_pool(MMAL_PORT_T *port, MMAL_POOL_T *pool) {
	MMAL_BUFFER_HEADER_T *buffer;
	MMAL_STATUS_T status;
	while ((buffer = mmal_queue_get(pool->queue)) != NULL) {
		status = mmal_port_send_buffer(port, buffer);
		if (status != MMAL_SUCCESS) {
			fprintf(stderr, "ERROR: mmal_port_send_buffer failed\n");
		}
	}
}

void camera_control_port_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
	CONTEXT_T *context = (CONTEXT_T *)port->userdata;

	if (buffer->cmd == MMAL_EVENT_ERROR) {
		fprintf(stderr, "camera control error event has occured\n");
	} else {
		fprintf(stderr, "unknown camera control event: %d\n", buffer->cmd);
	}

	mmal_buffer_header_release(buffer);
	vcos_semaphore_post(&context->semaphore);
}

void camera_video_port_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
	CONTEXT_T *context = (CONTEXT_T *)(port->userdata);
	mmal_queue_put(context->queue, buffer);
	vcos_semaphore_post(&context->semaphore);
}

bool setup_camera(CONTEXT_T *context) {
	MMAL_STATUS_T status;
	MMAL_COMPONENT_T *camera;
	MMAL_ES_FORMAT_T *format;
	MMAL_PORT_T *camera_preview_port;
	MMAL_PORT_T *camera_video_port;
	MMAL_POOL_T *camera_video_pool;

	status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr,	"ERROR: failed to create camera\n");
		return false;
	}

	context->camera = camera;
	camera_preview_port = camera->output[0];
	camera_video_port = camera->output[1];
	context->camera_video_port = camera_video_port;

	MMAL_PARAMETER_STEREOSCOPIC_MODE_T stereo = {
		{MMAL_PARAMETER_STEREOSCOPIC_MODE, sizeof(stereo)},
		context->stereo_mode,
		MMAL_FALSE,
		MMAL_FALSE
	};

	status = mmal_port_parameter_set(camera_preview_port, &stereo.hdr);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to set stereo mode on preview port\n");
		return false;
	}

	status = mmal_port_parameter_set(camera_video_port, &stereo.hdr);
	if (status != MMAL_SUCCESS)	{
		fprintf(stderr, "ERROR: failed to set stereo mode on video port\n");
		return false;
	}

	MMAL_PARAMETER_MIRROR_T mirror = {{MMAL_PARAMETER_MIRROR, sizeof(MMAL_PARAMETER_MIRROR_T)}, MMAL_PARAM_MIRROR_NONE};
	if (context->hflip && context->vflip)
		mirror.value = MMAL_PARAM_MIRROR_BOTH;
	else if (context->hflip)
		mirror.value = MMAL_PARAM_MIRROR_HORIZONTAL;
	else if (context->vflip)
		mirror.value = MMAL_PARAM_MIRROR_VERTICAL;
	status = mmal_port_parameter_set(camera_preview_port, &mirror.hdr);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to set mirror on preview port\n");
		return false;
	}
	status = mmal_port_parameter_set(camera_video_port, &mirror.hdr);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to set mirror on video port\n");
		return false;
	}

	MMAL_PARAMETER_INT32_T camera_num = {
		{MMAL_PARAMETER_CAMERA_NUM, sizeof(camera_num)}, context->camera_id
	};

	status = mmal_port_parameter_set(camera->control, &camera_num.hdr);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to select camera\n");
		return false;
	}

	status = mmal_port_parameter_set_uint32(camera->control, MMAL_PARAMETER_CAMERA_CUSTOM_SENSOR_CONFIG, context->sensor_mode);

	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to set sensor mode\n");
		return false;
	}

	camera->control->userdata = (struct MMAL_PORT_USERDATA_T *)context;

	status = mmal_port_enable(camera->control, camera_control_port_callback);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to enable control port\n");
		return false;
	}
	 
	MMAL_PARAMETER_CAMERA_CONFIG_T cam_config =	{
		{MMAL_PARAMETER_CAMERA_CONFIG, sizeof (cam_config)},
		.max_stills_w				= 2048,
		.max_stills_h				= 1440,
		.stills_yuv422				= 0,
		.one_shot_stills			= 0,
		.max_preview_video_w		= 2048,
		.max_preview_video_h		= 1440,
		.num_preview_video_frames	= 3,
		.stills_capture_circular_buffer_height	= 0,
		.fast_preview_resume		= 0,
		.use_stc_timestamp			= MMAL_PARAM_TIMESTAMP_MODE_RESET_STC
	};
	mmal_port_parameter_set(camera->control, &cam_config.hdr);

	format = camera_video_port->format;
	format->encoding = MMAL_ENCODING_YUYV;
	format->encoding_variant = MMAL_ENCODING_YUYV;
	format->es->video.width = context->camera_buffer_width;
	format->es->video.height = context->camera_buffer_height;
	format->es->video.crop.x = 0;
	format->es->video.crop.y = 0;
	format->es->video.crop.width = context->camera_width;
	format->es->video.crop.height = context->camera_height;
	format->es->video.frame_rate.num = context->framerate;
	format->es->video.frame_rate.den = 1;

	camera_video_port->buffer_num = 3;
	camera_video_port->buffer_size = (format->es->video.width * format->es->video.height * 2);

	status = mmal_port_parameter_set_boolean(camera_video_port,
            MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to set zero copy on camera video port\n");
		return false;
	}

	status = mmal_port_format_commit(camera_video_port);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to commit camera video port format\n");
		return false;
	}

	camera_video_pool = (MMAL_POOL_T *)mmal_port_pool_create(camera_video_port,
		camera_video_port->buffer_num, camera_video_port->buffer_size);
	context->camera_video_pool = camera_video_pool;
	camera_video_port->userdata = (struct MMAL_PORT_USERDATA_T *)context;

	status = mmal_port_enable(camera_video_port, camera_video_port_callback);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to enable camera video port\n");
		return false;
	}

	status = mmal_component_enable(camera);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to enable camera component\n");
		return false;
	}

	if (mmal_port_parameter_set_boolean(camera_video_port, MMAL_PARAMETER_CAPTURE, 1) != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to start capturing\n");
		return false;
	}

	return true;
}

void encoder_input_port_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
	CONTEXT_T *context = (CONTEXT_T *)(port->userdata);
	mmal_buffer_header_release(buffer);
	vcos_semaphore_post(&context->semaphore);
}

void encoder_output_port_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
	CONTEXT_T *context = (CONTEXT_T *)(port->userdata);
	mmal_buffer_header_mem_lock(buffer);

	if (context->output_file != (FILE *)NULL) {
		fwrite(buffer->data, 1, buffer->length, context->output_file);
		fflush(context->output_file);
	}

	mmal_buffer_header_mem_unlock(buffer);
	mmal_buffer_header_release(buffer);

	send_all_buffers_in_pool(port, context->encoder_output_pool);
}

bool setup_encoder(CONTEXT_T *context) {
	MMAL_STATUS_T status;
	MMAL_COMPONENT_T *encoder = 0;
	MMAL_PORT_T *encoder_input_port = NULL;
	MMAL_POOL_T *encoder_input_port_pool;
	MMAL_PORT_T *encoder_output_port = NULL;
	MMAL_POOL_T *encoder_output_port_pool;

	status	= mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &encoder);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to create preview\n");
		return false;
	}

	encoder_input_port = encoder->input[0];
	encoder_output_port = encoder->output[0];
	context->encoder_input_port = encoder_input_port;
	context->encoder_output_port = encoder_output_port;

	MMAL_ES_FORMAT_T *format;
	format = encoder_input_port->format;

	format->encoding = MMAL_ENCODING_I420;
	format->encoding_variant = MMAL_ENCODING_I420;
	format->es->video.width = context->video_buffer_width;
	format->es->video.height = context->video_buffer_height;
	format->es->video.crop.x = 0;
	format->es->video.crop.y = 0;
	format->es->video.crop.width = context->video_width;
	format->es->video.crop.height = context->video_height;
	format->es->video.frame_rate.num = context->framerate;
	format->es->video.frame_rate.den = 1;

	encoder_input_port->buffer_num = 3;
	encoder_input_port->buffer_size = encoder_input_port->buffer_size_recommended;

	status = mmal_port_parameter_set_boolean(encoder_input_port,
            MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to set zero copy\n");
		return false;
	}

	status = mmal_port_parameter_set_boolean(encoder_input_port,
            MMAL_PARAMETER_VIDEO_IMMUTABLE_INPUT, MMAL_TRUE);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to set immutable input\n");
		return false;
	}

	status	= mmal_port_format_commit(encoder_input_port);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to commit encoder input port format\n");
		return false;
	}

	mmal_format_copy(encoder_output_port->format, encoder_input_port->format);

	encoder_output_port->format->encoding = MMAL_ENCODING_H264;
	encoder_output_port->format->bitrate = context->bitrate;

	encoder_output_port->buffer_size = encoder_output_port->buffer_size_recommended;
    if (encoder_output_port->buffer_size < encoder_output_port->buffer_size_min) {
        encoder_output_port->buffer_size = encoder_output_port->buffer_size_min;
    }

    encoder_output_port->buffer_num = encoder_output_port->buffer_num_recommended;
    if (encoder_output_port->buffer_num < encoder_output_port->buffer_num_min) {
        encoder_output_port->buffer_num = encoder_output_port->buffer_num_min;
    }

	status = mmal_port_format_commit(encoder_output_port);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to commit encoder output port format\n");
		return false;
	}

	MMAL_PARAMETER_UINT32_T	param = {{ MMAL_PARAMETER_INTRAPERIOD, sizeof(param)}, context->keyframe};
	status = mmal_port_parameter_set(encoder_output_port, &param.hdr);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to set keyframe interval\n");
		return false;
	}

	status = mmal_port_parameter_set_boolean(encoder_output_port, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_HEADER, context->inline_header);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to set inline header\n");
		return false;
	}

	status = mmal_port_parameter_set_boolean(encoder_output_port, MMAL_PARAMETER_VIDEO_ENCODE_SPS_TIMING, context->sps_timing);
    if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to set sps timing\n");
		return false;
	}

	encoder_input_port_pool = (MMAL_POOL_T *)mmal_port_pool_create(encoder_input_port,
		encoder_input_port->buffer_num, encoder_input_port->buffer_size);
	context->encoder_input_pool = encoder_input_port_pool;
	encoder_input_port->userdata = (struct MMAL_PORT_USERDATA_T *)context;

	status = mmal_port_enable(encoder_input_port, encoder_input_port_callback);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to enable encoder input port\n");
		return false;
	}

	encoder_output_port_pool = (MMAL_POOL_T *)mmal_port_pool_create(encoder_output_port,
		encoder_output_port->buffer_num, encoder_output_port->buffer_size);
	context->encoder_output_pool = encoder_output_port_pool;
	encoder_output_port->userdata = (struct MMAL_PORT_USERDATA_T *)context;

	status = mmal_port_enable(encoder_output_port, encoder_output_port_callback);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "ERROR: failed to enable encoder output port\n");
		return false;
	}

	return true;
}

void remap_buffer(CONTEXT_T *context, MMAL_BUFFER_HEADER_T *input_buffer, MMAL_BUFFER_HEADER_T *output_buffer) {
	if (!is_running) {
		return;
	}

	pthread_mutex_lock(&context->mutex);

 	output_buffer->length = context->video_buffer_width * context->video_buffer_height * 3 / 2;
	output_buffer->offset = 0;
	output_buffer->flags = input_buffer->flags;
	output_buffer->pts = input_buffer->pts;
	output_buffer->dts = input_buffer->dts;
	*output_buffer->type = *input_buffer->type;

	vcsm_lock(context->map.handle);
	mmal_buffer_header_mem_lock(output_buffer);
	mmal_buffer_header_mem_lock(input_buffer);

	unsigned int vc_handle_input = vcsm_vc_hdl_from_ptr(input_buffer->data);
	unsigned int frameptr_input = mem_lock(context->mb, vc_handle_input);
	unsigned int vc_handle_output = vcsm_vc_hdl_from_ptr(output_buffer->data);
	unsigned int frameptr_output = mem_lock(context->mb, vc_handle_output);

    if (qpu_enable(context->mb, 1)) {
        fprintf(stderr, "ERROR: failed to enable QPU\n");
    }
    
    vcsm_lock(context->program.buffer.handle);

    unsigned int ptr = context->program.buffer.vc_mem_addr;
    unsigned vc_code = ptr + offsetof(vcsm_util_program_mmap_t, code);
    unsigned vc_uniforms = ptr + offsetof(vcsm_util_program_mmap_t, uniforms);

    for (int i = 0; i < context->program.num_qpus; ++i) {
      int offset = i * MAX_NUM_UNIFORMS;
	  unsigned int uniform_ptr = vc_uniforms + offset * sizeof(unsigned int);

      context->program.mmap->uniforms[offset++] = uniform_ptr;
      context->program.mmap->uniforms[offset++] = texture_config_0(frameptr_input, 0, 17); // texture config 0
      context->program.mmap->uniforms[offset++] = texture_config_1(context->camera_buffer_height, context->camera_buffer_width, 0, 0, 1, 1, 17); // texture config 1
      context->program.mmap->uniforms[offset++] = 0; // texture config 2
      context->program.mmap->uniforms[offset++] = 0; // texture config 3
      context->program.mmap->uniforms[offset++] = (unsigned int) i; // qpu id
      context->program.mmap->uniforms[offset++] = context->map.vc_mem_addr;
      context->program.mmap->uniforms[offset++] = frameptr_output; // pointer to frame buffer
      context->program.mmap->uniforms[offset++] = vpm_write_y_config((unsigned int) i); // vpm write y config
      context->program.mmap->uniforms[offset++] = vpm_write_uv_config((unsigned int) i); // vpm write uv config
      context->program.mmap->uniforms[offset++] = context->video_width / 128; // x tile count
      context->program.mmap->uniforms[offset++] = context->video_height / 12; // y tile count
      context->program.mmap->uniforms[offset++] = context->video_buffer_width; // frame buffer width
      context->program.mmap->uniforms[offset++] = context->video_buffer_height; // frame buffer height

      context->program.mmap->msg[2*i] = uniform_ptr;
      context->program.mmap->msg[2*i+1] = vc_code;
    }

    execute_qpu(context->mb, context->program.num_qpus, context->program.vc_msg, 1, 2000);
    
    vcsm_unlock_ptr(context->program.buffer.usr_mem_ptr);

    if (qpu_enable(context->mb, 0)) {
        fprintf(stderr, "ERROR: failed to disable QPU\n");
    }

	mem_unlock(context->mb, vc_handle_input);
	mem_unlock(context->mb, vc_handle_output);

	mmal_buffer_header_mem_unlock(input_buffer);
	mmal_buffer_header_mem_unlock(output_buffer);

	vcsm_unlock_ptr(context->map.usr_mem_ptr);

	pthread_mutex_unlock(&context->mutex);
}

void finalize(CONTEXT_T *context) {
	fprintf(stderr, "started finalizing...\n");

	is_running = false;

	pthread_mutex_lock(&context->mutex);

	if (context->output_file != NULL && context->output_file != stdout)
		fclose(context->output_file);

	if (context->map_file != NULL)
		fclose(context->map_file);

	vcsm_util_buffer_destroy(&context->map);
	vcsm_util_program_destroy(&context->program);

	vcsm_exit();

	mbox_close(context->mb);

	pthread_mutex_unlock(&context->mutex);

	fprintf(stderr, "finalizing completed\n");
}

void signal_handler(int signal_number) {
	is_running = false;
}

void print_usage() {
	fprintf(stderr,
	"Usage: remapvid\n"
		"\t--map <string> : Map filename\n"
		"\t[--output <string>] : Video output destination (default: stdout)\n"
		"\t[--camera <0|1>] : Camera ID for use (default: 0)\n"
		"\t[--stereo] : Side-by-side stereo mode\n"
		"\t[--bitrate <integer>] : Video bitrate (default: 10000000)\n"
		"\t[--framerate <integer>] : Video framerate (default: 30)\n"
		"\t[--keyframe <integer>] : Video keyframe interval (default: 60)\n"
		"\t[--inline-header] : Add inline header\n"
		"\t[--sps-timing] : Add SPS timing\n"
		"\t[--hflip] : Horizontal flip\n"
		"\t[--vflip] : Vertical flip\n"
	);
}

bool parse_arg_as_int(char *arg, int *res) {
	char *endptr;
	errno = 0;
	int i = strtol(arg, &endptr, 10);
	if (errno)
		return false;
	if (endptr == arg)
		return false;
	if (*endptr != '\0')
		return false;
	*res = i;
	return true;
}

int main(int argc, char *argv[]) {
	int exit_code = EXIT_FAILURE;

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGPIPE, signal_handler);

	CONTEXT_T context = {0};
	context.framerate = DEFAULT_FRAMERATE;
	context.keyframe = DEFAULT_KEYFRAME;
	context.output_file = stdout;
	context.stereo_mode = MMAL_STEREOSCOPIC_MODE_NONE;
	context.bitrate = DEFAULT_BITRATE;

	pthread_mutex_init(&context.mutex, NULL);

	bcm_host_init();

	vcsm_init();

	context.mb = mbox_open();

	vcos_semaphore_create(&context.semaphore, "remapvid", 1);

	context.queue = mmal_queue_create();

	vcsm_util_program_create(&context.program, NUM_QPUS);
	vcsm_util_program_load_from_memory(&context.program, kernel_bin, kernel_bin_len);

	struct option long_options[] =
	{
		{"camera", required_argument, NULL, 'a'},
		{"map", required_argument, NULL, 'd'},
		{"output", required_argument, NULL, 'g'},
		{"help", no_argument, NULL, 'h'},
		{"stereo", no_argument, NULL, 'i'},
		{"bitrate", required_argument, NULL, 'j'},
		{"framerate", required_argument, NULL, 'k'},
		{"keyframe", required_argument, NULL, 'l'},
		{"sensor", required_argument, NULL, 'm'},
		{"inline-header", no_argument, NULL, 'n'},
		{"sps-timing", no_argument, NULL, 'o'},
		{"hflip", no_argument, NULL, 'p'},
		{"vflip", no_argument, NULL, 'q'},
		{NULL, 0, NULL, 0}
	};

	char *output_filename = NULL;
	char *map_filename = NULL;
	int ch, option_index;
	while ((ch = getopt_long_only(argc, argv, "a:d:g:hij:k:l:m:nop:", long_options, &option_index)) != -1) {
		switch (ch) {
		case 'a': // --camera
			if (!parse_arg_as_int(optarg, &context.camera_id)) {
				fprintf(stderr, "ERROR: invalid value for argument '--camera'\n");
				goto error;
			}
			break;
		case 'd': // --map
			map_filename = optarg;
			break;
		case 'g': // --output
			output_filename = optarg;
			break;
		case 'h': // --help
			print_usage();
        	return EXIT_FAILURE;
		case 'i': // --stereo
			context.stereo_mode = MMAL_STEREOSCOPIC_MODE_SIDE_BY_SIDE;
			break;
		case 'j': // --bitrate
			if (!parse_arg_as_int(optarg, &context.bitrate)) {
				fprintf(stderr, "ERROR: invalid value for argument '--bitrate'\n");
				goto error;
			}
			break;
		case 'k': // --framerate
			if (!parse_arg_as_int(optarg, &context.framerate)) {
				fprintf(stderr, "ERROR: invalid value for argument '--framerate'\n");
				goto error;
			}
			break;
		case 'l': // --keyframe
			if (!parse_arg_as_int(optarg, &context.keyframe)) {
				fprintf(stderr, "ERROR: invalid value for argument '--keyframe'\n");
				goto error;
			}
			break;
		case 'm': // --sensor
			if (!parse_arg_as_int(optarg, &context.sensor_mode)) {
				fprintf(stderr, "ERROR: invalid value for argument '--sensor'\n");
				goto error;
			}
			break;
		case 'n': // --inline-header
			context.inline_header = MMAL_TRUE;
			break;
		case 'o': // --sps-timing
			context.sps_timing = MMAL_TRUE;
			break;
		case 'p': // --hflip
			context.hflip = 1;
			break;
		case 'q': // --vflip
			context.vflip = 1;
			break;
		default:
			print_usage();
			goto error;
		}
	}

	if (optind < argc) {
		fprintf(stderr, "ERROR: unknown arguments:");
		while (optind < argc) {
			fprintf(stderr, " %s", argv[optind++]);
		}
		fprintf(stderr, "\n");
		goto error;
	}

	if (!map_filename) {
		fprintf(stderr, "ERROR: map filename is not specified\n");
		goto error;
	}

    context.map_file = fopen(map_filename, "rb");
    if (!context.map_file) {
        fprintf(stderr, "ERROR: failed to open file %s\n", map_filename);
        goto error;
    }
    fread(&context.video_width, 1, sizeof(int), context.map_file);
    fread(&context.video_height, 1, sizeof(int), context.map_file);
    fread(&context.camera_width, 1, sizeof(int), context.map_file);
    fread(&context.camera_height, 1, sizeof(int), context.map_file);

	fprintf(stderr, "map width: %d, height: %d\n", context.video_width, context.video_height);
	fprintf(stderr, "capture width: %d, height: %d\n", context.camera_width, context.camera_height);

	if (context.video_width % 128 != 0 || context.video_width > 1920) {
		fprintf(stderr, "ERROR: map width must be multiple of 128 and below 1920\n");
        goto error;
	}

	if (context.video_height % 12 != 0 || context.video_height > 1080) {
		fprintf(stderr, "ERROR: map height must be multiple of 12 and below 1080\n");
        goto error;
	}

	context.camera_buffer_width = next_pow2(context.camera_width);
	context.camera_buffer_height = context.camera_height;
	context.video_buffer_width = context.video_width;
	context.video_buffer_height = VCOS_ALIGN_UP(context.video_height, 16);

	if (output_filename) {
		context.output_file	= fopen(output_filename, "wb");
		if (!context.output_file) {
			fprintf(stderr, "ERROR: failed to open output file: %s\n", output_filename);
	        goto error;
		}
	}

	const size_t map_size = context.video_width * context.video_height * sizeof(unsigned int);
	vcsm_util_buffer_create(&context.map, map_size);
	if (!vcsm_util_buffer_load_from_file(&context.map, context.map_file, map_size)) {
		goto error;
	}

	if (!setup_camera(&context)) {
		goto error;
	}

	if (!setup_encoder(&context)) {
		goto error;
	}

	send_all_buffers_in_pool(context.encoder_output_port, context.encoder_output_pool);

	while (is_running) {
		MMAL_BUFFER_HEADER_T *buffer;
		VCOS_STATUS_T vcos_status;
		MMAL_STATUS_T status = MMAL_EINVAL;

		vcos_status = vcos_semaphore_wait_timeout(&context.semaphore, 2000);
		if (vcos_status != VCOS_SUCCESS) {
			fprintf(stderr, "ERROR: semaphore timed out\n");
			goto error;
		}

		while ((buffer = mmal_queue_get(context.queue)) != NULL) {
			MMAL_BUFFER_HEADER_T *enc_buffer = mmal_queue_get(context.encoder_input_pool->queue);
			if (enc_buffer) {
				remap_buffer(&context, buffer, enc_buffer);
				status = mmal_port_send_buffer(context.encoder_input_port, enc_buffer);
				if (status != MMAL_SUCCESS) {
					fprintf(stderr, "ERROR: mmal_port_send_buffer failed\n");
					goto error;
				}
			}
			mmal_buffer_header_release(buffer);
		}
		send_all_buffers_in_pool(context.camera_video_port, context.camera_video_pool);
	}

	exit_code = EXIT_SUCCESS;

error:
	finalize(&context);

	return exit_code;
}
