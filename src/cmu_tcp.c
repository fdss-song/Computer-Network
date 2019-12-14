#include "cmu_tcp.h"

#ifdef DEBUG
    void print_state(cmu_socket_t *sock){
        printf("state = %d\n", sock->state);
    }
#endif

/* 发送SYN */ /* TODO:可以优化，当前重发SYN会重新malloc */
void send_SYN(cmu_socket_t *dst){
    srand((unsigned)time(NULL));
    /* 根据客户端当前的状态判断是否需要重发SYN */
    uint32_t ISN; /* 初始序列号 */
    char *pkt;
    if(dst->state == CLOSED){
        ISN = rand() % SEQMAX; /* 随机数 */
        dst->ISN = ISN;
        dst->window.last_ack_received = ISN + 1; /* 此时只有前端线程，不需要加锁 */
    }else{
        ISN = dst->ISN;
    }
    /* TODO:写文档请把这段删去 */
    ISN = 0;
    dst->ISN = 0;
    dst->window.last_ack_received = 1;
    /* ******以上删去****** */
    pkt = create_packet_buf(dst->my_port, ntohs(dst->conn.sin_port), ISN, 0, /* 初始ACK，任意值 */
                            DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, SYN_FLAG_MASK, 1, 0, NULL, NULL, 0);
    sendto(dst->socket, pkt, DEFAULT_HEADER_LEN, 0, (struct sockaddr*)
            &(dst->conn), sizeof(dst->conn));
    free(pkt);
    return;
}

/* TODO：可以优化，缓存SYNACK，可能性能提高不多 */
void send_SYNACK(cmu_socket_t *dst){
  uint32_t ISN; /* 初始序列号 */
  char *pkt;
  /* 根据当前状态判断是否要重发SYNACK */
  if(dst->state == LISTEN){
    ISN = rand() % SEQMAX; /* 随机数 */
    dst->ISN = ISN;
    dst->window.last_ack_received = ISN;
  } else {
    ISN = dst->ISN;
  }

  /* 写文档时请把这段删去，假装我们随机产生初始序列号 */
  ISN = 0;
  dst->ISN = 0;
  dst->window.last_ack_received = 1;
  /* *************以上删去************ */

  pkt = create_packet_buf(dst->my_port, ntohs(dst->conn.sin_port), ISN,
                  dst->window.last_seq_received, /* 对SYN的确认，ACK号为x+1 */
                  DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
                  SYN_FLAG_MASK | ACK_FLAG_MASK, 1, 0, NULL, NULL, 0);
  sendto(dst->socket, pkt, DEFAULT_HEADER_LEN, 0, (struct sockaddr*)
            &(dst->conn), sizeof(dst->conn));
  free(pkt);
  return;
}

void send_ACK(cmu_socket_t *dst){
  char *pkt;
  pkt = create_packet_buf(dst->my_port, ntohs(dst->conn.sin_port),
                            dst->window.last_ack_received,
                            dst->window.last_seq_received,
                            DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
                            ACK_FLAG_MASK, 1, 0, NULL, NULL, 0);
  sendto(dst->socket, pkt, DEFAULT_HEADER_LEN, 0, (struct sockaddr*)
            &(dst->conn), sizeof(dst->conn));
  free(pkt);
  return;
}

void handle_handshake(cmu_socket_t *sock, char *pkt){
    uint8_t flags = get_flags(pkt);
    switch(sock->state){
        case LISTEN:
            if(flags == SYN_FLAG_MASK){
                sock->window.last_seq_received = get_seq(pkt) + 1;
                send_SYNACK(sock);
                sock->state = SYN_RCVD;
            }
            break;
        case SYN_RCVD:
            sock->state = ESTABLISHED;  /* 无论是否收到ACK，都完成连接 */
            break;
        case SYN_SENT:
            if(flags == (SYN_FLAG_MASK | ACK_FLAG_MASK)){
                send_ACK(sock);
                sock->state = ESTABLISHED;
            }
            break;
        default:
            break;
    }
}

/* 超时和阻塞 */
void check_for_handshake(cmu_socket_t *sock, int flags){
    char hdr[DEFAULT_HEADER_LEN];
    char* pkt;
    socklen_t conn_len = sizeof(sock->conn);
    ssize_t len = -1;
    uint32_t plen = 0, buf_size = 0, n = 0;
    fd_set ackFD;

    struct timeval time_out;
    time_out.tv_sec = 3;
    time_out.tv_usec = 0;

    switch(flags){
        case NO_FLAG: /* wait */
            while(len < DEFAULT_HEADER_LEN)
                len = recvfrom(sock->socket, hdr, DEFAULT_HEADER_LEN, MSG_PEEK,
                               (struct sockaddr *) &(sock->conn), &conn_len);
            break;
        case TIMEOUT:/* 设定超时间隔 */
            FD_ZERO(&ackFD);
            FD_SET(sock->socket, &ackFD);
            if(select(sock->socket+1 /* 重要：所有的文件描述符最大值+1 */, &ackFD, NULL, NULL, &time_out) <= 0){
                break;
            }
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

        handle_handshake(sock, pkt);
        free(pkt);
    }
}

void handshake(cmu_socket_t *dst){
    while(dst->state != ESTABLISHED){
        switch(dst->state){
            case CLOSED:
                if(dst->type == TCP_INITATOR){
                    send_SYN(dst);
                    dst->state = SYN_SENT;
                } else if(dst->type == TCP_LISTENER){
                    dst->state = LISTEN;
                }
                break;
            case SYN_SENT:
                check_for_handshake(dst, TIMEOUT);
                if(dst->state == SYN_SENT){
                    send_SYN(dst);
                }
                break;
            case LISTEN:
                check_for_handshake(dst, NO_FLAG);
                break;
            case SYN_RCVD:
                check_for_handshake(dst, TIMEOUT);
                break;
            default:
                printf("Invalid state");
                break;
        }
    }
}

void send_FIN(cmu_socket_t *dst){
    char *pkt;
    uint32_t seq;

    while(pthread_mutex_lock(&(dst->window.ack_lock)) != 0);
    seq = dst->window.last_ack_received;
    pthread_mutex_unlock(&(dst->window.ack_lock));

    pkt = create_packet_buf(dst->my_port, ntohs(dst->conn.sin_port),
                seq, dst->window.last_seq_received, /* 此处的ack序列号不重要 */
                DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN,
                FIN_FLAG_MASK, dst->window.rwnd, 0, NULL, NULL, 0);

    sendto(dst->socket, pkt, DEFAULT_HEADER_LEN, 0, (struct sockaddr*)
          &(dst->conn), sizeof(dst->conn));
    dst->state = FIN_WAIT_1;
    free(pkt);
    return;
}

/*
 * Param: dst - The structure where socket information will be stored
 * Param: flag - A flag indicating the type of socket(Listener / Initiator)
 * Param: port - The port to either connect to, or bind to. (Based on flag)
 * Param: ServerIP - The server IP to connect to if the socket is an initiator.
 *
 * Purpose: To construct a socket that will be used in various connections.
 *  The initiator socket can be used to connect to a listener socket.
 *
 * Return: The newly created socket will be stored in the dst parameter,
 *  and the value returned will provide error information. 
 *
 */
int cmu_socket(cmu_socket_t * dst, int flag, int port, char * serverIP){
    int sockfd, optval;
    socklen_t len;

    struct sockaddr_in conn, my_addr;
    len = sizeof(my_addr);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0){
        perror("ERROR opening socket");
        return EXIT_ERROR;
    }
    dst->their_port = port;
    dst->socket = sockfd;

    sent_pkt *spkt;
    spkt = (sent_pkt *)malloc(sizeof(sent_pkt));
    recv_pkt *rpkt;
    rpkt = (recv_pkt *)malloc(sizeof(recv_pkt));
    spkt->next=NULL;
    rpkt->next=NULL;

    dst->state = CLOSED;

    dst->window.recv_head = rpkt;
    dst->window.recv_length = 0;
    pthread_mutex_init(&(dst->window.recv_lock), NULL);
    dst->window.sent_head = spkt;
    dst->window.sent_length = 0;
    dst->sending_buf = NULL;
    dst->sending_len = 0;
    pthread_mutex_init(&(dst->send_lock), NULL);
    dst->type = flag;
    dst->dying = FALSE;
    pthread_mutex_init(&(dst->death_lock), NULL);

    dst->window.last_ack_received = 0;
    dst->window.last_seq_received = 0;
    pthread_mutex_init(&(dst->window.ack_lock), NULL);

    dst->ack_dup=0;

    dst->window.rwnd=WINDOW_INITIAL_WINDOW_SIZE;
    dst->window.cwnd=WINDOW_INITIAL_WINDOW_SIZE;
    dst->window.EstimatedRTT=WINDOW_INITIAL_RTT;
    dst->window.DevRTT=0;

    dst->window.ssthresh=WINDOW_INITIAL_SSTHRESH * 1024;
    dst->window.con_state=SLOW_STAR;
    dst->window.TimeoutInterval=1000;


    if(pthread_cond_init(&dst->wait_cond, NULL) != 0){
        perror("ERROR condition variable not set\n");
        return EXIT_ERROR;
    }


    switch(flag){
        case(TCP_INITATOR):
            if(serverIP == NULL){
                perror("ERROR serverIP NULL");
                return EXIT_ERROR;
            }
            memset(&conn, 0, sizeof(conn));
            conn.sin_family = AF_INET;
            conn.sin_addr.s_addr = inet_addr(serverIP);
            conn.sin_port = htons(port);
            dst->conn = conn;

            my_addr.sin_family = AF_INET;
            my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
            my_addr.sin_port = 0;
            if (bind(sockfd, (struct sockaddr *) &my_addr,
                     sizeof(my_addr)) < 0){
                perror("ERROR on binding");
                return EXIT_ERROR;
            }

            handshake(dst);

            break;
        case(TCP_LISTENER):
            bzero((char *) &conn, sizeof(conn));
            conn.sin_family = AF_INET;
            conn.sin_addr.s_addr = htonl(INADDR_ANY);
            conn.sin_port = htons((unsigned short)port);

            optval = 1;
            setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
                       (const void *)&optval , sizeof(int));
            if (bind(sockfd, (struct sockaddr *) &conn,
                     sizeof(conn)) < 0){
                perror("ERROR on binding");
                return EXIT_ERROR;
            }
            dst->conn = conn;

            handshake(dst);

            break;

        default:
            perror("Unknown Flag");
            return EXIT_ERROR;
    }
    getsockname(sockfd, (struct sockaddr *) &my_addr, &len);
    dst->my_port = ntohs(my_addr.sin_port);

    pthread_create(&(dst->thread_id), NULL, begin_backend, (void *)dst);
    return EXIT_SUCCESS;
}

/*
 * Param: sock - The socket to close.
 *
 * Purpose: To remove any state tracking on the socket.
 *
 * Return: Returns error code information on the close operation.
 *
 */
int cmu_close(cmu_socket_t * sock){
    while(sock->window.sent_length != 0)
        sleep(2);

    /* TODO:超时重传 */
    if(sock->state == ESTABLISHED){
        send_FIN(sock);

#ifdef DEBUG
    printf("%d sent FIN\n", sock->type);
    print_state(sock);
#endif

    }

    while(pthread_mutex_lock(&(sock->death_lock)) != 0);
    sock->dying = TRUE;
    pthread_mutex_unlock(&(sock->death_lock));

    pthread_join(sock->thread_id, NULL);
    printf("state = %d\n", sock->state);

    if(sock != NULL){
        if(sock->window.recv_head != NULL)
            free(sock->window.recv_head);
        if(sock->sending_buf != NULL)
            free(sock->sending_buf);
    }
    else{
        perror("ERORR Null scoket\n");
        return EXIT_ERROR;
    }
    return close(sock->socket);
}

/*
 * Param: sock - The socket to read data from the received buffer.
 * Param: dst - The buffer to place read data into.
 * Param: length - The length of data the buffer is willing to accept.
 * Param: flags - Flags to signify if the read operation should wait for
 *  available data or not.
 *
 * Purpose: To retrive data from the socket buffer for the user application.
 *
 * Return: If there is data available in the socket buffer, it is placed
 *  in the dst buffer, and error information is returned.
 *
 */
int cmu_read(cmu_socket_t * sock, char* dst, int length, int flags){
    char* new_buf;
    int read_len = 0;
    recv_pkt *pkts, *nexts;
    if(length < 0){
        perror("ERROR negative length");
        return EXIT_ERROR;
    }

    while(pthread_mutex_lock(&(sock->window.recv_lock)) != 0);

    if(sock->window.recv_length == 0 && sock->state == CLOSED){
        pthread_mutex_unlock(&(sock->window.recv_lock));
        return 0;
    }

    switch(flags){
        case NO_FLAG:
            while(sock->window.recv_length == 0 || sock->window.recv_head->next == NULL || !sock->window.recv_head->next->adjacent){
                pthread_cond_wait(&(sock->wait_cond), &(sock->window.recv_lock));
                if(sock->window.recv_length == 0 && sock->state == CLOSED){
                    pthread_mutex_unlock(&(sock->window.recv_lock));
                    return 0;
                }
            }
        case NO_WAIT:
            pkts = sock->window.recv_head;
            while((nexts = pkts->next) != NULL && read_len < length && nexts->adjacent){/* 还需要继续读，并且有数据可读 */
                if(length - read_len >= nexts->data_length){/* 剩余要读的内容大于下一个recv_pkt的长度，并且该recv_pkt是能够直接读的，直接把整个pkt中的内容取出 */
                    memcpy(dst + read_len, nexts->data_start, nexts->data_length);
                    read_len += nexts->data_length;
                    sock->window.recv_length -= nexts->data_length;
                    pkts->next = nexts->next;
                    free(nexts->data_start);
                    free(nexts);
                }
                else if(length - read_len < nexts->data_length){/* 剩余要读的内容小于等于recv_pkt的长度，并且该recv_pkt是能够直接读的，只在recv_pkt中取出部分 */
                    memcpy(dst + read_len, nexts->data_start, length - read_len);
                    new_buf = malloc(nexts->data_length - (length - read_len));//剩余的长度
                    memcpy(new_buf, nexts->data_start + (length - read_len), nexts->data_length - (length - read_len));
                    free(nexts->data_start);
                    nexts->data_start = new_buf;
                    nexts->seq += (length - read_len);
                    nexts->data_length -= (length - read_len);
                    sock->window.recv_length -= (length - read_len);
                    read_len = length;
                }
                else{
                    //读到一个不能读的recv_pkt,什么也不做,跳出while循环
                    break;
                }
            }
            break;
        default:
            perror("ERROR Unknown flag.\n");
            read_len = EXIT_ERROR;
    }
    pthread_mutex_unlock(&(sock->window.recv_lock));
    return read_len;
}

/*
 * Param: sock - The socket which will facilitate data transfer.
 * Param: src - The data source where data will be taken from for sending.
 * Param: length - The length of the data to be sent.
 *
 * Purpose: To send data to the other side of the connection.
 *
 * Return: Writes the data from src into the sockets buffer and
 *  error information is returned.
 *
 */
int cmu_write(cmu_socket_t * sock, char* src, int length){
    while(pthread_mutex_lock(&(sock->send_lock)) != 0);
    if(sock->sending_buf == NULL)
        sock->sending_buf = malloc(length);
    else
        sock->sending_buf = realloc(sock->sending_buf, length + sock->sending_len);
    memcpy(sock->sending_buf + sock->sending_len, src, length);
    sock->sending_len += length;

    pthread_mutex_unlock(&(sock->send_lock));

    return EXIT_SUCCESS;
}
