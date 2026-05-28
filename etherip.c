#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <linux/if.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <arpa/inet.h>
#include <poll.h>

#include "etherip.h"
#include "tap.h"
#include "socket.h"

static pthread_t *threads;
static size_t thread_count;
static pthread_barrier_t barrier;

struct etherip_hdr {
    uint8_t hdr_1st;
    uint8_t hdr_2nd;
};

struct handler_args {
    int domain;
    int sock_fd;
    int tap_fd;
    struct sockaddr_storage *dst_addr;
    pthread_barrier_t *barrier;
};

static void on_signal(int s){
    (void)s;
    if(threads == NULL){
        return;
    }
    for(size_t i = 0; i < thread_count; i++){
        pthread_kill(threads[i], SIGHUP);
    }
}

static void print_usage(){
    printf("Usage\n");
    printf("    etherip [OPTIONS] { ipv4 | ipv6 } dst <ip addr> src <ip addr> tap <tap if name> &\n");
    printf("OPTIONS\n");
    printf("    dst <ip addr>\t: set the destination ip address\n");
    printf("    src <ip addr>\t: set the source ip address\n");
    printf("    tap <tap if name>\t: set the tap IF name\n");
    printf("    --mtu <mtu>\t\t: set mtu (Not a tunnel IF mtu). default: 1500\n");
    printf("    --mq [queues]\t\t: enable TAP multi-queue mode and create queue threads\n");

}

static void *recv_handlar(void *args){
    // setup
    struct handler_args *handler_args = (struct handler_args *)args;
    int domain = handler_args->domain;
    int sock_fd = handler_args->sock_fd;
    int tap_fd = handler_args->tap_fd;
    struct sockaddr_storage *dst_addr = handler_args->dst_addr;
    
    ssize_t rlen;
    uint8_t buffer[BUFFER_SIZE];
    struct sockaddr_storage addr;
    socklen_t addr_len;
    uint8_t reserved1;
    uint8_t reserved2;
    struct iphdr *ip_hdr;
    int ip_hdr_len;
    struct etherip_hdr *hdr;
    uint8_t version;
    size_t write_len;
    // end setup
    pthread_barrier_wait(handler_args->barrier);

    while(1){

        rlen = sock_read(sock_fd, buffer, sizeof(buffer), &addr, &addr_len);
        if(rlen == -1){
            // Failed to sock_read()
            return NULL;
        }

        
        if(domain == AF_INET){
            if((size_t)rlen < sizeof(struct iphdr) + sizeof(struct etherip_hdr)){
                // too short
                continue;
            }

            // destination check
            struct sockaddr_in *dst_addr4;
            dst_addr4 = (struct sockaddr_in *)dst_addr;
            struct sockaddr_in *addr4;
            addr4 = (struct sockaddr_in *)&addr;
            if(addr4->sin_addr.s_addr != dst_addr4->sin_addr.s_addr){
                continue;
            }

            // skip header
            ip_hdr = (struct iphdr *)buffer;
            ip_hdr_len = ip_hdr->ihl * 4;
            hdr = (struct etherip_hdr *)(buffer + ip_hdr_len);
            write_len = rlen - ETHERIP_HEADER_LEN - ip_hdr_len;
        }
        else if(domain == AF_INET6){
            if((size_t)rlen < sizeof(struct ip6_hdr) + sizeof(struct etherip_hdr)){
                // too short
                continue;
            }

            // destination check
            struct sockaddr_in6 *dst_addr6;
            dst_addr6 = (struct sockaddr_in6 *)dst_addr;
            struct sockaddr_in6 *addr6;
            addr6 = (struct sockaddr_in6 *)&addr;
            if(memcmp(addr6->sin6_addr.s6_addr, dst_addr6->sin6_addr.s6_addr, sizeof(addr6->sin6_addr.s6_addr)) != 0){
	            continue;
            }

            hdr = (struct etherip_hdr *)(&buffer);
            write_len = rlen - ETHERIP_HEADER_LEN;
        }


        // version check
        version = hdr->hdr_1st >> 4;
        if(version != ETHERIP_VERSION){
            // unknown version
            continue;
        }
        // reserved field check
        reserved1 = hdr->hdr_1st & 0xF;
        reserved2 = hdr->hdr_2nd;
        if(reserved1 != 0 || reserved2 != 0){
            // reserved field is not 0
            continue;
        }

        tap_write(tap_fd, (uint8_t *)(hdr+1), write_len);
    }

    return NULL;
}

static void *send_handlar(void *args){
    // setup
    struct handler_args *handler_args = (struct handler_args *)args;
    int domain = handler_args->domain;
    int sock_fd = handler_args->sock_fd;
    int tap_fd = handler_args->tap_fd;
    struct sockaddr_storage *dst_addr = handler_args->dst_addr;
    size_t dst_addr_len;

    ssize_t rlen; // receive len
    uint8_t buffer[BUFFER_SIZE];
    struct etherip_hdr *hdr;
    // end setup
    pthread_barrier_wait(&barrier);

    const size_t max_burst = BURST_SIZE;
    const int burst_flush_interval_ms = 100;
    const uint8_t *frames[BURST_SIZE];
    size_t sizes[BURST_SIZE];
    uint8_t *allocs[BURST_SIZE];
    size_t idx = 0;

    pthread_barrier_wait(handler_args->barrier);

    if(domain == AF_INET)
        dst_addr_len = sizeof( *(struct sockaddr_in *)dst_addr );
    else if(domain == AF_INET6)
        dst_addr_len = sizeof( *(struct sockaddr_in6 *)dst_addr );

    while(1){
        struct pollfd pfd = {
            .fd = tap_fd,
            .events = POLLIN,
            .revents = 0,
        };
        int timeout = (idx == 0) ? -1 : burst_flush_interval_ms;
        int pret = poll(&pfd, 1, timeout);

        if(pret == -1){
            if(errno == EINTR){
                continue;
            }
            fprintf(stderr, "[ERROR]: poll failed in send_handlar: %s\n", strerror(errno));
            for(size_t j = 0; j < idx; j++) free(allocs[j]);
            return NULL;
        }

        if(pret == 0){
            if(idx > 0){
                ssize_t sent = sock_send_burst(sock_fd, frames, sizes, idx, dst_addr, dst_addr_len);
                if(sent == -1){
                    fprintf(stderr, "[ERROR]: burst send failed\n");
                }
                for(size_t j = 0; j < idx; j++) free(allocs[j]);
                idx = 0;
            }
            continue;
        }

        if((pfd.revents & POLLIN) == 0){
            continue;
        }

        rlen = tap_read(tap_fd, buffer, sizeof(buffer));
        if(rlen == -1){
            // Failed to tap_read()
            // free any pending allocations
            for(size_t j = 0; j < idx; j++) free(allocs[j]);
            return NULL;
        }

        size_t total_len = sizeof(struct etherip_hdr) + (size_t)rlen;
        allocs[idx] = malloc(total_len);
        if(!allocs[idx]){
            fprintf(stderr, "[ERROR]: malloc failed in send_handlar\n");
            for(size_t j = 0; j < idx; j++) free(allocs[j]);
            return NULL;
        }

        hdr = (struct etherip_hdr *)allocs[idx];
        hdr->hdr_1st = ETHERIP_VERSION << 4;
        hdr->hdr_2nd = 0;
        memcpy(hdr+1, buffer, rlen);

        frames[idx] = allocs[idx];
        sizes[idx] = total_len;
        idx++;

        if(idx >= max_burst){
            ssize_t sent = sock_send_burst(sock_fd, frames, sizes, idx, dst_addr, dst_addr_len);
            if(sent == -1){
                fprintf(stderr, "[ERROR]: burst send failed\n");
            }
            for(size_t j = 0; j < idx; j++) free(allocs[j]);
            idx = 0;
        }

    }

    return NULL;
}

static int parse_queue_count(const char *value){
    char *end = NULL;
    long count = strtol(value, &end, 10);
    if(end == value || *end != '\0' || count <= 0){
        return 2;
    }
    return (int)count;
}

int main(int argc, char **argv){
    signal(SIGINT, on_signal);

    if(argc == 1){
        print_usage();
        return 0;
    }

    int domain;
    char src[IPv6_ADDR_STR_LEN];
    char dst[IPv6_ADDR_STR_LEN];
    char tap_name[IFNAMSIZ];
    int mtu = 1500;
    int mq = 0;
    int queue_count = 1;
    int tap_fd;
    int sock_fd;
    int required_arg_cnt;
    

    // parse arguments
    required_arg_cnt = 0;
    for(int i = 1; i < argc; i++){
        if(strcmp(argv[i], "ipv4") == 0){
            required_arg_cnt++;
            domain = AF_INET;
        }    
        if(strcmp(argv[i], "ipv6") == 0){
            required_arg_cnt++;
            domain = AF_INET6;
        }
        if(strcmp(argv[i], "dst") == 0){
            required_arg_cnt++;
            strcpy(dst, argv[++i]);
        }
        if(strcmp(argv[i], "src") == 0){
            required_arg_cnt++;
            strcpy(src, argv[++i]);
        }
        if(strcmp(argv[i], "tap") == 0){
            required_arg_cnt++;
            strcpy(tap_name, argv[++i]);
        }
        if(strcmp(argv[i], "--mtu") == 0){
            mtu = atoi(argv[++i]);
        }
        if(strcmp(argv[i], "--mq") == 0){
            mq = 1;
            if(i + 1 < argc && argv[i + 1][0] != '-' && strspn(argv[i + 1], "0123456789") == strlen(argv[i + 1])){
                queue_count = parse_queue_count(argv[++i]);
            } else {
                queue_count = 2;
            }
        }
        if(strcmp(argv[i], "-h") == 0){
            print_usage();
            return 0;
        }
    }
    if(required_arg_cnt != 4){
        printf("[ERROR]: Too few or too many arguments required.\n");
        printf("Help: etherip -h\n");
        return 0;
    }

    if(queue_count < 1){
        queue_count = 1;
    }

    // init
    if(tap_open(&tap_fd, tap_name, mtu, domain, mq) == -1){
        // Failed to tap_open()
        return 0;
    }

    struct sockaddr_storage src_addr;
    socklen_t sock_len;
    if(domain == AF_INET){
        struct sockaddr_in *src_addr4;
        src_addr4 = (struct sockaddr_in *)&src_addr;

        src_addr4->sin_family = AF_INET;
        inet_pton(AF_INET, src, &src_addr4->sin_addr.s_addr);
        src_addr4->sin_port  = htons(ETHERIP_PROTO_NUM);
        sock_len =  sizeof(*src_addr4);
    }
    else if(domain == AF_INET6){
        struct sockaddr_in6 *src_addr6;
        src_addr6 = (struct sockaddr_in6 *)&src_addr;

        src_addr6->sin6_family = AF_INET6;
        inet_pton(AF_INET6, src, &src_addr6->sin6_addr.s6_addr);
        src_addr6->sin6_port = htons(ETHERIP_PROTO_NUM);
        sock_len = sizeof(*src_addr6);
    }
    
    if(sock_open(&sock_fd, domain, &src_addr, sock_len) == -1){
        // Failed to sock_open()
        return 0;
    }
    
    struct sockaddr_storage dst_addr;
    if(domain == AF_INET){
        struct sockaddr_in *dst_addr4;
        dst_addr4 = (struct sockaddr_in *)&dst_addr;

        dst_addr4->sin_family = AF_INET;
        inet_pton(AF_INET, dst, &dst_addr4->sin_addr.s_addr);
        dst_addr4->sin_port  = htons(ETHERIP_PROTO_NUM);
    }
    else if(domain == AF_INET6){
        struct sockaddr_in6 *dst_addr6;
        dst_addr6 = (struct sockaddr_in6 *)&dst_addr;

        dst_addr6->sin6_family = AF_INET6;
        inet_pton(AF_INET6, dst, &dst_addr6->sin6_addr.s6_addr);
	    dst_addr6->sin6_port = htons(ETHERIP_PROTO_NUM);
    }

    const size_t pair_count = (mq != 0) ? (size_t)queue_count : 1;
    thread_count = pair_count * 2;
    threads = calloc(thread_count, sizeof(*threads));
    if(!threads){
        fprintf(stderr, "[ERROR]: Failed to allocate thread array\n");
        sock_close(sock_fd);
        tap_close(tap_fd);
        return 0;
    }

    int *tap_fds = calloc(pair_count, sizeof(*tap_fds));
    struct handler_args *recv_args = calloc(pair_count, sizeof(*recv_args));
    struct handler_args *send_args = calloc(pair_count, sizeof(*send_args));
    if(!tap_fds || !recv_args || !send_args){
        fprintf(stderr, "[ERROR]: Failed to allocate queue state\n");
        free(tap_fds);
        free(recv_args);
        free(send_args);
        free(threads);
        threads = NULL;
        thread_count = 0;
        sock_close(sock_fd);
        tap_close(tap_fd);
        return 0;
    }

    pthread_barrier_init(&barrier, NULL, thread_count);

    size_t started_threads = 0;
    if(mq != 0){
        tap_fds[0] = tap_fd;
        for(size_t queue = 1; queue < pair_count; queue++){
            if(tap_open(&tap_fds[queue], tap_name, mtu, domain, 1) == -1){
                fprintf(stderr, "[ERROR]: Failed to open mq tap queue %zu\n", queue);
                goto cleanup_threads;
            }
        }
    } else {
        tap_fds[0] = tap_fd;
    }

    for(size_t queue = 0; queue < pair_count; queue++){
        recv_args[queue].domain = domain;
        recv_args[queue].sock_fd = sock_fd;
        recv_args[queue].tap_fd = tap_fds[queue];
        recv_args[queue].dst_addr = &dst_addr;
        recv_args[queue].barrier = &barrier;

        send_args[queue].domain = domain;
        send_args[queue].sock_fd = sock_fd;
        send_args[queue].tap_fd = tap_fds[queue];
        send_args[queue].dst_addr = &dst_addr;
        send_args[queue].barrier = &barrier;

        if(pthread_create(&threads[queue * 2], NULL, recv_handlar, &recv_args[queue]) != 0){
            fprintf(stderr, "[ERROR]: Failed to create recv thread for queue %zu\n", queue);
            goto cleanup_threads;
        }
        started_threads++;

        if(pthread_create(&threads[queue * 2 + 1], NULL, send_handlar, &send_args[queue]) != 0){
            fprintf(stderr, "[ERROR]: Failed to create send thread for queue %zu\n", queue);
            goto cleanup_threads;
        }
        started_threads++;
    }

    fprintf(stdout, "[INFO]: Started etherip. dst: %s src: %s queues: %zu\n", dst, src, pair_count);

    for(size_t i = 0; i < thread_count; i++){
        pthread_join(threads[i], NULL);
    }

cleanup_threads:
    if(started_threads > 0){
        for(size_t i = 0; i < started_threads; i++){
            pthread_kill(threads[i], SIGHUP);
        }
        for(size_t i = 0; i < started_threads; i++){
            pthread_join(threads[i], NULL);
        }
    }
    pthread_barrier_destroy(&barrier);

    // cleanup
    sock_close(sock_fd);
    for(size_t i = 0; i < pair_count; i++){
        if(tap_fds[i] >= 0){
            tap_close(tap_fds[i]);
        }
    }

    free(send_args);
    free(recv_args);
    free(tap_fds);
    free(threads);

    return 0;
}