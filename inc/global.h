#include "grading.h"
#ifndef _GLOBAL_H_
#define _GLOBAL_H_

#define EXIT_SUCCESS 0
#define EXIT_ERROR -1
#define EXIT_FAILURE 1

#define SIZE32 4
#define SIZE16 2
#define SIZE8  1

#define NO_FLAG 0
#define NO_WAIT 1
#define TIMEOUT 2

#define TRUE 1
#define FALSE 0

#define SEQMAX 0xffffffff

typedef struct {
	uint32_t last_seq_received; /* 与原来表示内容不同，此处相当于接收方的窗口base，表示当前希望收到的序列号 */
	uint32_t last_ack_received; /* 最近一次被确认的序列号，相当于发送方的窗口base */
	pthread_mutex_t ack_lock; /* 单线程的话不需要 */
	sent_pkt *sent_head; /* 带头结点，指的是如果链表为空，head->next=NULL */
	uint32_t sent_length; /* 发送但未被确认的数据字节数，nextseqnum = sent_length + last_ack_received；
							 sent_length 可能会大于 window_size */
	recv_pkt *recv_head;
	uint32_t recv_length; /* 缓存的数据长度 */
	pthread_mutex_t recv_lock; /* TCP收到数据，放进去；cmu_read读数据 */
	uint32_t rwnd; /* 流量控制 */
	uint32_t cwnd; /* 拥塞控制 */
	uint32_t rtt; /* RTT */
	uint32_t ssthresh; /* ssthresh */
	bool timer_on; /* 计时器是否设置 */
    control_status con_state;/* 用于拥塞控制算法表示状态 */
	 /* TimeoutInterval */
}slide_window_t;

/* 每一个包的起始地址和发送时间 */
typedef struct {
	char *pkt_start; /* 包括头部 */
	struct timeval sent_time; /* 用来计算RTT，单指第一次发送的时间 */
	sent_pkt *next;
}sent_pkt;

/* 缓存乱序到达的包的数据，此链表按照seq排序 */
typedef struct {
	char *data_start; /* 只有数据 */
	uint32_t seq; /* 数据的序列号 */
	uint32_t data_length; /* 数据长度 */
	bool adjacent; /* 表示与前面的数据包是否相邻，相邻为true
					（如果当前接收方正在等待的包，也为true）；
					考虑到维护的是有序链表，减少计算次数 */
	/* 因此cmu_read是从第一个true一直读到第一个false */
	recv_pkt *next;
}recv_pkt;

typedef struct {
	uint32_t last_seq_received;
	uint32_t last_ack_received;
	pthread_mutex_t ack_lock;
} window_t;

typedef enum {
	CLOSED, /* 0 */
	LISTEN, /* 1 */
	SYN_SENT, /* 2 */
	SYN_RECVD, /* 3 */
	ESTABLISHED, /* 4 */
	FIN_WAIT_1, /* 5 */
	FIN_WAIT_2, /* 6 */
	CLOSING, /* 7 */
	TIME_WAIT, /* 8 */
	CLOSE_WAIT, /* 9 */
	LAST_ACK /* 10 */
}states;

typedef enum {
    SLOW_STAR, /* 0 */
    CONG_AVOI, /* 1 */
    FAST_RECO  /* 2 */
}control_status;

typedef struct {
	int socket;   
	pthread_t thread_id;
	uint16_t my_port;
	uint16_t their_port;
	struct sockaddr_in conn;
	pthread_mutex_t recv_lock;
	pthread_cond_t wait_cond;
	char* sending_buf;
	int sending_len;
	int type;
	pthread_mutex_t send_lock;
	int dying;
	pthread_mutex_t death_lock;
	slide_window_t window;
	enum states state;
	uint32_t ISN;
	uint32_t FSN;
    int ack_dup;/* ack重复的数目 用于切换到快速重传 */

} cmu_socket_t;

#endif
