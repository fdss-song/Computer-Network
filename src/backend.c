#include "backend.h"
#include <math.h>

#define min(A, B) ((A) > (B) ? (B) : (A))
#define min3(A,B,C) (((A)> (B) ? (B) : (A)) > (C)) ? (C) : ((A) > (B) ? (B) : (A))
#define max(A, B) ((A) < (B) ? (B) : (A))

long get_current_time(){
    struct timeval time;
    gettimeofday(&time,NULL);
    return time.tv_sec*1000000+time.tv_usec;/* 返回微秒值 */
}

void handle_FIN(cmu_socket_t *dst, char *msg){
    char *pkt;

    dst->window.last_ack_received = get_ack(msg);
    dst->window.last_seq_received = get_seq(msg) + 1;

    pkt = create_packet_buf(dst->my_port, ntohs(dst->conn.sin_port),
                            dst->window.last_ack_received,
                            dst->window.last_seq_received,
                            DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
                            ACK_FLAG_MASK, 1, 0, NULL, NULL, 0);
    sendto(dst->socket, pkt, DEFAULT_HEADER_LEN, 0, (struct sockaddr*)
            &(dst->conn), sizeof(dst->conn));
    dst->state = CLOSE_WAIT;
    free(pkt);
}

void handle_FIN1_ACK(cmu_socket_t *sock, char *pkt){
    sock->state = FIN_WAIT_2;
    /* TODO：此处ACK占用一个序列号吗？ */
    sock->window.last_seq_received = get_seq(pkt);
    sock->window.last_ack_received = get_ack(pkt);
}

void handle_FINACK(cmu_socket_t *dst, char *msg){
    char *pkt;

    dst->window.last_ack_received = get_ack(msg);
    dst->window.last_seq_received = get_seq(msg) + 1;

    pkt = create_packet_buf(dst->my_port, ntohs(dst->conn.sin_port),
                            dst->window.last_ack_received,
                            dst->window.last_seq_received,
                            DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
                            ACK_FLAG_MASK, 1, 0, NULL, NULL, 0);
    sendto(dst->socket, pkt, DEFAULT_HEADER_LEN, 0, (struct sockaddr*)
            &(dst->conn), sizeof(dst->conn));
    dst->state = TIME_WAIT;
    dst->window.fin_time = get_current_time();
    free(pkt);
    return;
}

void handle_ack(cmu_socket_t * sock, char * pkt){
    uint32_t sample_RTT;
    socklen_t conn_len = sizeof(sock -> conn);
    sent_pkt *pkts, *nexts;
    sock->window.rwnd = get_advertised_window(pkt);
//    printf("recv ack : %d\n", get_ack(pkt));
    if(get_ack(pkt) > sock->window.last_ack_received){
        sock->ack_dup = 0;
        switch(sock->window.con_state){
            case SLOW_STAR: /* 慢启动 */
                sock->window.cwnd += MAX_DLEN;
                if(sock->window.cwnd >= sock->window.ssthresh) /* 当cwnd≥ssthresh，需要进入拥塞避免状态 */
                    sock->window.con_state = CONG_AVOI;
                break;
            case CONG_AVOI: /* 拥塞避免状态 */
                sock->window.cwnd += MAX_DLEN * MAX_DLEN / sock->window.cwnd;
                break;
            case FAST_RECO: /* 快速恢复 */
                // sock->window.cwnd = sock->window.ssthresh;
                sock->window.cwnd = max(sock->window.ssthresh, MAX_DLEN);
                sock->window.con_state = CONG_AVOI;
                break;
            default:
                perror("ERROR unknown flag");
                return;
        }
        sock->window.last_ack_received = get_ack(pkt);
        sock->window.rwnd = get_advertised_window(pkt);
        pkts = sock->window.sent_head;
        while((nexts = pkts->next) != NULL && get_seq(nexts->pkt_start) < get_ack(pkt)){//接收方处理
            pkts->next = nexts->next;
            //确定该ack时，才更新rtt, devRtt, timeoutInterval
            if(get_seq(nexts->pkt_start)+ get_plen(nexts->pkt_start) - get_hlen(nexts->pkt_start) == get_ack(pkt) && !nexts->is_resend){
                struct timeval arrival_time;
                gettimeofday(&arrival_time, NULL);
                sample_RTT = (arrival_time.tv_sec - nexts->sent_time.tv_sec) * 1000 + (arrival_time.tv_usec - nexts->sent_time.tv_usec) / 1000;

                sock->window.EstimatedRTT = alpha * sample_RTT + (1 - alpha) * sock->window.EstimatedRTT;
                sock->window.DevRTT = (1 - beta) * sock->window.DevRTT + beta * abs(sock->window.EstimatedRTT - sample_RTT);

                /* 这里是调整超时时间，单独测丢包率时注释掉了 */
               sock->window.TimeoutInterval = sock->window.EstimatedRTT + 4 * sock->window.DevRTT;
            }

            sock->window.sent_length -= (get_plen(nexts->pkt_start) - get_hlen(nexts->pkt_start));/* 每释放一个缓存pkt需要将sent_length减小 */
//            printf("本地缓存的长度:%d\n", sock->window.sent_length);
//            printf("本次释放的长度:%d\n\n", (get_plen(nexts->pkt_start) - get_hlen(nexts->pkt_start)));
            free(nexts->pkt_start);
            free(nexts);
        }
    } else if(get_ack(pkt) == sock->window.last_ack_received){ /* 只考虑对窗口前一个包的重复ACK */
        sock->ack_dup += 1;

        if(sock->window.con_state == FAST_RECO){
            sock->window.cwnd += MAX_DLEN;/* 如果已经处于快速恢复状态，则加上一个mss */
        }else{
            if(sock->ack_dup == 3){
            printf("快速重传了！！！\n");
            /* 立即快速重传，并进入快速恢复状态 */
            if(sock->window.con_state != FAST_RECO){
                sock->window.ssthresh = sock->window.cwnd / 2;
                sock->window.cwnd = sock->window.ssthresh + 3 * MAX_DLEN;
                sock->window.con_state = FAST_RECO;
            }
            pkts = sock->window.sent_head;
            nexts = pkts->next;
            if(nexts != NULL){
                nexts->is_resend = TRUE;
                gettimeofday(&(nexts->sent_time), NULL);
                sendto(sock->socket, nexts->pkt_start, get_plen(nexts->pkt_start), 0, (struct sockaddr*) &(sock->conn), conn_len);/* 快速重传一定是传缓存链表的第一个包，此处主要看看语法对不对 */
                sock->ack_dup = 0;
            }
        }
        }
    }

    if(sock->window.sent_length == 0){
//        printf("ssssss\n");
        sock->window.sent_head->next = NULL;
    }

}

void handle_datapkt(cmu_socket_t *sock, char *pkt){
    char *rsp;
    uint32_t seq, ack, rwnd;
    socklen_t conn_len = sizeof(sock->conn);
    recv_pkt *pktr, *prevr, *nextr;
    seq = get_seq(pkt);
    bool is_dup = FALSE;


    if(seq >= sock->window.last_seq_received){
        /*
         * 1. 找到插入位置
         * 2. 相邻adjacent
         * 3. last_seq_received
         * 4. send(ACK)
         * 更新：last_seq, adjacent, next, recv_length
         */

        pktr = malloc(sizeof(recv_pkt));
        pktr->seq = seq;
        pktr->data_length = get_plen(pkt) - get_hlen(pkt);
        pktr->data_start = malloc(pktr->data_length);
        memcpy(pktr->data_start, pkt + get_hlen(pkt), pktr->data_length);
        pktr->adjacent = FALSE;
        pktr->next = NULL;

        prevr = sock->window.recv_head;

        nextr = prevr->next;
        
        /* 插入，更新next */
        if(nextr == NULL){
            prevr->next = pktr;
        }else{
            while(nextr != NULL){
                if(nextr->seq == seq){ /* 之前已经插入了*/
                    free(pktr->data_start);
                    free(pktr);
                    is_dup = TRUE;
                    break;
                }

                if(nextr->seq > seq){ /* 插入位置，prevr->pktr->nextr */
                    prevr->next = pktr;
                    pktr->next = nextr;
                    if(nextr->seq == seq + pktr->data_length) /* 插入位置，prevr->pktr->nextr */
                        nextr->adjacent = TRUE;
                    break;
                }
                prevr = nextr;
                nextr = prevr->next;
            }

            if(seq > prevr->seq && nextr == NULL){
                prevr->next = pktr;
                pktr->next = nextr;
            }
        }
    } else{
        is_dup = TRUE;
    }

    if(!is_dup){
        sock->window.recv_length += pktr->data_length; /* 更新recv_length */
        /* 更新adjacent */
        if(prevr == sock->window.recv_head) { /* ***** */
            pktr->adjacent = (seq == sock->window.last_seq_received);
        } else if (prevr->seq + prevr->data_length == seq) {
            pktr->adjacent = TRUE;
        }

        /* 更新last_ack_received */
        if(seq == sock->window.last_seq_received){
            while(pktr != NULL && pktr->adjacent){
                sock->window.last_seq_received += pktr->data_length;
                pktr = pktr->next;
            }
        }
    }

    /* 回ACK */
    seq = sock->window.last_ack_received + sock->window.sent_length;
    ack = sock->window.last_seq_received; /* 累积确认 */
    rwnd = MAX_NETWORK_BUFFER - sock->window.recv_length;
    rsp = create_packet_buf(sock->my_port, ntohs(sock->conn.sin_port), seq, /* TODO：考虑ACK带数据，此处需要更改 */ 
        ack, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, rwnd, 0, NULL, NULL, 0);

    sendto(sock->socket, rsp, DEFAULT_HEADER_LEN, 0, (struct sockaddr *)
            &(sock->conn), conn_len);
    free(rsp);
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
    uint8_t flags = get_flags(pkt);
    switch(flags){
        case ACK_FLAG_MASK:
            if(sock->state == FIN_WAIT_1){
                printf("客户端连接结束\n");
                handle_FIN1_ACK(sock, pkt);
            } else if(sock->state == LAST_ACK){
                printf("服务器连接结束\n");
                sock->state = CLOSED;
            } else{
                handle_ack(sock, pkt);
            }
            break;
        case FIN_FLAG_MASK:
            if(sock->state == ESTABLISHED)
                handle_FIN(sock, pkt);
            break;
        case ACK_FLAG_MASK | FIN_FLAG_MASK:
            if(sock->state == FIN_WAIT_2)
                handle_FINACK(sock, pkt);
            break;
        default:
            handle_datapkt(sock, pkt);
            break;
    }
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
    char hdr[DEFAULT_HEADER_LEN];
    char* pkt;
    socklen_t conn_len = sizeof(sock->conn);
    ssize_t len = -1;
    uint32_t plen = 0, buf_size = 0, n = 0;
    fd_set ackFD;

    /* TODO： 握手有独立的实现，TIMEOUT可删 */
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
                FD_SET(sock->socket, &ackFD); /* TODO: 可不可以用sock->window.TimeoutInterval */
                if(select(sock->socket+1 /* 重要：所有的文件描述符最大值+1 */, &ackFD, NULL, NULL, &time_out) <= 0){
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
        if(len >= DEFAULT_HEADER_LEN){ /* 如果此处收到了一个完整的头部，那么循环判断等待完全收到这个包之后handle处理 */
            plen = get_plen(hdr);
            pkt = malloc(plen);
            while(buf_size < plen){
                n = recvfrom(sock->socket, pkt + buf_size, plen - buf_size,
                         NO_FLAG, (struct sockaddr *) &(sock->conn), &conn_len);
                buf_size = buf_size + n;
            }

            handle_message(sock, pkt);
            free(pkt);
        }
    pthread_mutex_unlock(&(sock->window.recv_lock));
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

    seq = sock->window.last_ack_received + sock->window.sent_length ;
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
    pkt_store->is_resend = FALSE;

    pkts = sock->window.sent_head;
    while((nexts = pkts->next) != NULL){ /* 此处为了找到发送缓存队列最后一个包的位置,pkts是最后一个包 */
        pkts = nexts;
    }
    pkts->next = pkt_store;
}

void check_timeout(cmu_socket_t * sock){
    int sockfd;
    sent_pkt *nexts;
    socklen_t conn_len = sizeof(sock -> conn);
    sockfd = sock->socket;

    uint32_t time;

    if(sock->window.fin_time != 0 && sock->state == TIME_WAIT){
        long tim = get_current_time();
        if(tim - sock->window.fin_time >= 2 * sock->window.TimeoutInterval){
            sock->state = CLOSED;
        }
    }

    if((nexts = sock->window.sent_head->next) != NULL){
        if(sock->state == TIME_WAIT)
            printf("nexts->is_resend = %d\n", nexts->is_resend);
        struct timeval now_time;
        gettimeofday(&now_time, NULL); /* 获取当前时间 */
        time = (now_time.tv_sec - nexts->sent_time.tv_sec) * 1000 + (now_time.tv_usec - nexts->sent_time.tv_usec) / 1000;
        if(time >= sock->window.TimeoutInterval){//超时

            /* 这里是调整超时时间，单独测丢包率时注释掉了 */
            sock->window.TimeoutInterval = 2 * sock->window.TimeoutInterval; /* 超时，时间间隔加倍 */
            nexts->is_resend = TRUE;
            gettimeofday(&(nexts->sent_time), NULL);

            //更新状态
            sock->window.ssthresh = sock->window.cwnd / 2;
            printf("超时了\n");
            sock->window.cwnd = MAX_DLEN;
            sock->ack_dup = 0;
            sock->window.con_state = SLOW_STAR;
            sendto(sockfd, nexts->pkt_start, get_plen(nexts->pkt_start), 0, (struct sockaddr*) &(sock->conn), conn_len);//重传
        }
    }
}

void send_FINACK(cmu_socket_t *dst){
    char *pkt;

    pkt = create_packet_buf(dst->my_port, ntohs(dst->conn.sin_port),
                            dst->window.last_ack_received,
                            ++dst->window.last_seq_received,
                            DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
                            ACK_FLAG_MASK | FIN_FLAG_MASK, 1, 0, NULL, NULL, 0);
    sendto(dst->socket, pkt, DEFAULT_HEADER_LEN, 0, (struct sockaddr*)
            &(dst->conn), sizeof(dst->conn));
    dst->state = LAST_ACK;
    free(pkt);
    return;
}

/*
 * Param: in - the socket that is used for backend processing
 *
 * Purpose: To poll in the background for sending and receiving data to
 *  the other side.
 *
 */
void* begin_backend(void * in){
    cmu_socket_t * dst = (cmu_socket_t *) in;
    int death, send_signal, window_size;
    char *data, *temp;
    int buf_len, length;

    bool one_byte_flag = 0;

    struct timeval now_time;

    while(TRUE){
        gettimeofday(&now_time,NULL); /* 获取当前时间 */
        while(pthread_mutex_lock(&(dst->death_lock)) !=  0);
        death = dst->dying;
        pthread_mutex_unlock(&(dst->death_lock));

        check_timeout(dst); /* 检查是否超时 */

        /* 查看是否有数据要发 */
        while(pthread_mutex_lock(&(dst->send_lock)) != 0);
        buf_len = dst->sending_len;

        if(death && buf_len == 0 && dst->window.sent_length == 0 && dst->state == CLOSED){ /* 如果确认结束并且没有数据要发，跳出循环 */
            pthread_mutex_unlock(&(dst->send_lock));
            break;
        }

        if(dst->state == CLOSED){
            send_signal = TRUE;
        }

        if(dst->window.sent_length == 0 && dst->state == CLOSE_WAIT){
            send_FINACK(dst);
            dst->state = LAST_ACK;
        }
        
        if(buf_len > 0){/* 有数据要发时才会，如果没数据就直接跳过，所以在外面套了一层判断 */
            uint32_t temp0 = (dst->window.rwnd > dst->window.sent_length) ? 
                    dst->window.rwnd - dst->window.sent_length : 0;
                // printf("judge\n");
            if ((temp0 == 0 && !one_byte_flag) || temp0 > 0) {
                if (temp0 == 0) {
                    one_byte_flag = 1;
                    length = 1;
                } else {
                    one_byte_flag = 0;
                    window_size = min(dst->window.cwnd, temp0);
                    length = min3(buf_len, MAX_DLEN, window_size);
                }
              
            // length = (length > 0 ? length : 1);/* 如果length小于0，在这里把它重新赋值为1 */

            printf("buf_len:%d, ssthresh: %d, cwnd: %d ,rwnd: %d , window_size: %d; length: %d\n",
                    buf_len, dst->window.ssthresh, dst->window.cwnd,dst->window.rwnd, window_size, length);
            data = malloc(length);
            memcpy(data, dst->sending_buf, length);

            /* 剩余数据 */
            dst->sending_len -= length; /* TODO：what if sending_len = 0？ */
            temp = malloc(dst->sending_len); /* 此处的行为是未定义的，有可能返回NULL，或者是一个可以free/realloc的非NULL指针；两种情况下都可以正确处理 */
            memcpy(temp, dst->sending_buf + length, dst->sending_len);
            free(dst->sending_buf);
            dst->sending_buf = temp;

            pthread_mutex_unlock(&(dst->send_lock));
            single_send(dst, data, length);
            free(data);
            }else{
            pthread_mutex_unlock(&(dst->send_lock));
            }
        } else { /* 重要：加锁和解锁对应 */
            pthread_mutex_unlock(&(dst->send_lock));
        }

        check_for_data(dst, NO_WAIT); /* 检查有没有收到数据 */

        check_timeout(dst); /* 检查是否超时 */

        /* TODO：如果此时数据还没有接受完全，那么读出的数据是不完整的 */
        /* 检查接收缓冲区有没有内容，如果有就读 */
        while(pthread_mutex_lock(&(dst->window.recv_lock)) != 0);
        if((dst->window.recv_length > 0
            && dst->window.recv_head->next != NULL
            && dst->window.recv_head->next->adjacent) || dst->state == CLOSED)
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


