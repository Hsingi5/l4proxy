#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "l4proxy.h"
#include "config.h"

#define MAX_FLOW_NUM (100)

#define MAX_EVENTS (MAX_FLOW_NUM * 3)

#define MAX_BUF_SIZE 16384

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#ifndef ERROR
#define ERROR (-1)
#endif

#define HT_SUPPORT FALSE

#ifndef MAX_CPUS
#define MAX_CPUS 16
#endif

/*----------------------------------------------------------------------------*/
static int num_cores;
static int num_cores_used;
static int core_limit;
static pthread_t app_thread[MAX_CPUS];
static int done[MAX_CPUS];
static char *conf_file = NULL;
static int backlog = -1;

// void XorPacketPayload(char *buf, int buf_size) {
//     for (int i = 0; i < buf_size; i++) {
//         buf[i] = buf[i] ^ 0x01;
//     }
// }

static void
RegisterEvent(struct thread_context *ctx, int sock, uint32_t events)
{
    printf("Register new event %d in epoll descriptor for fd: %d\n", events, sock);
    int ret;
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = sock;

    ret = epoll_ctl(ctx->ep, EPOLL_CTL_ADD, sock, &ev);
    if (ret < 0 && errno != EEXIST)
    {
        printf("epoll_ctl() with EPOLL_CTL_ADD error\n");
        exit(-1);
    }
}

static void
ModifyEvent(struct thread_context *ctx, int sock, uint32_t events)
{
    printf("Modify event %d in epoll descriptor for fd: %d\n", events, sock);
    int ret;
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = sock;

    ret = epoll_ctl(ctx->ep, 3, sock, &ev);

    if (ret < 0 && errno != EEXIST)
    {
        printf("epoll_ctl() with EPOLL_CTL_MOD error (errno = %d)\n", errno);
        exit(-1);
    }
}

static void
UnregisterEvent(struct thread_context *ctx, int sock)
{
    printf("Unregister event from epoll descriptor for fd: %d\n", sock);
    int ret;
    ret = epoll_ctl(ctx->ep, EPOLL_CTL_DEL, sock, NULL);
    if (ret < 0 && errno != EEXIST)
    {
        //		printf("epoll_ctl() with EPOLL_CTL_DEL error\n");
        //		exit(-1);
    }
}

static void
FreeBuffer(struct thread_context *ctx, struct stream_buf *buf)
{
    if (buf == NULL)
        return;

    /* add it to the free list, only if nobody uses this buffer */
    buf->cnt_refs--;

    if (buf->cnt_refs == 0)
    {
        TAILQ_INSERT_TAIL(&ctx->free_hbmap, buf, link);
        buf->data_len = 0;
    }
}

/*----------------------------------------------------------------------------*/
void CloseConnection(struct thread_context *ctx, int sockid)
{
    struct tcp_stream *hs = &ctx->tcp_streams[sockid];

    printf("Closing connection with sock id: %d\n", sockid);

    UnregisterEvent(ctx, sockid);

    close(sockid);

    FreeBuffer(ctx, hs->rbuf);
    FreeBuffer(ctx, hs->wbuf);

    if (hs->endpoint_sock >= 0)
    {
        ctx->tcp_streams[hs->endpoint_sock].endpoint_sock = -1;
    }
}

/*----------------------------------------------------------------------------*/
static int
CreateBackendConnection(struct thread_context *ctx, int frontend_sock)
{
    struct backend_info *backend;
    struct sockaddr_in *backend_addr;
    tcp_stream *backend_stream;
    int backend_fd;
    int ret;

    backend = &g_proxy_ctx->backend;
    backend_addr = &(backend->addr);

    printf("Creating new backend connection\n");

    backend_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (backend_fd < 0)
    {
        printf("error when creating a socket");
        return -1;
    }

    if (backend_fd >= MAX_FLOW_NUM)
    {
        printf("invalid socket id %d.\n", backend_fd);
        return -1;
    }

    ret = fcntl(backend_fd, F_SETFL, O_NONBLOCK);

    if (ret < 0)
    {
        printf("Failed to set socket in nonblocking mode.\n");
        return -1;
    }

    ret = connect(backend_fd, (struct sockaddr *)backend_addr, sizeof(struct sockaddr_in));

    if (ret < 0 && errno != EINPROGRESS)
    {
        perror("mtcp_connect");
        close(backend_fd);
        return -1;
    }

    /* record the socket number of peer TCP stream */
    ctx->tcp_streams[frontend_sock].endpoint_sock = backend_fd;

    backend_stream = &ctx->tcp_streams[backend_fd];
    memset(backend_stream, 0, sizeof(tcp_stream));
    backend_stream->sock_id = backend_fd;
    backend_stream->endpoint_sock = frontend_sock;

    /* forward from front's read buf to backend write buf */
    backend_stream->wbuf = ctx->tcp_streams[frontend_sock].rbuf;
    ctx->tcp_streams[frontend_sock].rbuf->cnt_refs++;

    backend_stream->write_blocked = TRUE;
    RegisterEvent(ctx, backend_fd, EPOLLOUT);

    printf("New backend connection created with socket id: %d\n", backend_fd);

    return 0;
}

/*----------------------------------------------------------------------------*/
static int
AcceptConnection(struct thread_context *ctx, int listener)
{
    struct tcp_stream *t_stream;
    struct sockaddr addr;
    socklen_t addrlen;
    int c_sock, ret;

    printf("Calling accept to check if there are other connections to accept!\n");

    c_sock = accept(listener, &addr, &addrlen);

    printf("New connection accepted! Socked id: %d\n", c_sock);
    if (c_sock < 0)
    {
        if (errno == EAGAIN)
        {
            printf("errno is equal to EAGAIN\n");
            return -1;
        }
        printf("Failed to accept incoming connection.\n");
        fprintf(stderr, "Error on accept() %s\n", strerror(errno));
        exit(-1);
    }

    if (c_sock >= MAX_FLOW_NUM)
    {
        printf("sock id (%d) exceeds the max concurrency (%d).\n",
               c_sock, MAX_FLOW_NUM);
        exit(-1);
    }

    printf("Setting socket as non-blocking!\n");

    ret = fcntl(c_sock, F_SETFL, O_NONBLOCK);

    if (ret < 0)
    {
        printf("setting socket %d nonblocking returns error\n", c_sock);
    }

    t_stream = &ctx->tcp_streams[c_sock];
    memset(t_stream, 0, sizeof(struct tcp_stream));

    t_stream->sock_id = c_sock;
    t_stream->endpoint_sock = -1;
    t_stream->is_fronted = TRUE;

    printf("Registering new input event on new socket!\n");

    RegisterEvent(ctx, c_sock, EPOLLIN);

    return c_sock;
}

void CheckOrAllocateFreeBuffer(struct thread_context *ctx, struct tcp_stream *stream)
{
    if (stream->rbuf == NULL)
    {
        printf("Allocate new read buffer\n");
        stream->rbuf = TAILQ_FIRST(&ctx->free_hbmap);
        if (!stream->rbuf)
        {
            fprintf(stderr, "alloc from free_hbmap fails\n");
            exit(-1);
        }
        TAILQ_REMOVE(&ctx->free_hbmap, stream->rbuf, link);

        /* (for safety) check if the given buffer is being used or has data */
        if (stream->rbuf->cnt_refs > 0)
        {
            fprintf(stderr, "(should not happen) there are still some refs.\n");
            exit(-1);
        }
        if (stream->rbuf->data_len > 0)
        {
            fprintf(stderr, "(should not happen) there are still some data.\n");
            exit(-1);
        }

        /* if there is no peer stream, it is referenced by one TCP stream */
        if (stream->endpoint_sock < 0)
        {
            stream->rbuf->cnt_refs = 1;
        }

        /* if there is a peer stream, it is referenced by two TCP streams */
        else
        {
            ctx->tcp_streams[stream->endpoint_sock].wbuf = stream->rbuf;
            stream->rbuf->cnt_refs = 2;
        }
    }

    /* make sure that it has payload buffer which is allocated during init */
    if (!stream->rbuf->data)
    {
        fprintf(stderr, "hs->rbuf holds a NULL buffer\n");
        exit(-1);
    }
}

// static int writen(struct thread_context *ctx, int fd, char *ptr, int nbytes)
//{
//     int nleft, nwritten;
//
//     nleft = nbytes;
//     while (nleft > 0)
//     {
//         nwritten = mtcp_write(ctx->mctx, fd, ptr, nleft);
//         if(nwritten <= 0)
//             return(nwritten);       /* error */
//
//         nleft -= nwritten;
//         ptr   += nwritten;
//     }
//     return(nbytes - nleft);
// }

static int
WriteAvailData(struct thread_context *ctx, int fd)
{
    tcp_stream *t_stream = &ctx->tcp_streams[fd];
    stream_buf *buff = t_stream->wbuf;
    int res;

    //    if (buff->data_len < 1 || t_stream->write_blocked == 1) {
    //        printf("No data to write or write blocked\n");
    //        return 0;
    //    }

    //    if((res = writen(ctx, fd, buff->data, buff->data_len)) != buff->data_len)
    //    {
    //        printf("Error in writen!\n");
    //        return -1;
    //    }
    res = write(fd, buff->data, buff->data_len);

    printf("Wrote %d bytes to sock id: %d\n", res, fd);

    if (res < 0)
    {
        /* we might have been full but didn't realize it */
        if (errno == EAGAIN)
        {
            printf("We might have been full but didn't realize it\n");
            t_stream->write_blocked = 1;
            ModifyEvent(ctx, fd, EPOLLOUT);
            return 0;
        }

        /* error occured while writing to remote host */
        return -1;
    }

    /* if (res > 0) */
    buff->data_len -= res;

    //    if (t_stream->is_fronted) {
    //        t_stream->bytes_to_write -= res;
    //
    //        /* mismatch cases (exit for debugging purposes now) */
    //        if (t_stream->bytes_to_write < 0 ||
    //            (t_stream->bytes_to_write == 0 && buff->data_len > 0)) {
    //            fprintf(stderr, "content-length mismatch (bytes_to_write: %d, data_len: %d)\n",
    //                    (int) t_stream->bytes_to_write, buff->data_len);
    //            exit(-1);
    //        }
    //
    //        /* finished a HTTP GET, so wait for the next connection */
    //        if (t_stream->bytes_to_write == 0) {
    //            if (t_stream->wbuf->data_len > 0 || t_stream->rbuf->data_len > 0) {
    //                fprintf(stderr, "hs->wbuf->data_len = %d, hs->rbuf->data_len = %d\n",
    //                        t_stream->wbuf->data_len, t_stream->rbuf->data_len);
    //                exit(-1);
    //            }
    //
    //            /* backend connection is already closed */
    //            if (t_stream->endpoint_sock < 0) {
    //                CloseConnection(ctx, fd);
    //                return 0;
    //            }
    //
    //            /* if (hs->peer_sock >= 0) */
    //            /* backend server may close the connection */
    //            ModifyEvent(ctx, fd, MTCP_EPOLLIN);
    //            ModifyEvent(ctx, t_stream->endpoint_sock, MTCP_EPOLLIN);
    //        }
    //    }

    /* since we could not write all, assume that it's blocked */
    if (buff->data_len > 0)
    {
        printf("Left %d bytes to write, add new write event\n", buff->data_len);
        memmove(buff->data, &buff->data[res], buff->data_len);
        t_stream->write_blocked = 1;
        ModifyEvent(ctx, fd, EPOLLOUT);
    }

    return 0;
}

static void HandleReadEvent(struct thread_context *ctx, int fd)
{
    tcp_stream *t_stream;
    int space_left, res;

    printf("Handle Read Event called!\n");
    // if peer is closed, close ourselves
    t_stream = &ctx->tcp_streams[fd];
    // If the backend connection has not been created (yet), the value will be 0
    //    if (t_stream->endpoint_sock < 0) {
    //        CloseConnection(ctx, fd);
    //        return;
    //    }

    printf("Check of allocate free buffer\n");
    /* if there is no read buffer in this stream, bring one from free list */
    CheckOrAllocateFreeBuffer(ctx, t_stream);

    if ((space_left = MAX_BUF_SIZE - t_stream->rbuf->data_len - 1) <= 0)
    {
        // Unregister from read event for a while */
        UnregisterEvent(ctx, fd);
        return;
    }

    printf("Read data from socket %d\n", fd);

    res = read(fd, &t_stream->rbuf->data[t_stream->rbuf->data_len], space_left);

    printf("Read %d byte/s from socket %d\n", res, fd);
    /* when a connection closed by remote host */
    if (res == 0)
    {
        CloseConnection(ctx, fd);
        if (t_stream->rbuf->data_len == 0 && t_stream->endpoint_sock >= 0)
        {
            CloseConnection(ctx, t_stream->endpoint_sock);
            t_stream->endpoint_sock = -1;
        }
        return;
    }

    /* read is unavailable or an error occured */
    if (res == -1)
    {
        if (errno != EAGAIN)
        {
            printf("mtcp_read() error\n");
            fprintf(stderr, "Error on mtcp_read() %s\n", strerror(errno));
            CloseConnection(ctx, fd);
            if (t_stream->rbuf->data_len == 0 && t_stream->endpoint_sock >= 0)
            {
                CloseConnection(ctx, t_stream->endpoint_sock);
                t_stream->endpoint_sock = -1;
            }
        }
        return;
    }

    /* res > 0 */
    t_stream->rbuf->data_len += res;
    t_stream->rbuf->data[t_stream->rbuf->data_len] = 0;

    if (t_stream->is_fronted)
    {
        /* so let's connect to the backend server */
        if (t_stream->endpoint_sock < 0)
        {
            /* case 1: create a new connection (or bring one from pool) */
            printf("Let's create a new endpoint connection\n");
            if (CreateBackendConnection(ctx, fd) < 0)
            {
                CloseConnection(ctx, fd);
            }
            return;
        }
        else
        { /* t_stream->peer_sock >= 0 */
            /* proceed and write available data (= request) to server */
            /* (you already have a backend connetion, go ahead) */
            ModifyEvent(ctx, t_stream->endpoint_sock, EPOLLIN);
            //            RegisterEvent(ctx, t_stream->endpoint_sock, EPOLLIN);
        }
    }
    else
    {
        assert(t_stream->endpoint_sock >= 0);
    }

    printf("Writing available data to socket %d\n", t_stream->endpoint_sock);

    /* try writing available data in the buffer including that we read */
    if (WriteAvailData(ctx, t_stream->endpoint_sock) < 0)
    {
        printf("WriteAvailData() error\n");
        /* close both side of HTTP stream */
        CloseConnection(ctx, fd);
        if (t_stream->endpoint_sock >= 0)
        {
            CloseConnection(ctx, t_stream->endpoint_sock);
            t_stream->endpoint_sock = -1;
        }
    }
}

static void
HandleWriteEvent(struct thread_context *ctx, int fd)
{
    tcp_stream *hs = &ctx->tcp_streams[fd];

    /* unblock it and read what it has */
    hs->write_blocked = FALSE;
    printf("Modifying write event for sock id: %d\n", fd);
    //    UnregisterEvent(ctx, fd);
    //    RegisterEvent(ctx, fd, EPOLLIN);
    ModifyEvent(ctx, fd, EPOLLIN);

    /* enable reading on peer just in case it was off */
    if (hs->endpoint_sock >= 0)
    {
        printf("Registering read event for sock id: %d\n", hs->endpoint_sock);
        RegisterEvent(ctx, hs->endpoint_sock, EPOLLIN);
    }

    printf("Writing available data to sock id: %d\n", fd);
    /* if we have data, write it */
    if (WriteAvailData(ctx, fd) < 0)
    {
        /* if write fails, close the HTTP stream */
        CloseConnection(ctx, fd);
        if (hs->endpoint_sock >= 0)
        {
            CloseConnection(ctx, hs->endpoint_sock);
            hs->endpoint_sock = -1;
        }
        return;
    }

    //    RegisterEvent(ctx, fd,EPOLLIN);

    /* if peer is closed and we're done writing, we should close */
    if (hs->endpoint_sock < 0 && hs->wbuf->data_len == 0)
    {
        CloseConnection(ctx, fd);
    }
}

/*----------------------------------------------------------------------------*/
int CreateListeningSocket(struct thread_context *ctx)
{
    int listener;
    int ret;

    listener = socket(AF_INET, SOCK_STREAM, 0);

    if (listener < 0)
    {
        printf("Failed to create listening socket!\n");
        return -1;
    }

    /* we won't linger on close (as mTCP does) */
    struct linger linger_opt;
    linger_opt.l_onoff = 0;
    linger_opt.l_linger = 0;
    if (setsockopt(listener, SOL_SOCKET, SO_LINGER, &linger_opt, sizeof(linger_opt)) < 0)
    {
        printf("Failed to turn off linger option\n");
        return -1;
    }

    /* reuse address */
    int reuse_opt = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   &reuse_opt, sizeof(reuse_opt)) < 0)
    {
        printf("Failed to turn on reuse option\n");
        return -1;
    }

    ret = fcntl(listener, F_SETFL, O_NONBLOCK);

    if (ret < 0)
    {
        printf("Failed to set socket in nonblocking mode.\n");
        return -1;
    }

    ret = bind(listener, (struct sockaddr *)&(g_proxy_ctx->listen_addr), sizeof(struct sockaddr_in));

    if (ret < 0)
    {
        printf("Failed to bind to the listening socket!\n");
        return -1;
    }

    ret = listen(listener, backlog);

    if (ret < 0)
    {
        printf("mtcp_listen() failed!\n");
        return -1;
    }

    /* wait for incoming accept events */
    RegisterEvent(ctx, listener, EPOLLIN);

    return listener;
}

int initEpollDescriptor(struct thread_context *ctx, struct epoll_event **events)
{
    ctx->ep = epoll_create(MAX_EVENTS);
    if (ctx->ep < 0)
    {
        printf("Failed to create epoll descriptor!\n");
        return -1;
    }

    *events = (struct epoll_event *)calloc(MAX_EVENTS, sizeof(struct epoll_event));

    if (!*events)
    {
        printf("Failed to create event struct!\n");
        return -1;
    }

    return 0;
}

int initServerVariables(struct thread_context *ctx)
{
    ctx->tcp_streams = (struct tcp_stream *)calloc(MAX_FLOW_NUM, sizeof(struct tcp_stream));
    if (!ctx->tcp_streams)
    {
        return -1;
    }

    return 0;
}

int initFreeFlowBuffers(struct thread_context *ctx)
{
    /* initialize memory pool for flow buffers */
    ctx->hbmap = (stream_buf *)calloc(MAX_FLOW_NUM, sizeof(struct stream_buf));

    if (!ctx->hbmap)
    {
        printf("Failed to allocate memory for flow buffer map.\n");
        return -1;
    }

    for (int i = 0; i < MAX_FLOW_NUM; i++)
    {
        ctx->hbmap[i].data = (char *)calloc(1, MAX_BUF_SIZE);
        if (!ctx->hbmap[i].data)
        {
            printf("Failed to allocate memory for flow buffer.\n");
            return -1;
        }
    }

    TAILQ_INIT(&ctx->free_hbmap);
    for (int i = 0; i < MAX_FLOW_NUM; i++)
        TAILQ_INSERT_TAIL(&ctx->free_hbmap, &ctx->hbmap[i], link);

    return 0;
}

/*----------------------------------------------------------------------------*/
void RunMainLoop(void *arg_ctx)
{
    struct thread_context *ctx;
    int nevents;
    int i, ret, err;
    socklen_t len = sizeof(err);
    int do_accept;

    ctx = (struct thread_context *)arg_ctx;

    printf("Run application on core %d\n", ctx->cpu);

    struct epoll_event *events;

    printf("Initialize EPOLL Descriptors!\n");
    // Create epoll descriptor
    ret = initEpollDescriptor(ctx, &events);
    if (ret < 0)
    {
        printf("Error while initializing epoll descriptor!\n");
        exit(-1);
    }

    printf("Allocate memory for server variables!\n");
    // Allocate memory for server variables
    ret = initServerVariables(ctx);
    if (ret < 0)
    {
        printf("Failed to create server_vars struct!\n");
        exit(-1);
    }

    printf("Allocate memory for free flow buffers!\n");
    // Allocate memory for free flow buffers
    ret = initFreeFlowBuffers(ctx);
    if (ret < 0)
    {
        printf("Failed to allocate flow buffers!\n");
        exit(-1);
    }

    printf("Create listening socket!\n");
    ctx->listener = CreateListeningSocket(ctx);
    if (ctx->listener < 0)
    {
        printf("Failed to create listening socket.\n");
        exit(-1);
    }

    while (1)
    {
        nevents = epoll_wait(ctx->ep, events, MAX_EVENTS, 1000);
        if (nevents < 0 && errno != EINTR)
        {
            if (errno == EPERM)
                break;
            printf("mtcp_epoll_wait() error\n");
            exit(-1);
        }

        do_accept = FALSE;
        for (i = 0; i < nevents; i++)
        {

            printf("New event arrived on sock id: %d\n", events[i].data.fd);
            if (events[i].data.fd == ctx->listener)
            {
                do_accept = TRUE;
            }
            else if (events[i].events & EPOLLIN)
            {
                printf("New READ event arrived on sock id: %d\n", events[i].data.fd);
                HandleReadEvent(ctx, events[i].data.fd);
            }
            else if (events[i].events & EPOLLOUT)
            {
                printf("New WRITE event arrived on sock id: %d\n", events[i].data.fd);
                HandleWriteEvent(ctx, events[i].data.fd);
            }
            else if (events[i].events & EPOLLERR)
            {
                ret = getsockopt(events[i].data.fd,
                                 SOL_SOCKET, SO_ERROR,
                                 (void *)&err, &len);
                if (ret == 0)
                {
                    if (err == ETIMEDOUT)
                        continue; /* continue for epoll timeout case */
                    else
                    {
                        printf("epoll error: %s\n", strerror(err));
                        exit(-1);
                    }
                }
                else
                {
                    printf("getsockopt error: %s\n", strerror(errno));
                    exit(-1); /* for debugging now */
                }
            }
            else if (events[i].events & EPOLLHUP)
            {
                fprintf(stderr, "EPOLLHUP\n");
                exit(-1); /* for debugging now */
            }
            else if (events[i].events & EPOLLRDHUP)
            {
                fprintf(stderr, "EPOLLRDHUP\n");
                exit(-1); /* for debugging now */
            }
            else
            {
                /* Unknown epoll flag */
                fprintf(stderr, "unknown epoll flag\n");
                exit(-1);
            }
        }

        // if do_accept flag is set, accept connections
        if (do_accept)
        {
            while (AcceptConnection(ctx, ctx->listener) >= 0)
                ;
        }
    }

    free(ctx->tcp_streams);
    free(events);
}

/*----------------------------------------------------------------------------*/
void *
RunMTCP(void *arg)
{
    int core = *(int *)arg;
    struct thread_context *ctx = (struct thread_context *)calloc(1, sizeof(struct thread_context));
    if (!ctx)
    {
        printf("Failed to create thread context!\n");
        exit(-1);
    }

    ctx->cpu = core;

    /* run main application loop */
    RunMainLoop((void *)ctx);

    free(ctx);

    pthread_exit(NULL);
}

/*----------------------------------------------------------------------------*/
void SignalHandler(int signum)
{
    int i;

    for (i = 0; i < core_limit; i++)
    {
        if (app_thread[i] == pthread_self())
        {
            // printf("Server thread %d got SIGINT\n", i);
            done[i] = TRUE;
        }
        else
        {
            if (!done[i])
            {
                pthread_kill(app_thread[i], signum);
            }
        }
    }
}

/*----------------------------------------------------------------------------*/
static void
printHelp(const char *prog_name)
{
    printf("%s -p <path_to_www/> -f <mtcp_conf_file> "
           "[-N num_cores] [-c <per-process core_id>] [-h]\n",
           prog_name);
    exit(EXIT_SUCCESS);
}

/*----------------------------------------------------------------------------*/

int main(int argc, char **argv)
{
    int cores[MAX_CPUS];
    int i, o;
    num_cores = 8; // we set 8 as default num of our cpu cores
                   /* soft limit for sockets */
    struct rlimit limit;
    limit.rlim_cur = MAX_FLOW_NUM;
    limit.rlim_max = MAX_FLOW_NUM;
    if (setrlimit(RLIMIT_NOFILE, &limit) < 0)
    {
        printf("failed to increase number of fds\n");
        exit(-1);
    }

    num_cores = sysconf(_SC_NPROCESSORS_ONLN);

    /* if backlog is not specified, set it to 4K */
    if (backlog == -1)
    {
        backlog = 199;
    }

    printf("Application initialization finished.\n");

    /* read blu5_proxy configuration from config/blu5_proxy.yaml */
    g_proxy_ctx = LoadConfigData("/home/wxy/empty/l4proxy/config/blu5_proxy.yaml");
    if (!g_proxy_ctx)
    {
        printf("LoadConfigData() error\n");
        exit(-1);
    }

    if (g_proxy_ctx->backend_num < 1)
    {
        printf("No Available Backend Server.\n");
        exit(-1);
    }

    num_cores_used = 0;
    core_limit = 1;
    for (i = 0; i < core_limit; i++)
    {
        cores[i] = i;
        num_cores_used++;
        printf("Creating thread %d\n", i);
        if (pthread_create(&app_thread[i], NULL, RunMTCP, (void *)&cores[i]))
        {
            printf("Failed to create msg_test thread.\n");
            exit(-1);
        }
    }

    for (i = 0; i < num_cores_used; i++)
    {
        pthread_join(app_thread[i], NULL);
        printf("Message test thread %d joined.\n", i);
    }

    return 0;
}
