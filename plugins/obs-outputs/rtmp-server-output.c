/******************************************************************************
    Copyright (C) 2017 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <util/threading.h>
#include <obs-module.h>
#include "rtmp-server-impl.h"

#define RTMP_PORT 1935

// The server object must be global to be usable in the rtmp_server_* callbacks :(
server_t rtmp_server_output_instance;

struct rtmp_server_output {
	obs_output_t *output;

	pthread_t stop_thread;
	bool stop_thread_active;
};

static const char *rtmp_server_output_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "RTMP Server Encoding Output";
}

static void *rtmp_server_output_create(obs_data_t *settings, obs_output_t *output)
{
	struct rtmp_server_output *context = bzalloc(sizeof(*context));
	context->output = output;
	int ret = ServerCreate(&rtmp_server_output_instance, RTMP_PORT,
		&rtmp_server_send, &rtmp_server_onplay, &rtmp_server_onpause,
		&rtmp_server_onseek, &rtmp_server_ongetduration);
	if(ret) {
		rserror("server Create failed: ret=%d", ret);
		return context;
	}

	UNUSED_PARAMETER(settings);
	return context;
}

static void rtmp_server_output_destroy(void *data)
{
	struct rtmp_server_output *context = data;
	if (context->stop_thread_active)
		pthread_join(context->stop_thread, NULL);
	int ret = ServerDestroy(&rtmp_server_output_instance);
	if(ret) {
		rserror("server Destroy failed: ret=%d", ret);
	}
	bfree(context);
}

static bool rtmp_server_output_start(void *data)
{
	struct rtmp_server_output *context = data;

	if (!obs_output_can_begin_data_capture(context->output, 0))
		return false;
	if (!obs_output_initialize_encoders(context->output, 0))
		return false;

	if (context->stop_thread_active)
		pthread_join(context->stop_thread, NULL);

	int ret = ServerStart(&rtmp_server_output_instance);
	if(ret) {
		rserror("ServerStart failed: ret=%d", ret);
		return ret;
	}
	obs_output_begin_data_capture(context->output, 0);
	return true;
}

static void *stop_thread(void *data)
{
	struct rtmp_server_output *context = data;
	obs_output_end_data_capture(context->output);
	int ret = ServerStop(&rtmp_server_output_instance);
	if(ret) {
		rserror("ServerStop failed: ret=%d", ret);
	}
	context->stop_thread_active = false;
	return NULL;
}

static void rtmp_server_output_stop(void *data, uint64_t ts)
{
	struct rtmp_server_output *context = data;
	UNUSED_PARAMETER(ts);

	context->stop_thread_active = pthread_create(&context->stop_thread,
			NULL, stop_thread, data) == 0;
}

static void rtmp_server_output_data(void *data, struct encoder_packet *packet)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(packet);
	queue_buf_t buf;

	if(packet->size > BUF_SIZE) {
		rserror("packet too large");
		return;
	}

	if(packet->type == OBS_ENCODER_AUDIO) {
		buf.type = FLV_TYPE_AUDIO;
	} else if (packet->type == OBS_ENCODER_VIDEO) {
		buf.type = FLV_TYPE_VIDEO;
	} else {
		rserror("unknown packet type %d", packet->type);
		return;
	}

	// FLV requires ms timestamps
	int64_t pts_ms = (1000 * packet->pts * ((int64_t)packet->timebase_num)) / ((int64_t)packet->timebase_den);
	int64_t dts_ms = (1000 * packet->dts * ((int64_t)packet->timebase_num)) / ((int64_t)packet->timebase_den);

	// TODO why though
	if(packet->type == OBS_ENCODER_VIDEO) {
		pts_ms /= 1000;
		dts_ms /= 1000;
	}

	buf.data = packet->data;
	buf.size = packet->size;
	buf.pts = pts_ms;
	buf.dts = dts_ms;
	buf.next = NULL;

	// Write the buffer and memcpy the data
	QueueWriteBuffer(&rtmp_server_output_instance.queue, &buf, true);
}

struct obs_output_info rtmp_server_output_info = {
	.id                 = "rtmp_server_output",
	.flags              = OBS_OUTPUT_AV |
	                      OBS_OUTPUT_ENCODED,
	.get_name           = rtmp_server_output_getname,
	.create             = rtmp_server_output_create,
	.destroy            = rtmp_server_output_destroy,
	.start              = rtmp_server_output_start,
	.stop               = rtmp_server_output_stop,
	.encoded_packet     = rtmp_server_output_data
};
