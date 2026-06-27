#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <pthread.h>
#include <sys/queue.h>
#include <time.h>
#include <sys/ioctl.h>
#include "../aesd-char-driver/aesd_ioctl.h"

/* Constants */
#define PORT            9000
#define BACKLOG         10
#if USE_AESD_CHAR_DEVICE
    #define DATA_FILE       "/dev/aesdchar"
#else    
    #define DATA_FILE       "/var/tmp/aesdsocketdata"
#endif
#define RECV_BUF_SIZE   1024

/* Thread list node */
typedef struct thread_node {
    pthread_t   tid;
    int         client_fd;
    char        client_ip[INET_ADDRSTRLEN];
    volatile int stop;
    SLIST_ENTRY(thread_node) entries;
} thread_node_t;

typedef struct {
    int client_fd;
    thread_node_t *node;
} worker_args_t;

static int server_fd = -1;
static volatile sig_atomic_t exit_requested = 0;
static volatile sig_atomic_t timer_stop = 0;
static SLIST_HEAD(thread_head, thread_node) thread_list = SLIST_HEAD_INITIALIZER(thread_list);

static pthread_mutex_t thread_list_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t file_lock = PTHREAD_MUTEX_INITIALIZER;

#if !USE_AESD_CHAR_DEVICE
static void *timer_thread(void *arg)
{
    (void)arg;

    while (!timer_stop)
    {
        for (int i = 0; i < 10 && !timer_stop; i++)
            sleep(1);

        if (timer_stop) break;

        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char timestamp[64];
        strftime(timestamp, sizeof(timestamp),
                "timestamp:%a, %d %b %Y %T %z\n", tm_info);

        pthread_mutex_lock(&file_lock);
        FILE *fp = fopen(DATA_FILE, "a");
        if (fp)
        {
            fwrite(timestamp, 1, strlen(timestamp), fp);
            fclose(fp);
        }
        else
        {
            syslog(LOG_ERR, "timer fopen failed %m");
        }

        pthread_mutex_unlock(&file_lock);
    }

    return NULL;
}
#endif

static void handle_signal(int signo)
{
    (void)signo;
    syslog(LOG_INFO, "Signal catch, exiting");
    exit_requested = 1;

    if (server_fd != -1) 
    {
        close(server_fd);
        server_fd = -1;
    }
}


static void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0)
        exit(EXIT_FAILURE);

    if (pid > 0)
        exit(EXIT_SUCCESS);

    if (setsid() == -1)
        exit(EXIT_FAILURE);


    int fd = open("/dev/null", O_RDWR);

    if (fd != -1) 
    {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);

        if (fd > 2)
            close(fd);
    }
}

static thread_node_t *thread_list_add(pthread_t tid, int client_fd, const char *client_ip)
{
    thread_node_t *node = calloc(1, sizeof(*node));
    if (!node)
        return NULL;
    node->tid = tid;
    node->client_fd = client_fd;
    node->stop = 0;
    strncpy(node->client_ip, client_ip, sizeof(node->client_ip) - 1);

    pthread_mutex_lock(&thread_list_lock);
    SLIST_INSERT_HEAD(&thread_list, node, entries);
    pthread_mutex_unlock(&thread_list_lock);

    return node;
}

static void thread_list_remove(thread_node_t *node)
{
    pthread_mutex_lock(&thread_list_lock);
    SLIST_REMOVE(&thread_list, node, thread_node, entries);
    pthread_mutex_unlock(&thread_list_lock);

    free(node);
}

static void shutdown_all_threads(void)
{
    pthread_mutex_lock(&thread_list_lock);
    thread_node_t *n;
    SLIST_FOREACH(n, &thread_list, entries)
    {
        n->stop = 1;
        shutdown(n->client_fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&thread_list_lock);

    while (!SLIST_EMPTY(&thread_list))
    {
        thread_node_t *node = SLIST_FIRST(&thread_list);
        pthread_join(node->tid, NULL);
        thread_list_remove(node);
    }
}

static void *worker_thread(void *arg)
{
    worker_args_t args = *(worker_args_t *)arg;
    free(arg);

    int client_fd = args.client_fd;
    thread_node_t *node = args.node;

    char recv_buf[RECV_BUF_SIZE];
    char *packet = NULL;
    size_t packet_size = 0;

    while (!node->stop)
    {
        ssize_t bytes = recv(client_fd, recv_buf, sizeof(recv_buf), 0);
        if (bytes <= 0)
        {
            if (bytes < 0 && errno == EINTR) 
                continue;
            break;
        }

        size_t offset = 0;
        char *newline;

        while ((newline = memchr(recv_buf + offset, '\n', (size_t)bytes - offset)) != NULL)
        {
            size_t chunk_len = (size_t)(newline - (recv_buf + offset)) + 1;

            char *tmp = realloc(packet, packet_size + chunk_len);
            if (!tmp)
            {
                syslog(LOG_ERR, "realloc failed");
                free(packet);
                packet = NULL;
                packet_size = 0;
                break;
            }
            packet = tmp;
            memcpy(packet + packet_size, recv_buf + offset, chunk_len);
            packet_size += chunk_len;

            if (strncmp(packet, "AESDCHAR_IOCSEEKTO:", 19) == 0)
            {
#if USE_AESD_CHAR_DEVICE
                uint32_t write_cmd, write_cmd_offset;
                if (sscanf(packet, "AESDCHAR_IOCSEEKTO:%u,%u", &write_cmd, &write_cmd_offset) == 2)
                {
                    struct aesd_seekto seekto;
                    seekto.write_cmd = write_cmd;
                    seekto.write_cmd_offset = write_cmd_offset;

                    pthread_mutex_lock(&file_lock);
                    int fd = open(DATA_FILE, O_RDWR);
                    if (ioctl(fd, AESDCHAR_IOCSEEKTO, &seekto) == -1)
                    {
                        syslog(LOG_ERR, "ioctl AESDCHAR_IOCSEEKTO failed: %m");
                        close(fd);
                    }
                    else
                    {
            
                        char file_buf[RECV_BUF_SIZE];
                        ssize_t readRt;
                        while ((readRt = read(fd, file_buf, sizeof(file_buf))) > 0)
                            send(client_fd, file_buf, readRt, 0);
                        close(fd);
                    }
                    pthread_mutex_unlock(&file_lock);
                }
                else
                {
                    syslog(LOG_ERR, "Failed to parse AESDCHAR_IOCSEEKTO command");
                }
#else
                syslog(LOG_WARNING, "AESDCHAR_IOCSEEKTO recieved but USE_AESD_CHAR_DEVICE not enabled");
#endif
            }
            else
            {

                pthread_mutex_lock(&file_lock);

#if USE_AESD_CHAR_DEVICE
                int fd = open(DATA_FILE, O_RDWR);
                if (fd == -1)
                    syslog(LOG_ERR, "open failed: %m");
                else
                {
                    ssize_t written = write(fd, packet, packet_size);
                    if (written < 0)
                        syslog(LOG_ERR, "write failed: %m");

                    char file_buf[RECV_BUF_SIZE];
                    ssize_t readRet;
                    
                    lseek(fd, 0, SEEK_SET);
                    while((readRet = read(fd, file_buf, sizeof(file_buf))) > 0)
                        send(client_fd, file_buf, readRet, 0);
                    
                    close(fd);
                }
#else
                FILE *fp = fopen(DATA_FILE, "a");
                if (fp)
                {
                    fwrite(packet, 1, packet_size, fp);
                    fclose(fp);
                }
                else
                {
                    syslog(LOG_ERR, "fopen a+ failed %m");
                }

                fp = fopen(DATA_FILE, "r");
                if (fp)
                {
                    char file_buf[RECV_BUF_SIZE];
                    size_t readRt;
                    while ((readRt = fread(file_buf, 1, sizeof(file_buf), fp)) > 0)
                        send(client_fd, file_buf, readRt, 0);

                    fclose(fp);
                }
                else
                    syslog(LOG_ERR, "fopen r failed %m");
#endif

                pthread_mutex_unlock(&file_lock);
            }

            free(packet);
            packet = NULL;
            packet_size = 0;
            offset += chunk_len;
        }

        if (offset < (size_t)bytes)
        {
            size_t remaining = (size_t)bytes - offset;
            char *tmp = realloc(packet, packet_size + remaining);
            if (!tmp)
            {
                syslog(LOG_ERR, "realloc failed");
                free(packet);
                packet = NULL;
                packet_size = 0;
                break;
            }
            packet = tmp;
            memcpy(packet + packet_size, recv_buf + offset, remaining);
            packet_size += remaining;
        }
    }

    free(packet);
    close(client_fd);
    syslog(LOG_INFO, "Connection closed from %s", node->client_ip);

    if (!exit_requested)
        thread_list_remove(node);

    return NULL;
}

int main(int argc, char *argv[])
{
    int daemon_mode = 0;
    int opt;

#if USE_AESD_CHAR_DEVICE
    syslog(LOG_INFO, "COMPILED WITH USE_AESD_CHAR_DEVICE - timer not used");
#else
    syslog(LOG_INFO, "COMPILED WITHOUT USE_AESD_CHAR_DEVICE - timer used");
#endif

    while ((opt = getopt(argc, argv, "d")) != -1)
    {
        if (opt == 'd')
        {
            syslog(LOG_INFO, "Entering daemon mode");
            daemon_mode = 1;
        }
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);
    
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) 
    {
        syslog(LOG_ERR, "socket failed");
        return -1;
    }

    int optval = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
   
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) 
    {
        syslog(LOG_ERR, "bind failed %m");
        close(server_fd);
        return -1;
    }

    if (daemon_mode)
    {
        syslog(LOG_INFO, "Entering daemon mode");
        daemonize();
    }

    if (listen(server_fd, BACKLOG) != 0)
    {
        syslog(LOG_ERR, "listen failed %m");
        close(server_fd);
        return -1;
    }
#if !USE_AESD_CHAR_DEVICE
    pthread_t timer_tid;
    if (pthread_create(&timer_tid, NULL, timer_thread, NULL) != 0)
    {
        syslog(LOG_ERR, "failed to create timer thread %m");
        close(server_fd);
        return -1;
    }
#endif

    while (!exit_requested)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        
        if (client_fd == -1)
        {
            if (exit_requested)
                break;
            if (errno == EINTR)
                continue;

            syslog(LOG_ERR, "accept failed %m");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        syslog(LOG_INFO, "Successful connect from %s", client_ip);

        worker_args_t *args = malloc(sizeof(*args));
        if (!args)
        {
            syslog(LOG_ERR, "malloc failed dropping connection");
            close(client_fd);
            continue;
        }

        thread_node_t *node = thread_list_add(0, client_fd, client_ip);
        if (!node)
        {
            syslog(LOG_ERR, "thread_list_add failed dropping connection");
            free(args);
            close(client_fd);
            continue;
        }

        args->client_fd = client_fd;
        args->node = node;

        pthread_t tid;
        if (pthread_create(&tid, NULL, worker_thread, args) != 0)
        {
            syslog(LOG_ERR, "pthread_create failed %m");
            thread_list_remove(node);
            free(args);
            close(client_fd);
            continue;
        }

        pthread_mutex_lock(&thread_list_lock);
        node->tid = tid;
        pthread_mutex_unlock(&thread_list_lock);
    }

    shutdown_all_threads();
#if !USE_AESD_CHAR_DEVICE
    timer_stop = 1;
    pthread_join(timer_tid, NULL);

    remove(DATA_FILE);

#endif

    pthread_mutex_destroy(&thread_list_lock);
    pthread_mutex_destroy(&file_lock);

    closelog();
    return 0;
}