#include "backend.h"
#include <math.h>

#define min(A, B) ((A) > (B) ? (B) : (A))
#define min3(A,B,C) (((A)> (B) ? (B) : (A)) > C) ? C : ((A) > (B) ? (B) : (A))

void handle_ack(cmu_socket_t * sock, char * pkt){
#ifdef DEBUG
    printf("%s", "handle_ack start");
    printf("\n");
#endif
    uint32_t sample_RTT;
    socklen_t conn_len = sizeof(sock -> conn);
    sent_pkt *pkts, *nexts;
    sock->window.rwnd = get_advertised_window(pkt);
    if(get_ack(pkt) > sock->window.last_ack_received){
        sock->ack_dup = 0;
        switch(sock->window.con_state){
            case SLOW_STAR: /* 慢启动 */
                sock->window.cwnd += MAX_DLEN;
                if(sock->window.cwnd >= sock->window.ssthresh) /* 当cwnd≥ssthresh，需要进入拥塞避免状态 */
                    sock->window.con_state = CONG_AVOI;
                break;
            case CONG_AVOI: /* 拥塞避免状态 */
                sock->window.cwnd += MAX_DLEN * MAX_DLEN/sock->window.cwnd;
                break;
            case FAST_RECO: /* 快速恢复 */
                sock->window.cwnd = sock->window.ssthresh;
                sock->window.con_state = CONG_AVOI;
                break;
            default:
                perror("ERROR unknown flag");
                return;
        }
        sock->window.last_ack_received = get_ack(pkt);
        sock->window.rwnd = get_advertised_window(pkt);
        pkts = sock->window.sent_head;
        while((nexts = pkts->next) != NULL && get_seq(nexts->pkt_start) < get_ack(pkt)){
            pkts->next = nexts->next;

            //确定该ack时，才更新rtt, devRtt, timeoutInterval
            if(get_seq(nexts->pkt_start) + get_plen(nexts->pkt_start) - get_hlen(nexts->pkt_start) == get_ack(pkt) && !nexts->is_resend){
                struct timeval arrival_time;
                gettimeofday(&arrival_time, NULL);
                sample_RTT = (arrival_time.tv_sec - nexts->sent_time.tv_sec) * 1000 + (arrival_time.tv_usec - nexts->sent_time.tv_usec) / 1000;
                if (sock->window.EstimatedRTT == 0){ /* 初始状态下，rtt=0 */
                    sock->window.EstimatedRTT = sample_RTT;
                } else{
                    sock->window.EstimatedRTT = alpha * sample_RTT + (1 - alpha) * sock->window.EstimatedRTT;
                    sock->window.DevRTT = (1 - beta) * sock->window.DevRTT + beta * abs(sock->window.EstimatedRTT - sample_RTT);
                }
                sock->window.TimeoutInterval = sock->window.EstimatedRTT + 4 * sock->window.DevRTT;
            }

            sock->window.sent_length -= (get_plen(nexts->pkt_start) - get_hlen(nexts->pkt_start));/* 每释放一个缓存pkt需要将sent_length减小 */
            free(nexts->pkt_start);
            free(nexts);
        }
    } else if(get_ack(pkt) == sock->window.last_ack_received){ /* 只考虑对窗口前一个包的重复ACK */
        sock->ack_dup += 1;
        if(sock->window.con_state == FAST_RECO){
            sock->window.cwnd += MAX_DLEN;/* 如果已经处于快速恢复状态，则加上一个mss */
        }
        if(sock->ack_dup == 3){
            /* 立即快速重传，并进入快速恢复状态 */
            if(sock->window.con_state != FAST_RECO){
                sock->window.ssthresh = sock->window.cwnd / 2;
                sock->window.cwnd = sock->window.ssthresh + 3 * MAX_DLEN;
                sock->window.con_state = FAST_RECO;
            }
            pkts = sock->window.sent_head;
            nexts = pkts->next;
            nexts->is_resend = TRUE;
            sendto(sock->socket, nexts->pkt_start, get_plen(nexts->pkt_start), 0, (struct sockaddr*) &(sock->conn), conn_len);/* 快速重传一定是传缓存链表的第一个包，此处主要看看语法对不对 */
            sock->ack_dup = 0;
        }
    }
#ifdef DEBUG
    printf("%s", "handle_ack over");
    printf("\n");
#endif
}

void handle_datapkt(cmu_socket_t * sock, char * pkt){
#ifdef DEBUG
    printf("%s", "handle_datapkt start");
    printf("\n");
#endif

    char *rsp;
    uint32_t seq, ack, rwnd;
    socklen_t conn_len = sizeof(sock -> conn);
    recv_pkt *pktr, *prevr, *nextr;
    bool contins;
    seq = get_seq(pkt);
    /* 如果不是之前接收到的包，需要缓存下来； */
    if(seq >= sock->window.last_seq_received /* 因为只有长度不为0的包才会占用序号并被缓存 */
       || ((seq == sock->ISN+1) && (sock->window.last_seq_received == sock->ISN+1))/* 考虑三次握手刚建立好连接的时候 */
            ){
        /* 生成链表中要存储的pkt，确定pktr在链表中的顺序之后才能确定adjacent */
        pktr = malloc(sizeof(recv_pkt));
        pktr->seq = seq;
        pktr->data_length = get_plen(pkt) - get_hlen(pkt);
        pktr->data_start = malloc(pktr->data_length);
        memcpy(pktr->data_start, pkt + get_hlen(pkt), pktr->data_length);
        /* 因为此函数返回之后，pkt会free，所以需要malloc新的空间来存储 */
        pktr->adjacent = (seq == sock->window.last_seq_received);
        pktr->next = NULL;
#ifdef DEBUG
    printf("handle_datapkt: 收到的数据： %s\n", pktr->data_start);
    printf("%s", "handle_datapkt: 正在缓存数据包……");
    printf("\n");
#endif
  
        sock->window.recv_length += pktr->data_length;

        prevr = sock->window.recv_head;
        nextr = prevr->next;

        if(nextr == NULL){
#ifdef DEBUG
    printf("%s", "handle_datapkt: 链表为空，插入中……");
    printf("\n");
#endif
            prevr->next = pktr;
        } else {
            contins = TRUE;
            while(nextr != NULL){
                if(nextr->seq > seq){ /* 找到插入的位置，通过更改指针插入 */
                    prevr->next = pktr;
                    pktr->next = nextr;
                    if(nextr->seq == seq + pktr->data_length){ /* 如果nextr与pkt相邻 */
                        nextr->adjacent = TRUE;
                    }
                }

                if(!nextr->adjacent){ /* 如果adjacent为FALSE，说明可以读的链表在此处断掉 */
                    contins = FALSE;
                }

                if(contins){ /* 如果从第一个包开始连续可读，那么更改last_seq_received */
                    sock->window.last_seq_received = prevr->seq + prevr->data_length;
                }

                prevr = prevr->next;
                nextr = prevr->next;
            }

            if(seq > prevr->seq){
                nextr = pktr;
            }
        }
    }

#ifdef DEBUG
    printf("\n此时链表中的数据\n");
    recv_pkt *pktd, *prevd;
    prevd = sock->window.recv_head;
    while((pktd = prevd->next) != NULL){
        printf("%s\n", pktd->data_start);
    }
    printf("\n链表中数据结束\n");
#endif

    seq = sock->window.last_ack_received + sock->window.sent_length;
    ack = sock->window.last_seq_received; /* 累积确认 */
    rwnd = MAX_NETWORK_BUFFER - sock->window.recv_length;
    rsp = create_packet_buf(sock->my_port, ntohs(sock->conn.sin_port), seq, ack,
                            DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, rwnd, 0, NULL, NULL, 0);
    sendto(sock->socket, rsp, DEFAULT_HEADER_LEN, 0, (struct sockaddr*)
            &(sock->conn), conn_len);
    free(rsp);

#ifdef DEBUG
    printf("%s", "handle_datapkt return");
    printf("\n");
#endif
}

/*
 * Param: sock - The socket used for handling packets received
 * Param: pkt - The packet data received by the socket
 *
 * Purpose: Updates the socket information to represent
 *  the newly received packet.
 *
 * Comment: This will need to be updated for checkpoints 1,2,3
 *
 */
void handle_message(cmu_socket_t * sock, char * pkt){
#ifdef DEBUG
    printf("%s", "handle_datapkt start");
    printf("\n");
#endif
    uint8_t flags = get_flags(pkt);
    switch(flags){
        case ACK_FLAG_MASK:
            handle_ack(sock, pkt);
            break;
        case ACK_FLAG_MASK | SYN_FLAG_MASK:
            break;
        case SYN_FLAG_MASK:
            break;
        case FIN_FLAG_MASK:
            break;
        default:
            handle_datapkt(sock, pkt);
            break;
    }
#ifdef DEBUG
    printf("%s", "handle_message return");
    printf("\n");
#endif
}


/*
 * Param: sock - The socket to check for acknowledgements.
 * Param: seq - Sequence number to check
 *
 * Purpose: To tell if a packet (sequence number) has been acknowledged.
 *
 */
int check_ack(cmu_socket_t * sock, uint32_t seq){
    int result;
    while(pthread_mutex_lock(&(sock->window.ack_lock)) != 0);
    if(sock->window.last_ack_received > seq)
        result = TRUE;
    else
        result = FALSE;
    pthread_mutex_unlock(&(sock->window.ack_lock));
    return result;
}

/*
 * Param: sock - The socket used for receiving data on the connection.
 * Param: flags - Signify different checks for checking on received data.
 *  These checks involve no-wait, wait, and timeout.
 *
 * Purpose: To check for data received by the socket.
 *
 */
void check_for_data(cmu_socket_t * sock, int flags){
#ifdef DEBUG
    printf("%s", "check_for_data start");
    printf("\n");
#endif

    char hdr[DEFAULT_HEADER_LEN];
    char* pkt;
    socklen_t conn_len = sizeof(sock->conn);
    ssize_t len = -1;
    uint32_t plen = 0, buf_size = 0, n = 0;
    fd_set ackFD;

    /* TODO： 三次握手阶段的timeout设置 */
    struct timeval time_out;
    time_out.tv_sec = 3;
    time_out.tv_usec = 0;

    while(pthread_mutex_lock(&(sock->window.recv_lock)) != 0);
    
        switch(flags){
            case NO_FLAG: /* wait */
                len = recvfrom(sock->socket, hdr, DEFAULT_HEADER_LEN, MSG_PEEK,
                               (struct sockaddr *) &(sock->conn), &conn_len);
                break;
            case TIMEOUT:/* 设定超时间隔 */
                FD_ZERO(&ackFD);
                FD_SET(sock->socket, &ackFD);
                if(select(sock->socket+1, &ackFD, NULL, NULL, &time_out) <= 0){
                    break;
                }
            case NO_WAIT:
                len = recvfrom(sock->socket, hdr, DEFAULT_HEADER_LEN, MSG_DONTWAIT | MSG_PEEK,
                               (struct sockaddr *) &(sock->conn), &conn_len);
                break;
            default:
                perror("ERROR unknown flag");
                return;
        }
        if(len >= DEFAULT_HEADER_LEN){//收到一个以上的包，但不知道是ack包还是数据包,在handle里判断
#ifdef DEBUG
    printf("%s", "check_for_data: ");
    printf("%s", "收到数据包的长度： ");
    printf("%d", get_plen(hdr) - get_hlen(hdr));
    printf("\n");
#endif
            plen = get_plen(hdr);
            pkt = malloc(plen);
            while(buf_size < plen){
                n = recvfrom(sock->socket, pkt + buf_size, plen - buf_size,
                         NO_FLAG, (struct sockaddr *) &(sock->conn), &conn_len);
                buf_size = buf_size + n;
            }

#ifdef DEBUG
    printf("%s", "check_for_data: ");
    printf("%s", "收到数据包的长度： ");
    printf("%d", plen - get_hlen(hdr));
    printf("\n");
#endif           
            handle_message(sock, pkt);
            free(pkt);
        }
    
    pthread_mutex_unlock(&(sock->window.recv_lock));

#ifdef DEBUG
    printf("%s", "check_for_data return");
    printf("\n");
#endif
}

/*
 * Param: sock - The socket to use for sending data
 * Param: data - The data to be sent
 * Param: buf_len - the length of the data being sent
 *
 * Purpose: Breaks up the data into packets and sends a single
 *  packet at a time.
 *
 * Comment: This will need to be updated for checkpoints 1,2,3
 *
 */
void single_send(cmu_socket_t * sock, char* data, int length){
    char* msg;
    int sockfd, plen;
    sent_pkt *pkts, *nexts;
    size_t conn_len = sizeof(sock->conn);
    uint32_t seq;
    sockfd = sock->socket;

    seq = sock->window.last_ack_received /* + sock->window.sent_length */;
    plen = DEFAULT_HEADER_LEN + length;
    msg = create_packet_buf(sock->my_port, sock->their_port, seq, seq, /* TODO：如果考虑发送的ACK带数据，此处需要改 */
                            DEFAULT_HEADER_LEN, plen, NO_FLAG, 1, 0, NULL, data, length);
    sendto(sockfd, msg, plen, 0, (struct sockaddr*) &(sock->conn), conn_len);

    sent_pkt *pkt_store;
    sock->window.sent_length += length; /* 储存在链表中的数据需要计算在sending_len中 */
    pkt_store = malloc(sizeof(sent_pkt)); /* 用于储存sent_pkt的结构 */

    struct timeval sent_time;
    gettimeofday(&sent_time,NULL); /* 获取当前时间 */
    pkt_store->pkt_start = msg; /* msg和sent_time要记得在收到ack以后把它free掉 */
    pkt_store->sent_time = sent_time;
    pkt_store->next = NULL;

    pkts = sock->window.sent_head;
    while((nexts = pkts->next) != NULL){ /* 此处为了找到发送缓存队列最后一个包的位置,pkts是最后一个包 */
        pkts = nexts;
    }
    pkts->next = pkt_store;

#ifdef DEBUG
    printf("%s", "single_send over: ");
    printf("%s", "sent_pkt的长度 = ");
    printf("%d", sock->window.sent_length);
    printf("\n");
#endif

}

void check_timeout(cmu_socket_t * sock){
    int sockfd;
    sent_pkt *nexts;
    socklen_t conn_len = sizeof(sock -> conn);
    sockfd = sock->socket;

    uint32_t time;

    if((nexts = sock->window.sent_head->next) != NULL){
        struct timeval now_time;
        gettimeofday(&now_time, NULL); /* 获取当前时间 */
        time = (now_time.tv_sec - nexts->sent_time.tv_sec) * 1000 + (now_time.tv_usec - nexts->sent_time.tv_usec) / 1000;
        if(time >= sock->window.TimeoutInterval){//超时
            sock->window.TimeoutInterval = 2 * sock->window.TimeoutInterval; /* 超时，时间间隔加倍 */
            nexts->is_resend = TRUE;
            gettimeofday(&(nexts->sent_time), NULL);

            //更新状态
            sock->window.ssthresh = sock->window.cwnd / 2;
            sock->window.cwnd = MAX_DLEN;
            sock->ack_dup = 0;
            sock->window.con_state = SLOW_STAR;

            sendto(sockfd, nexts->pkt_start, get_plen(nexts->pkt_start), 0, (struct sockaddr*) &(sock->conn), conn_len);//重传
        }
    }
}

/*
 * Param: in - the socket that is used for backend processing
 *
 * Purpose: To poll in the background for sending and receiving data to
 *  the other side.
 *
 */
void* begin_backend(void * in){
#ifdef DEBUG
    printf("%s", "begin_backend start");
    printf("\n");
#endif

    cmu_socket_t * dst = (cmu_socket_t *) in;
    int death, send_signal, window_size;
    char *data, *temp;
    uint32_t buf_len, length;

    /* TODO：此处在三次握手中初始化 */
    dst->window.TimeoutInterval = 1000;

    struct timeval now_time;

    while(TRUE){
        gettimeofday(&now_time,NULL); /* 获取当前时间 */
        while(pthread_mutex_lock(&(dst->death_lock)) !=  0);
        death = dst->dying;
        pthread_mutex_unlock(&(dst->death_lock));

        while(pthread_mutex_lock(&(dst->send_lock)) != 0);
        buf_len = dst->sending_len;

        if(death && buf_len == 0){
            pthread_mutex_unlock(&(dst->send_lock));//之前没有解除send锁
            break;
        }


#ifdef DEBUG
    printf("%s", "begin_backend: buf_len = ");
    printf("%d", buf_len);
    printf("\n");
#endif

        if(buf_len > 0){/* 有数据要发时才会，如果没数据就直接跳过，所以在外面套了一层判断 */
            window_size = min(dst->window.cwnd, dst->window.rwnd);
            length = min3(buf_len, MAX_DLEN, window_size - dst->window.sent_length);

#ifdef DEBUG
    printf("%s", "begin_backend: 可发送数据长度");
    printf("%d", length);
    printf("\n");
#endif

            length = length > 0 ? length : 1;/* 如果length小于0，在这里把它重新赋值为1 */

            data = malloc(length);
            memcpy(data, dst->sending_buf, length);
            dst->sending_len -= length;
            temp = malloc(dst->sending_len);
            memcpy(temp, dst->sending_buf+length, dst->sending_len);
            free(dst->sending_buf);
            dst->sending_buf = temp;

            pthread_mutex_unlock(&(dst->send_lock));
            single_send(dst, data, length);
            free(data);
        } else { /* 重要：加锁和解锁对应 */
            pthread_mutex_unlock(&(dst->send_lock));
        }

        check_for_data(dst, NO_WAIT);

        check_timeout(dst);//在这里检查超时

        /* 判断接收缓冲区有没有内容，如果有就读 */
        while(pthread_mutex_lock(&(dst->window.recv_lock)) != 0);
        if(dst->window.recv_length > 0)
            send_signal = TRUE;
        else
            send_signal = FALSE;
        pthread_mutex_unlock(&(dst->window.recv_lock));

        if(send_signal){
            pthread_cond_signal(&(dst->wait_cond));
        }
    }
    pthread_exit(NULL);
    return NULL;
}

