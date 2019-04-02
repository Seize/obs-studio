#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include "sys/thread.h"

#include "sockutil.h"
#include "sys/system.h"
#include "flv-proto.h"
#include "flv-muxer.h"
#include "rtmp-server.h"

#define UNUSED_PARAMETER(x) ((void)(x))

#define BUF_SIZE (8 * 1024 * 1024)
#define NB_PACKETS_BUF 32

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

typedef struct queue_buf {
	unsigned char* data;
	size_t size;
	int64_t pts;
	int64_t dts;
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

void QueueInit(queue_t* q);
void QueueDestroy(queue_t* q);
queue_buf_t* QueueGetWriteBuffer(queue_t* q);
queue_buf_t* QueuePopFrontInternal(queue_t* q, unsigned short* idx);
queue_buf_t* QueuePopFront(queue_t* q);

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

typedef struct server {
	struct rtmp_server_handler_t handler;
	rtmp_server_t* s_rtmp;
    struct flv_muxer_t* muxer;
	queue_t queue;
	pthread_t threadListener;
	pthread_t threadOutput;
	socket_t socket;
	socket_t client_socket;
	bool quitServe;
	bool quitOutput;
} server_t;

void ServerInit(server_t* s,
		int (*send_cb)(void* param, const void* header, size_t len, const void* data, size_t bytes),
		int (*onplay_cb)(void* param, const char* app, const char* stream, double start, double duration, uint8_t reset),
		int (*onpause_cb)(void* param, int pause, uint32_t ms),
		int (*onseek_cb)(void* param, uint32_t ms),
		int (*ongetduration_cb)(void* param, const char* app, const char* stream, double* duration)
		);

int ServerCreate(server_t* s, int port);
int ServerDestroy(server_t* s);
int ServerStart(server_t* s);
int ServerStop(server_t* s);
int ServerServe(server_t* s);
int ServerCreateOutputThread(server_t* s);
queue_buf_t* ServerGetBuffer(server_t* s);
queue_buf_t* ServerPopBuffer(server_t* s);
int ServerSendPkt(server_t* s, unsigned char* data, size_t size, int type, int64_t dts);

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

int STDCALL listener_thread(void* param);
int STDCALL output_thread(void* param);
int flv_handler(void* param, int type, const void* data, size_t bytes, int64_t dts);
int rtmp_server_send(void* param, const void* header, size_t len, const void* data, size_t bytes);
int rtmp_server_onplay(void* param, const char* app, const char* stream, double start, double duration, uint8_t reset);
int rtmp_server_onpause(void* param, int pause, uint32_t ms);
int rtmp_server_onseek(void* param, uint32_t ms);
int rtmp_server_ongetduration(void* param, const char* app, const char* stream, double* duration);
