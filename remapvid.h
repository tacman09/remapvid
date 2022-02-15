#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <pthread.h>
#include <bcm_host.h>
#include <interface/vcos/vcos.h>
#include <interface/mmal/mmal.h>
#include <interface/mmal/mmal_logging.h>
#include <interface/mmal/mmal_buffer.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_util_params.h>
#include <interface/mmal/util/mmal_default_components.h>
#include <interface/mmal/util/mmal_connection.h>
#include <interface/mmal/mmal_parameters_camera.h>

#include "vcsm_util.h"

#define	DEFAULT_BITRATE   10000000
#define DEFAULT_FRAMERATE 30
#define	DEFAULT_KEYFRAME  60

#define NUM_QPUS          12

typedef	struct {
	int camera_id;
	int camera_width;
	int camera_height;
	int camera_buffer_width;
	int camera_buffer_height;
	int video_width;
	int video_height;
	int video_buffer_width;
	int video_buffer_height;
	int keyframe;
	MMAL_STEREOSCOPIC_MODE_T stereo_mode;
	int bitrate;
	int framerate;
	int sensor_mode;
	int inline_header;
	int sps_timing;
	int hflip;
	int vflip;

	MMAL_COMPONENT_T *camera;
	MMAL_PORT_T *camera_video_port;
	MMAL_POOL_T *camera_video_pool;
	MMAL_COMPONENT_T *encoder;
	MMAL_PORT_T *encoder_input_port;
	MMAL_POOL_T *encoder_input_pool;
	MMAL_PORT_T *encoder_output_port;
	MMAL_POOL_T *encoder_output_pool;

	int mb;
	vcsm_util_program_t program;
	vcsm_util_buffer_t map;

	pthread_mutex_t mutex;
	VCOS_SEMAPHORE_T semaphore;
	MMAL_QUEUE_T *queue;

	FILE *output_file;
	FILE *map_file;
} CONTEXT_T;
