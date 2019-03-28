#include "rtmp-server-impl.h"

extern server_t rtmp_server_output_instance;

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

int QueueStopRead(queue_t* q) {
	printf("%s\n", __func__);
	q->reader_init = false;
	return 0;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

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
	s->handler.onplay = onplay_cb;
	s->handler.onpause = onpause_cb;
	s->handler.onseek = onseek_cb;
	s->handler.ongetduration = ongetduration_cb;
	//s->handler->oncreate_stream = oncreate_stream_cb;
	//s->handler->ondelete_stream = ondelete_stream_cb;
	//s->handler->onpublish = onpublish_cb;
	//s->handler->onvideo = onvideo_cb;
	//s->handler->onaudio = onaudio_cb;

	s->muxer = flv_muxer_create(&flv_handler, s);
	s->quitServe = false;
	s->quitOutput = false;
	QueueInit(&s->queue);
	printf("%s ok\n", __func__);
}

int ServerCreate(server_t* s, int port) {
	printf("%s\n", __func__);
	socket_init();

	printf("%s: socket_tcp_listen\n", __func__);
	s->s = socket_tcp_listen(NULL, port, SOMAXCONN);
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
	int ret = QueueStopRead(&s->queue);
	if(ret != 0) {
		printf("%s: QueueStopRead failed, ret=%d\n", __func__, ret);
	}
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

queue_buf_t* ServerPopBuffer(server_t* s) {
	return QueuePopFront(&s->queue);
}

int ServerSendPkt(server_t* s, unsigned char* data, size_t size, int type, int64_t dts) {
	if(size > BUF_SIZE) {
		printf("%s: buffer too large\n", __func__);
		return -1;
	}

	if(type == FLV_TYPE_AUDIO) {
		return rtmp_server_send_audio(s->s_rtmp, data, size, dts);
	} else if (type == FLV_TYPE_VIDEO) {
		return rtmp_server_send_video(s->s_rtmp, data, size, dts);
	} else if (type == FLV_TYPE_SCRIPT) {
		return rtmp_server_send_script(s->s_rtmp, data, size, dts);
	} else {
		printf("%s: unknown pkt type: %d\n", __func__, type);
		return -1;
	}
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

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
	while(!s->quitServe)
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
		printf("%s: socket closed\n", __func__);
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
		queue_buf_t* buf = ServerPopBuffer(s);
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
			// TODO flv_muxer_h264
		}
		else {
			printf("%s: unknown type of buffer ! buf=%p, type=%d, size=%zu, pts=%ld\n", __func__, buf->data, buf->type, buf->size, buf->pts);
		}

		if(ret != 0) {
			printf("%s: error while muxing pkt, ret=%d\n", __func__, ret);
		}
	}
	s->quitOutput = false;

	printf("%s: exit thread\n", __func__);
	return 0;
}

int flv_handler(void* param, int type, const void* data, size_t bytes, int64_t dts) {
	return ServerSendPkt((server_t*) param, (unsigned int*)data, bytes, type, dts);
}

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
	return ServerCreateOutputThread(&rtmp_server_output_instance);
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
