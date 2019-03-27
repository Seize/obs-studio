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

#include <stdio.h>
#include <stdint.h>
#include "sys/thread.h"
#include <pthread.h>


#include "sockutil.h"
#include "sys/system.h"
#include "flv-proto.h"
#include "flv-muxer.h"
#include "rtmp-server.h"

#define RTMP_PORT 1935

#define BUF_SIZE (8 * 1024 * 1024)
#define NB_PACKETS_BUF 32

typedef struct queue_buf {
	unsigned char* data;
	size_t size;
	uint32_t pts;
	uint32_t dts;
	int type;
	bool readable;
} queue_buf_t;

typedef struct queue {
	queue_buf_t buffers[NB_PACKETS_BUF];
	unsigned short rd_idx;
	unsigned short wr_idx;
	bool reader_init;
	pthread_mutex_t mutex;
} queue_t;

void QueueInit(queue_t* q) {
	q->rd_idx = 0;
	q->wr_idx = 0;
	q->reader_init = false;
	int rc = pthread_mutex_init(&q->mutex, NULL);
	assert(rc == 0);
	for(int i=0; i<NB_PACKETS_BUF; i++) {
		q->buffers[i].data = malloc(BUF_SIZE);
	}
}

void QueueDestroy(queue_t* q) {
	for(int i=0; i<NB_PACKETS_BUF; i++) {
		free(q->buffers[i].data);
	}
}

queue_buf_t* QueuePopFrontInternal(queue_t* q, unsigned short* idx) {
	queue_buf_t* buf = NULL;

	if(q->rd_idx == q->wr_idx) {
		return NULL;
	}

	if(!q->reader_init) {
		q->rd_idx = q->wr_idx-1;
		q->reader_init = true;
	}

	buf = &q->buffers[q->rd_idx];
	if(!buf->readable) {
		return NULL;
	}
	*idx = q->rd_idx;

	q->rd_idx++;
	if(q->rd_idx >= NB_PACKETS_BUF) {
		q->rd_idx = 0;
	}

	buf->readable = false;
	return buf;
}

queue_buf_t* QueueGetWriteBuffer(queue_t* q) {
	queue_buf_t* buf = NULL;
	unsigned short idx;

	pthread_mutex_lock(&q->mutex);
	buf = &q->buffers[q->wr_idx];
	idx = q->wr_idx;

	if(q->reader_init && q->wr_idx+1 == q->rd_idx) {
		printf("%s: drop old buffer at id=%d\n", __func__, q->rd_idx);
		unsigned short rd_idx;
		QueuePopFrontInternal(q, &rd_idx);
	}

	q->wr_idx++;
	if(q->wr_idx >= NB_PACKETS_BUF) {
		q->wr_idx = 0;
	}
	pthread_mutex_unlock(&q->mutex);

	// printf("%s: got buffer %d\n", __func__, idx);
	return buf;
}

queue_buf_t* QueuePopFront(queue_t* q) {
	queue_buf_t* buf = NULL;
	unsigned short idx;

	pthread_mutex_lock(&q->mutex);
	buf = QueuePopFrontInternal(q, &idx);
	pthread_mutex_unlock(&q->mutex);

	if(buf == NULL) {
		// printf("%s: no buffer yet\n", __func__);
	} else {
		// printf("%s: read buffer %d\n", __func__, idx);
	}
	return buf;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

typedef struct server {
	struct rtmp_server_handler_t handler;
	rtmp_server_t* s_rtmp;
    struct flv_muxer_t* muxer;
	queue_t queue;
	pthread_t threadListener;
	pthread_t threadOutput;
	socket_t s;
	socket_t c;
	bool quitServe;
	bool quitOutput;
} server_t;


int ServerServe(server_t* s);
int ServerCreateOutputThread(server_t* s);
queue_buf_t* ServerGetBuffer(server_t* s);
queue_buf_t* ServerGetPkt(server_t* s);
int ServerSendAudioPkt(server_t* s, queue_buf_t* buf);
int ServerSendVideoPkt(server_t* s, queue_buf_t* buf);
int ServerSendScriptPkt(server_t* s, queue_buf_t* buf);

int STDCALL listener_thread(void* param) {
	printf("%s: start\n", __func__);

	struct sockaddr_storage ss;
	socklen_t n;
	server_t* s = (server_t*) param;
	if(s == NULL) {
		printf("%s: NULL context\n", __func__);
		return 1;
	}

	// TODO don't exit on client quit
	// TODO manage multiple clients
	//while(!s->quitServe)
	{
		printf("%s: socket_accept\n", __func__);
		s->c = socket_accept(s->s, &ss, &n);
		if(s->c == -1) {
			printf("%s: socket_accept failed\n", __func__);
			return s->c;
		}

		printf("%s: rtmp_server_create\n", __func__);
		s->s_rtmp = rtmp_server_create(&s->c, &s->handler);
		if(s->s_rtmp == NULL) {
			printf("%s: rtmp_server_create failed\n", __func__);
			return 1;
		}

		ServerServe(s);
	}
	printf("%s: exit thread\n", __func__);
	return 0;
}

int STDCALL output_thread(void* param) {
	int ret;
	printf("%s: start\n", __func__);

	server_t* s = (server_t*) param;
	if(s == NULL) {
		printf("%s: NULL context\n", __func__);
		return 1;
	}

	while (!s->quitOutput) {
		queue_buf_t* buf = ServerGetPkt(s);
		if(buf == NULL) {
			system_sleep(20); // TODO signal, not sleep
			continue;
		}

		// printf("%s: send buf=%p, type=%d, size=%zu, pts=%u\n", __func__, buf->data, buf->type, buf->size, buf->pts);
		if (FLV_TYPE_AUDIO == buf->type) {
			ret = flv_muxer_aac(s->muxer,  buf->data, buf->size, buf->pts, buf->dts);
		}
		else if (FLV_TYPE_VIDEO == buf->type) {
			ret = flv_muxer_avc(s->muxer, buf->data, buf->size, buf->pts, buf->dts);
		}
		else {
			printf("%s: unknown type of buffer ! buf=%p, type=%d, size=%zu, pts=%u\n", __func__, buf->data, buf->type, buf->size, buf->pts);
		}

		if(ret != 0) {
			printf("%s: error while muxing pkt, ret=%d\n", __func__, ret);
		}
	}

	printf("%s: exit thread\n", __func__);
	return 0;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////


int flv_handler(void* param, int type, const void* data, size_t bytes, uint32_t timestamp);

// The server object must be global to be usable in the rtmp_server_* callbacks :(
server_t server;

void ServerInit(server_t* s,
		int (*send_cb)(void* param, const void* header, size_t len, const void* data, size_t bytes),
		int (*onplay_cb)(void* param, const char* app, const char* stream, double start, double duration, uint8_t reset),
		int (*onpause_cb)(void* param, int pause, uint32_t ms),
		int (*onseek_cb)(void* param, uint32_t ms),
		int (*ongetduration_cb)(void* param, const char* app, const char* stream, double* duration)
		) {
	printf("%s\n", __func__);

	memset(&s->handler, 0, sizeof(s->handler));
	s->handler.send = send_cb;
	//s->handler->oncreate_stream = oncreate_stream_cb;
	//s->handler->ondelete_stream = ondelete_stream_cb;
	s->handler.onplay = onplay_cb;
	s->handler.onpause = onpause_cb;
	s->handler.onseek = onseek_cb;
	//s->handler->onpublish = onpublish_cb;
	//s->handler->onvideo = onvideo_cb;
	//s->handler->onaudio = onaudio_cb;
	s->handler.ongetduration = ongetduration_cb;


	s->muxer = flv_muxer_create(&flv_handler, s);

	s->quitServe = false;
	s->quitOutput = false;
	QueueInit(&s->queue);
	printf("%s ok\n", __func__);
}

int flv_handler(void* param, int type, const void* data, size_t bytes, uint32_t timestamp)
{
	queue_buf_t buf;
	if(bytes > BUF_SIZE) {
		printf("%s: buffer too large\n", __func__);
		return 1;
	}

	// printf("%s: type=%d, bytes=%zu, ts=%u\n", __func__, type, bytes, timestamp);
	buf.data = data; // TODO is input data buffer persistent enough ?
	buf.size = bytes;
	buf.pts = timestamp; // TODO how to make dts ?
	buf.type = type;
	buf.readable = false; // TODO ?

	switch (type)
	{
	case FLV_TYPE_SCRIPT:
		// printf("FLV_TYPE_SCRIPT\n");
		return ServerSendScriptPkt(&server, &buf);
	case FLV_TYPE_AUDIO:
		// printf("FLV_TYPE_AUDIO\n");
		return ServerSendAudioPkt(&server, &buf);
	case FLV_TYPE_VIDEO:
		// printf("FLV_TYPE_VIDEO\n");
		return ServerSendVideoPkt(&server, &buf);
	default:
		// printf("unknown pkt type: %d\n", type);
		return -1;
	}
}

int ServerCreate(server_t* s) {
	printf("%s\n", __func__);

	socket_init();

	printf("%s: socket_tcp_listen\n", __func__);
	s->s = socket_tcp_listen(NULL, RTMP_PORT, SOMAXCONN);
	if(s->s == -1) {
		printf("%s: socket_tcp_listen failed\n", __func__);
		return s->s;
	}

	printf("%s ok\n", __func__);
	return 0;
}

int ServerDestroy(server_t* s) {
	printf("%s\n", __func__);
	rtmp_server_destroy(s->s_rtmp);
	flv_muxer_destroy(s->muxer);
	socket_close(s->c);
	socket_close(s->s);
	socket_cleanup();
	QueueDestroy(&s->queue);
	printf("%s ok\n", __func__);
	return 0;
}

int ServerStart(server_t* s) {
	printf("%s\n", __func__);
	thread_create(&s->threadListener, listener_thread, s);
	printf("%s ok\n", __func__);
	return 0;
}

int ServerStop(server_t* s) {
	printf("%s\n", __func__);
	s->quitOutput = true;
	thread_destroy(s->threadOutput);
	s->quitServe = true;
	thread_destroy(s->threadListener);
	printf("%s ok\n", __func__);
	return 0;
}

int ServerServe(server_t* s) {
	printf("%s\n", __func__);
	int ret;
	unsigned char packet[2 * 1024 * 1024];
	while (!s->quitServe && (ret = socket_recv(s->c, packet, sizeof(packet), 0)) > 0) {
		ret = rtmp_server_input(s->s_rtmp, packet, ret);
	}
	s->quitOutput = true;
	printf("%s: exit loop\n", __func__);
	printf("%s ok\n", __func__);
	return 0;
}

int ServerCreateOutputThread(server_t* s) {
	printf("%s\n", __func__);
	thread_create(&s->threadOutput, output_thread, s);
	printf("%s ok\n", __func__);
	return 0;
}

queue_buf_t* ServerGetBuffer(server_t* s) {
	return QueueGetWriteBuffer(&s->queue);
}

queue_buf_t* ServerGetPkt(server_t* s) {
	return QueuePopFront(&s->queue);
}

int ServerSendAudioPkt(server_t* s, queue_buf_t* buf) {
	return rtmp_server_send_audio(s->s_rtmp, buf->data, buf->size, buf->pts); // TODO pts or dts ?
}

int ServerSendVideoPkt(server_t* s, queue_buf_t* buf) {
	return rtmp_server_send_video(s->s_rtmp, buf->data, buf->size, buf->pts); // TODO pts or dts ?
}

int ServerSendScriptPkt(server_t* s, queue_buf_t* buf) {
	return rtmp_server_send_script(s->s_rtmp, buf->data, buf->size, buf->pts); // TODO pts or dts ?
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

int rtmp_server_send(void* param, const void* header, size_t len, const void* data, size_t bytes) {
	socket_t* socket = (socket_t*)param;
	socket_bufvec_t vec[2];
	socket_setbufvec(vec, 0, (void*)header, len);
	socket_setbufvec(vec, 1, (void*)data, bytes);
	return socket_send_v_all_by_time(*socket, vec, bytes > 0 ? 2 : 1, 0, 20000);
}

int rtmp_server_onplay(void* param, const char* app, const char* stream, double start, double duration, uint8_t reset) {
	printf("rtmp_server_onplay(%s, %s, %f, %f, %d)\n", app, stream, start, duration, (int)reset);
	UNUSED_PARAMETER(param);
	return ServerCreateOutputThread(&server);
}

int rtmp_server_onpause(void* param, int pause, uint32_t ms) {
	printf("rtmp_server_onpause(%d, %u)\n", pause, (unsigned int)ms);
	UNUSED_PARAMETER(param);
	return 0;
}

int rtmp_server_onseek(void* param, uint32_t ms) {
	printf("rtmp_server_onseek(%u)\n", (unsigned int)ms);
	UNUSED_PARAMETER(param);
	return 0;
}

int rtmp_server_ongetduration(void* param, const char* app, const char* stream, double* duration) {
	*duration = 30 * 60;
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(app);
	UNUSED_PARAMETER(stream);
	return 0;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

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
	printf("%s\n", __func__);
	context->output = output;
	int ret = ServerCreate(&server);
	if(ret) {
		printf("%s: server Create failed: ret=%d\n", __func__, ret);
		return context;
	}

	ServerInit(&server,
		&rtmp_server_send, &rtmp_server_onplay, &rtmp_server_onpause, &rtmp_server_onseek, &rtmp_server_ongetduration
	);
	UNUSED_PARAMETER(settings);
	printf("%s ok\n", __func__);
	return context;
}

static void rtmp_server_output_destroy(void *data)
{
	printf("%s\n", __func__);
	struct rtmp_server_output *context = data;
	if (context->stop_thread_active)
		pthread_join(context->stop_thread, NULL);
	int ret = ServerDestroy(&server);
	if(ret) {
		printf("%s: server Destroy failed: ret=%d\n", __func__, ret);
	}
	bfree(context);
	printf("%s ok\n", __func__);
}

static bool rtmp_server_output_start(void *data)
{
	printf("%s\n", __func__);
	struct rtmp_server_output *context = data;

	if (!obs_output_can_begin_data_capture(context->output, 0))
		return false;
	if (!obs_output_initialize_encoders(context->output, 0))
		return false;

	if (context->stop_thread_active)
		pthread_join(context->stop_thread, NULL);

	int ret = ServerStart(&server);
	if(ret) {
		printf("%s: server Start failed: ret=%d\n", __func__, ret);
		return ret;
	}
	obs_output_begin_data_capture(context->output, 0);
	printf("%s ok\n", __func__);
	return true;
}

static void *stop_thread(void *data)
{
	struct rtmp_server_output *context = data;
	obs_output_end_data_capture(context->output);
	int ret = ServerStop(&server);
	if(ret) {
		printf("%s: server Stop failed: ret=%d\n", __func__, ret);
	}
	context->stop_thread_active = false;
	return NULL;
}

static void rtmp_server_output_stop(void *data, uint64_t ts)
{
	printf("%s\n", __func__);
	struct rtmp_server_output *context = data;
	UNUSED_PARAMETER(ts);

	context->stop_thread_active = pthread_create(&context->stop_thread,
			NULL, stop_thread, data) == 0;
	printf("%s ok\n", __func__);
}

static void rtmp_server_output_data(void *data, struct encoder_packet *packet)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(packet);

	queue_buf_t* buf = ServerGetBuffer(&server);
	if(buf == NULL) {
		printf("%s: error while getting buffer\n", __func__);
		return;
	}

	if(packet->size > BUF_SIZE) {
		printf("%s: packet too large\n", __func__);
		return;
	}

	if(packet->type == OBS_ENCODER_AUDIO) {
		buf->type = FLV_TYPE_AUDIO;
	} else if (packet->type == OBS_ENCODER_VIDEO) {
		buf->type = FLV_TYPE_VIDEO;
	} else {
		printf("%s: unknown packet type %d\n", __func__, packet->type);
		return;
	}

	memcpy(buf->data, packet->data, packet->size); // TODO noooo :(
	buf->size = packet->size;
	buf->pts = packet->pts;
	buf->dts = packet->dts;
	buf->readable = true;
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
