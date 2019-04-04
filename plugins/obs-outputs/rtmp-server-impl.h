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

#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)
#define rserror(fmt, args...) (printf("\e[41m%s:%d %s()\e[0m " fmt "\n", __FILENAME__, __LINE__, __func__, ##args))
#define rstrace(fmt, args...) (printf("\e[104m%s:%d %s()\e[0m " fmt "\n", __FILENAME__, __LINE__, __func__, ##args))

#define BUF_SIZE (8 * 1024 * 1024)
#define NB_QUEUE_BUF 32
#define NB_OUTPUTS 4

// Forward declarations
typedef struct queue_buf queue_buf_t;
typedef struct queue queue_t;

////////////////////////////////////////////////////////////////////////////////
// Queue Buffer                                                               //
////////////////////////////////////////////////////////////////////////////////

typedef struct queue_buf {
	unsigned char* data;
	size_t size;
	int64_t pts;
	int64_t dts;
	int type;
	queue_buf_t* next;
} queue_buf_t;

////////////////////////////////////////////////////////////////////////////////
// Queue Reader                                                               //
////////////////////////////////////////////////////////////////////////////////

typedef struct queue_reader {
	bool init;
	queue_t* queue;
	queue_buf_t* curr_buf;
	pthread_mutex_t mutex;
} queue_reader_t;

int QueueReaderInit(queue_reader_t* q, queue_t* queue);
int QueueReaderDestroy(queue_reader_t* q);
queue_buf_t* QueueReaderRead(queue_reader_t* q);
bool QueueReaderCheckOverlap(queue_reader_t* q);

////////////////////////////////////////////////////////////////////////////////
// Queue                                                                      //
////////////////////////////////////////////////////////////////////////////////

typedef struct queue {
	queue_buf_t buffers[NB_QUEUE_BUF];
	queue_buf_t* curr_buf;
	pthread_mutex_t mutex;
	queue_reader_t readers[NB_OUTPUTS];
} queue_t;

int QueueInit(queue_t* q);
int QueueDestroy(queue_t* q);

int QueueWriteBuffer(queue_t* q, queue_buf_t* buf, bool copy);

////////////////////////////////////////////////////////////////////////////////
// Server Output                                                              //
////////////////////////////////////////////////////////////////////////////////

typedef struct output {
	bool init;
	socket_t client_socket;
	queue_reader_t* queue_reader;
    struct flv_muxer_t* muxer;
	rtmp_server_t* s_rtmp;
	pthread_mutex_t mutex;
	pthread_t threadReadClient;
	bool quitReadClientThread;
	bool quitSendPktThread;
} output_t;

int OutputStart(output_t* o, int idx, queue_t* queue, struct rtmp_server_handler_t* base_handler, socket_t socket);
int OutputStop(output_t* o);

int STDCALL OutputReadClientThread(void* param);
int STDCALL OutputSendPktThread(void* param);

int OutputFLVHandler(void* param, int type, const void* data, size_t bytes, int64_t dts);

int rtmp_server_send(void* param, const void* header, size_t len, const void* data, size_t bytes);
int rtmp_server_onplay(void* param, const char* app, const char* stream, double start, double duration, uint8_t reset);
int rtmp_server_onpause(void* param, int pause, uint32_t ms);
int rtmp_server_onseek(void* param, uint32_t ms);
int rtmp_server_ongetduration(void* param, const char* app, const char* stream, double* duration);

////////////////////////////////////////////////////////////////////////////////
// Server                                                                     //
////////////////////////////////////////////////////////////////////////////////

typedef struct server {
	queue_t queue;
	socket_t socket;
	output_t outputs[NB_OUTPUTS];
	struct rtmp_server_handler_t handler; // Temp storage before outputs are created
	pthread_t threadWaitForClient;
	bool quitThreadWaitForClient;
} server_t;

int ServerCreate(server_t* s, int port,
		int (*send_cb)(void* param, const void* header, size_t len, const void* data, size_t bytes),
		int (*onplay_cb)(void* param, const char* app, const char* stream, double start, double duration, uint8_t reset),
		int (*onpause_cb)(void* param, int pause, uint32_t ms),
		int (*onseek_cb)(void* param, uint32_t ms),
		int (*ongetduration_cb)(void* param, const char* app, const char* stream, double* duration));
int ServerDestroy(server_t* s);
int ServerStart(server_t* s);
int ServerStop(server_t* s);

int STDCALL ServerWaitForClientThread(void* param);
