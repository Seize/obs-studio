#include "rtmp-server-impl.h"

extern server_t rtmp_server_output_instance;

////////////////////////////////////////////////////////////////////////////////
// Queue Reader                                                               //
////////////////////////////////////////////////////////////////////////////////

int QueueReaderInit(queue_reader_t* q, queue_t* queue) {
	q->init = true;
	q->queue = queue;
	q->curr_buf = queue->curr_buf;
	int ret = pthread_mutex_init(&q->mutex, NULL);
	return ret;
}

int QueueReaderDestroy(queue_reader_t* q) {
	q->init = false;
	int ret = pthread_mutex_destroy(&q->mutex);
	return ret;
}

queue_buf_t* QueueReaderRead(queue_reader_t* q) {
	queue_buf_t* buf = NULL;

	pthread_mutex_lock(&q->mutex);
	if(q->curr_buf == q->queue->curr_buf) {
		// Not ready
		// rstrace("not ready");
	} else {
		// We must copy output packets because there might be multiple readers
		buf = malloc(sizeof(queue_buf_t));
		buf->size = q->curr_buf->size;
		buf->pts = q->curr_buf->pts;
		buf->dts = q->curr_buf->dts;
		buf->type = q->curr_buf->type;

		buf->data = malloc(q->curr_buf->size);
		memcpy(buf->data, q->curr_buf->data, q->curr_buf->size);

		q->curr_buf = q->curr_buf->next;
	}
	pthread_mutex_unlock(&q->mutex);

	return buf;
}

bool QueueReaderCheckOverlap(queue_reader_t* q) {
	bool drop = false;

	pthread_mutex_lock(&q->mutex);
	if(q->curr_buf == q->queue->curr_buf->next) {
		q->curr_buf = q->curr_buf->next;
		drop = true;
	}
	pthread_mutex_unlock(&q->mutex);

	return drop;
}

////////////////////////////////////////////////////////////////////////////////
// Queue                                                                      //
////////////////////////////////////////////////////////////////////////////////

int QueueInit(queue_t* q) {
	int ret = pthread_mutex_init(&q->mutex, NULL);
	if(ret != 0) {
		return 1;
	}

	memset(q->buffers, 0, NB_QUEUE_BUF * sizeof(queue_buf_t));
	memset(q->readers, 0, NB_OUTPUTS * sizeof(queue_reader_t));

	// Init circular pointers
	for(int i=0; i<NB_QUEUE_BUF-1; i++) {
		q->buffers[i].data = malloc(BUF_SIZE);
		q->buffers[i].next = &q->buffers[i+1];
	}

	q->buffers[NB_QUEUE_BUF-1].data = malloc(BUF_SIZE);
	q->buffers[NB_QUEUE_BUF-1].next = &q->buffers[0];

	q->curr_buf = &q->buffers[0];

	return 0;
}

int QueueDestroy(queue_t* q) {
	int ret;

	for(int i=0; i<NB_OUTPUTS; i++) {
		if(q->readers[i].init) {
			ret = QueueReaderDestroy(&q->readers[i]);
			if(ret != 0) {
				rserror("QueueReaderDestroy failed, ret=%d", ret);
			}
		}
	}

	for(int i=0; i<NB_QUEUE_BUF; i++) {
		free(q->buffers[i].data);
	}

	ret = pthread_mutex_destroy(&q->mutex);
	return ret;
}

int QueueWriteBuffer(queue_t* q, queue_buf_t* buf, bool copy) {
	pthread_mutex_lock(&q->mutex);

	// Copy the packet info
	q->curr_buf->size = buf->size;
	q->curr_buf->pts = buf->pts;
	q->curr_buf->dts = buf->dts;
	q->curr_buf->type = buf->type;

	// Copy the packet data, or just pass the pointer
	if(copy) {
		memcpy(q->curr_buf->data, buf->data, buf->size);
	} else {
		q->curr_buf->data = buf->data;
	}

	// Check overlap with readers
	for(int i=0; i<NB_OUTPUTS; i++) {
		if(q->readers[i].init) {
			bool drop = QueueReaderCheckOverlap(&q->readers[i]);
			if(drop) {
				rstrace("reader %d circular buffer overlap, dropped buffer", i);
			}
		}
	}

	// Next buffer to write
	q->curr_buf = q->curr_buf->next;
	pthread_mutex_unlock(&q->mutex);

	return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Server Output                                                              //
////////////////////////////////////////////////////////////////////////////////

int OutputStart(output_t* o, int idx, queue_t* queue, struct rtmp_server_handler_t* handler, socket_t socket) {
	int ret;
	struct sockaddr_storage ss;
	socklen_t n;

	memset(o, 0, sizeof(output_t));
	o->init = true;

	ret = pthread_mutex_init(&o->mutex, NULL);
	if(ret != 0) {
		return ret;
	}

	o->client_socket = socket_accept(socket, &ss, &n);
	if(o->client_socket == -1) {
		perror("OutputStart: socket_accept failed");
		return o->client_socket;
	}

	o->queue_reader = &queue->readers[idx];
	ret = QueueReaderInit(o->queue_reader, queue);
	if(ret != 0) {
		rserror("QueueReaderInit failed, ret=%d", ret);
		return ret;
	}

	o->muxer = flv_muxer_create(&OutputFLVHandler, o);

	o->s_rtmp = rtmp_server_create(&o->client_socket, handler);
	if(o->s_rtmp == NULL) {
		rserror("rtmp_server_create failed");
		return 1;
	}

	thread_create(&o->threadReadClient, OutputReadClientThread, o);
	return 0;
}

int OutputStop(output_t* o) {
	int ret;

	o->quitReadClientThread = true;
	thread_destroy(o->threadReadClient);

	socket_close(o->client_socket);
	QueueReaderDestroy(o->queue_reader);
	rtmp_server_destroy(o->s_rtmp);
	flv_muxer_destroy(o->muxer);

	ret = pthread_mutex_destroy(&o->mutex);
	o->init = false;
	return ret;
}

int STDCALL OutputReadClientThread(void* param) {
	int ret, bytes;
	unsigned char* packet;
	pthread_t threadSendPkt;

	output_t* o = (output_t*) param;
	if(o == NULL) {
		rserror("NULL context");
		return 1;
	}

	thread_create(&threadSendPkt, OutputSendPktThread, o);

	packet = malloc(2 * 1024 * 1024);

	pthread_mutex_lock(&o->mutex);
	do {
		pthread_mutex_unlock(&o->mutex);
		bytes = socket_recv(o->client_socket, packet, sizeof(packet), 0);

		pthread_mutex_lock(&o->mutex);
		if(bytes > 0) {
			ret = rtmp_server_input(o->s_rtmp, packet, bytes);
		}
	} while (!o->quitReadClientThread && bytes > 0 && ret == 0);
	o->quitSendPktThread = true;
	pthread_mutex_unlock(&o->mutex);

	free(packet);
	thread_destroy(threadSendPkt);
	rstrace("Client disconnected, stop output");
	OutputStop(o);
	return 0;
}

int STDCALL OutputSendPktThread(void* param) {
	int ret;
	bool quit = false;

	output_t* o = (output_t*) param;
	if(o == NULL) {
		rserror("NULL context");
		return 1;
	}

	pthread_mutex_lock(&o->mutex);
	while (!quit) {
		pthread_mutex_unlock(&o->mutex);

		queue_buf_t* buf = QueueReaderRead(o->queue_reader);
		if(buf == NULL) {
			system_sleep(10); // TODO signal, not sleep
			continue;
		}

		pthread_mutex_lock(&o->mutex);
		quit = o->quitSendPktThread;
		if(!quit) {
			if (FLV_TYPE_AUDIO == buf->type) {
				ret = flv_muxer_aac(o->muxer,  buf->data, buf->size, buf->pts, buf->dts);
			} else if (FLV_TYPE_VIDEO == buf->type) {
				ret = flv_muxer_avc(o->muxer, buf->data, buf->size, buf->pts, buf->dts);
			} else {
				rserror("unknown type of buffer ! buf=%p, type=%d, size=%zu, dts=%ld", buf->data, buf->type, buf->size, buf->dts);
			}

			if(ret != 0) {
				rserror("error while muxing pkt, ret=%d", ret);
			}
		}
		free(buf->data);
		free(buf);
	}
	o->quitSendPktThread = false;
	pthread_mutex_unlock(&o->mutex);
	return 0;
}

int OutputFLVHandler(void* param, int type, const void* data, size_t bytes, int64_t dts) {
	output_t* o = (output_t*) param;
	if(o == NULL) {
		rserror("NULL context");
		return 1;
	}

	if(bytes > BUF_SIZE) {
		rserror("buffer too large");
		return -1;
	}

	if(o->s_rtmp == NULL) {
		rstrace("RTMP output is not ready");
		return -1;
	}

	if(type == FLV_TYPE_AUDIO) {
		return rtmp_server_send_audio(o->s_rtmp, data, bytes, dts);
	} else if (type == FLV_TYPE_VIDEO) {
		return rtmp_server_send_video(o->s_rtmp, data, bytes, dts);
	} else if (type == FLV_TYPE_SCRIPT) {
		return rtmp_server_send_script(o->s_rtmp, data, bytes, dts);
	} else {
		rserror("unknown pkt type: %d", type);
		return -1;
	}
	return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Server                                                                     //
////////////////////////////////////////////////////////////////////////////////

int ServerCreate(server_t* s, int port,
		int (*send_cb)(void* param, const void* header, size_t len, const void* data, size_t bytes),
		int (*onplay_cb)(void* param, const char* app, const char* stream, double start, double duration, uint8_t reset),
		int (*onpause_cb)(void* param, int pause, uint32_t ms),
		int (*onseek_cb)(void* param, uint32_t ms),
		int (*ongetduration_cb)(void* param, const char* app, const char* stream, double* duration)) {
	int ret;
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

	for(int i=0; i< NB_OUTPUTS; i++) {
		s->outputs[i].init = false;
	}

	s->quitThreadWaitForClient = false;
	ret = QueueInit(&s->queue);
	if(ret != 0) {
		rserror("QueueInit failed, ret=%d", ret);
		return 1;
	}

	socket_init();
	s->socket = socket_tcp_listen(NULL, port, SOMAXCONN);
	if(s->socket == -1) {
		return s->socket;
	}
	return 0;
}

int ServerDestroy(server_t* s) {
	socket_close(s->socket);
	socket_cleanup();
	QueueDestroy(&s->queue);
	return 0;
}

int ServerStart(server_t* s) {
	thread_create(&s->threadWaitForClient, ServerWaitForClientThread, s);
	return 0;
}

int ServerStop(server_t* s) {
	int ret;
	s->quitThreadWaitForClient = true;
	thread_destroy(s->threadWaitForClient);

	for(int i=0; i<NB_OUTPUTS; i++) {
		if(s->outputs[i].init) {
			ret = OutputStop(&s->outputs[i]);
			if(ret != 0) {
				rserror("OutputStop failed, ret=%d", ret);
				continue;
			}
			s->outputs[i].init = false;
		}
	}
	return 0;
}

int STDCALL ServerWaitForClientThread(void* param) {
	int ret;

	server_t* s = (server_t*) param;
	if(s == NULL) {
		rserror("NULL context");
		return 1;
	}

	while(!s->quitThreadWaitForClient) {
		int rv;
		struct timeval timeout = { 1, 0 }; // 1 second timeout

		fd_set set;
		FD_ZERO(&set);
		FD_SET(s->socket, &set);

		rv = select(s->socket + 1, &set, NULL, NULL, &timeout);
		if(rv == -1) {
			perror("ServerWaitForClientThread: error in select");
			return 1;
		}
		else if(rv == 0) {
			// a timeout occured, retry
			continue;
		}
		else {
			// Find free output
			int i;
			for(i=0; i<NB_OUTPUTS; i++) {
				if(s->outputs[i].init == false) {
					break;
				}
			}
			if(i == NB_OUTPUTS) {
				rserror("no more free outputs");
			} else {
				rstrace("Client connected, start output %d", i);
				ret = OutputStart(&s->outputs[i], i, &s->queue, &s->handler, s->socket);
				if(ret != 0) {
					rserror("OutputStart failed");
				}
			}
		}
	}
	return 0;
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
	// rstrace("rtmp_server_onplay(%s, %s, %f, %f, %d)", app, stream, start, duration, (int)reset);
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(app);
	UNUSED_PARAMETER(stream);
	UNUSED_PARAMETER(start);
	UNUSED_PARAMETER(duration);
	UNUSED_PARAMETER(reset);
	return 0;
}

int rtmp_server_onpause(void* param, int pause, uint32_t ms) {
	// rstrace("rtmp_server_onpause(%d, %u)", pause, (unsigned int)ms);
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(pause);
	UNUSED_PARAMETER(ms);
	return 0;
}

int rtmp_server_onseek(void* param, uint32_t ms) {
	// rstrace("rtmp_server_onseek(%u)", (unsigned int)ms);
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(ms);
	return 0;
}

int rtmp_server_ongetduration(void* param, const char* app, const char* stream, double* duration) {
	// rstrace("");
	*duration = 30 * 60;
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(app);
	UNUSED_PARAMETER(stream);
	return 0;
}
