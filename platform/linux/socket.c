#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <unistd.h>

#include "socket.h"
#include "etherip.h"
#include <sys/uio.h>
#include <stdlib.h>
#include <linux/limits.h>

#define SOCK_BUFFER_SIZE (1024 * 1024 * 1024)

extern int sock_open(int *fd, int domain, struct sockaddr_storage *addr, socklen_t addr_len){
    *fd = socket(domain, SOCK_RAW, ETHERIP_PROTO_NUM);
    if(*fd == -1){
        fprintf(stderr, "[ERROR]: Failed to open socket: %s\n", strerror(errno));
        return -1;
    }

    int sock_buf = SOCK_BUFFER_SIZE;
    if(setsockopt(*fd, SOL_SOCKET, SO_RCVBUF, &sock_buf, sizeof(sock_buf)) == -1){
        fprintf(stderr, "[ERROR]: Failed to set socket receive buffer: %s\n", strerror(errno));
        close(*fd);
        return -1;
    }
    if(setsockopt(*fd, SOL_SOCKET, SO_SNDBUF, &sock_buf, sizeof(sock_buf)) == -1){
        fprintf(stderr, "[ERROR]: Failed to set socket send buffer: %s\n", strerror(errno));
        close(*fd);
        return -1;
    }

    if(domain == AF_INET){
        int pmtu_discover = IP_PMTUDISC_DONT;
        if(setsockopt(*fd, IPPROTO_IP, IP_MTU_DISCOVER, &pmtu_discover, sizeof(pmtu_discover)) == -1){
            fprintf(stderr, "[ERROR]: Failed to disable IPv4 PMTU discovery: %s\n", strerror(errno));
            close(*fd);
            return -1;
        }
    }

    if(domain == AF_INET6){
        int pmtu6 = IPV6_PMTUDISC_DONT;
        if(setsockopt(*fd, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &pmtu6, sizeof(pmtu6)) == -1){
            fprintf(stderr, "[ERROR]: Failed to disable IPv6 PMTU discovery: %s\n", strerror(errno));
            close(*fd);
            return -1;
        }
    }

    if(bind(*fd, (struct sockaddr *)addr, addr_len) == -1){
        fprintf(stderr, "[ERROR]: Failed to bind socket: %s\n", strerror(errno));
        close(*fd);
        return -1;
    }
    return 0;
}

extern int sock_close(int fd){
    close(fd);
    return 0;
}

extern ssize_t sock_read(int fd, uint8_t *buffer, size_t size, struct sockaddr_storage *addr, socklen_t *addr_len){
    ssize_t len;
    len = recvfrom(fd, buffer, size, 0, (struct sockaddr *)addr, addr_len);
    if(len <= 0){
        fprintf(stderr, "[ERROR]: sock_read: %s\n", strerror(errno));
        return -1;
    }
    return len;
}

extern ssize_t sock_write(int fd, const uint8_t *frame, size_t size, struct sockaddr_storage *addr, socklen_t addr_len){
    ssize_t len;
    len = sendto(fd, frame, size, 0, (struct sockaddr *)addr, addr_len);
    if(len <= 0){
        fprintf(stderr, "[ERROR]: sock_write: %s\n", strerror(errno));
        return -1;
    }
    return len;
}

extern ssize_t sock_send_burst(int fd, const uint8_t **frames, const size_t *sizes, size_t count, struct sockaddr_storage *addr, socklen_t addr_len){
    struct mmsghdr *msgs = NULL;
    struct iovec *iovs = NULL;
    ssize_t ret = -1;
    if(count == 0) return 0;

    msgs = calloc(count, sizeof(struct mmsghdr));
    if(!msgs){
        fprintf(stderr, "[ERROR]: sock_send_burst: calloc failed\n");
        return -1;
    }

    iovs = calloc(count, sizeof(struct iovec));
    if(!iovs){
        fprintf(stderr, "[ERROR]: sock_send_burst: calloc iovs failed\n");
        free(msgs);
        return -1;
    }

    for(size_t i = 0; i < count; i++){
        iovs[i].iov_base = (void *)frames[i];
        iovs[i].iov_len = sizes[i];

        msgs[i].msg_hdr.msg_name = addr;
        msgs[i].msg_hdr.msg_namelen = addr_len;
        msgs[i].msg_hdr.msg_iov = &iovs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_control = NULL;
        msgs[i].msg_hdr.msg_controllen = 0;
        msgs[i].msg_hdr.msg_flags = 0;
    }

    int sent = sendmmsg(fd, msgs, (unsigned int)count, 0);
    if(sent == -1){
        fprintf(stderr, "[ERROR]: sock_send_burst: %s\n", strerror(errno));
        ret = -1;
    } else {
        ret = sent;
    }

    free(iovs);
    free(msgs);

    return ret;
}
