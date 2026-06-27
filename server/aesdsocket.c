// Socket server program
//
// Gino Calgaro

#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/queue.h>
#include <pthread.h>
#include <time.h>
#include <sys/select.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "../aesd-char-driver/aesd_ioctl.h"

#ifndef SLIST_FOREACH_SAFE
#define SLIST_FOREACH_SAFE(var, head, field, tvar)           \
    for ((var) = SLIST_FIRST((head));                        \
         (var) && ((tvar) = SLIST_NEXT((var), field), 1);    \
         (var) = (tvar))
#endif

//#define DATA_FILE "/var/tmp/aesdsocketdata"

#if USE_AESD_CHAR_DEVICE
    #define DATA_FILE "/dev/aesdchar"
#else
    #define DATA_FILE "/var/tmp/aesdsocketdata"
#endif

#define BUFFER_SIZE 1024

// extern bool signaled;	// Used for signal interrupts

static int server_fd = -1;
static volatile sig_atomic_t signaled = 0;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t thread_list_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct threadData 
{
    pthread_t threadID;
    int client_fd;
    char clientIP[16];
    volatile int complete;
    SLIST_ENTRY(threadData) entries;
} threadData_t;

typedef struct {
    int client_fd;
    threadData_t *node;
} workerArgs_t;

SLIST_HEAD(slisthead, threadData) head = SLIST_HEAD_INITIALIZER(head);

static void handle_signal(int signo)
{
    (void)signo;
    syslog(LOG_INFO, "Signal catch, exiting");
    signaled = 1;

    if (server_fd != -1) 
    {
        close(server_fd);
        server_fd = -1;
    }

    remove(DATA_FILE);
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

static threadData_t *thread_list_add(pthread_t tid, int client_fd, const char *client_ip)
{
    threadData_t *node = calloc(1, sizeof(*node));

    if (!node)
    {
        return NULL;
    }

    node->threadID = tid;
    node->client_fd = client_fd;
    node->complete = 0;
    strncpy(node->clientIP, client_ip, sizeof(node->clientIP) - 1);

    pthread_mutex_lock(&thread_list_lock);
    SLIST_INSERT_HEAD(&head, node, entries);
    pthread_mutex_unlock(&thread_list_lock);

    return node;
}

static void thread_list_remove(threadData_t *node)
{
    pthread_mutex_lock(&thread_list_lock);
    SLIST_REMOVE(&head, node, threadData, entries);
    pthread_mutex_unlock(&thread_list_lock);

    free(node);
}

static void shutdown_all_threads(void)
{
    pthread_mutex_lock(&thread_list_lock);
    threadData_t *n;
    SLIST_FOREACH(n, &head, entries)
    {
        n->complete = 1;
        shutdown(n->client_fd, SHUT_RDWR);
    }

    pthread_mutex_unlock(&thread_list_lock);

    while (!SLIST_EMPTY(&head))
    {
        threadData_t *node = SLIST_FIRST(&head);
        pthread_join(node->threadID, NULL);
        thread_list_remove(node);
    }
}


static void * connect_thread(void * threadParam)
{
    workerArgs_t params = *(workerArgs_t *)threadParam;
    free(threadParam);

    int client_fd = params.client_fd;
    threadData_t *theData = params.node;
    char recv_buf[BUFFER_SIZE];
    char *packet = NULL;
    size_t packet_size = 0;

    while (!theData->complete)
    {
        ssize_t bytesRead = recv(client_fd, recv_buf, sizeof(recv_buf), 0);

        if (bytesRead <= 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            break;
        }

        else if (bytesRead == 0)
        {
            break;
        }

        size_t offset = 0;
        char *newLine;

        while ((newLine = memchr(recv_buf + offset, '\n', (size_t)bytesRead - offset)) != NULL)
        {
            size_t chunk_len = (size_t)(newLine - (recv_buf + offset)) + 1;

            char *newbuf = realloc(packet, packet_size + bytesRead);

            if (!newbuf)
            {
                syslog(LOG_ERR, "realloc failed");
                free(packet);
                packet = NULL;
                packet_size = 0;
                break;
            }

            packet = newbuf;
            memcpy(packet + packet_size, recv_buf + offset, chunk_len);
            packet_size += chunk_len;

            if (strncmp(recv_buf, "AESD_IOCSEEKTO:", 19) == 0)
            {
#if USE_AESD_CHAR_DEVICE
         
                uint32_t writeCmd, writeCmdOffset;

                if (sscanf(packet, "AESDCHAR_IOCSEEKTO:%u,%u", &writeCmd, &writeCmdOffset) == 2)
                {
                    struct aesd_seekto seekTo;
                    seekTo.write_cmd = writeCmd;
                    seekTo.write_cmd_offset = writeCmdOffset;

                    pthread_mutex_lock(&file_mutex);

                    int fd = open(DATA_FILE, O_RDWR);

                    if (ioctl(fd, AESDCHAR_IOCSEEKTO, &seekTo) == -1)
                    {
                        syslog(LOG_ERR, "ioctl AESDCHAR_IOCSEEKTO failure: %m");
                        close(fd);
                    }
                    else
                    {
                        char send_buf[BUFFER_SIZE];
                        ssize_t bytesToSend;

                        while ((bytesToSend = read(fd, send_buf, sizeof(send_buf))) > 0)
                        {
                            send(client_fd, send_buf, bytesToSend, 0);
                        }

                        close(fd);
                    }

                    pthread_mutex_unlock(&file_mutex);
            }
#else

            syslog(LOG_WARNING, "AESDCHAR_IOCSEEKTO received, but USE_AESD_CHAR_DEVICE is false");
            
#endif
        }
        else
        {
            pthread_mutex_lock(&file_mutex);

#if USE_AESD_CHAR_DEVICE
          
            int fd = open(DATA_FILE, O_RDWR);

            if (fd == -1)
            {
                syslog(LOG_ERR, "open failed: %m");
            }
            else
            {
                ssize_t written = write(fd, packet, packet_size);

                if (written < 0)
                {
                    syslog(LOG_ERR, "write failed: %m");
                }

                char send_buf[BUFFER_SIZE];
                ssize_t bytesToSend;

                lseek(fd, 0, SEEK_SET);

                while ((bytesToSend = read(fd, send_buf, sizeof(send_buf))) > 0)
                {
                    send(client_fd, send_buf, bytesToSend, 0);
                }

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
                syslog(LOG_ERR, "fopen failed %m");
            }

            fp = fopen(DATA_FILE, "r");

            if (fp)
            {
                char send_buf[BUFFER_SIZE];
                size_t bytesToSend;
            
                while ((bytesToSend = fread(send_buf, 1, sizeof(send_buf), fp)) > 0)
                {
                    send(client_fd, send_buf, bytesToSend, 0);
                }

                fclose(fp);
            }

#endif
            pthread_mutex_unlock(&file_mutex);
        }

        free(packet);
        packet = NULL;
        packet_size = 0;
        offset += chunk_len;
    }

    if (offset < (size_t)bytesRead)
        {
            size_t remaining = (size_t)bytesRead - offset;
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
    syslog(LOG_INFO, "Closed connection from %s", theData->clientIP);

    if (!signaled)
    {
        thread_list_remove(theData);
    }

    return NULL;
}

#if !USE_AESD_CHAR_DEVICE
void * append_timestamp(void * arg)
{
    (void)arg;
    struct timespec ts;

    while (!signaled)
    {
        ts.tv_sec = 0;
        ts.tv_nsec = 100000000;

        for (int i = 0; i < 100 && !signaled; i++)
        {
            while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL) && errno == EINTR)
            {
                if (signaled)
                {
                    return NULL;
                }
            }
        }

        if (signaled)
        {
            break;
        }

        time_t rawtime;
        struct tm *timeinfo;
        char timeStr[100];
        char output[150];

        time(&rawtime);
        timeinfo = localtime(&rawtime);
        strftime(timeStr, sizeof(timeStr), "%a, %d %b %Y %H:%M:%S %z", timeinfo);
        int len = snprintf(output, sizeof(output), "timestamp:%s\n", timeStr);

        pthread_mutex_lock(&file_mutex);
        FILE *f = fopen(DATA_FILE, "a");
        if (f != NULL)
        {
            fwrite(output, 1, len, f);
            fclose(f);
        }

        pthread_mutex_unlock(&file_mutex); 
    }

    return NULL;
}
#endif

int main(int argc, char* argv[])
{
	// Initialize syslog
	openlog("aesdsocket", LOG_PID, LOG_USER);

	// Set up signal handler
	signaled = false;
	struct sigaction new_action;
	int daemon_mode = 0;
	int opt;

	while ((opt = getopt(argc, argv, "d")) != -1)
    {
        if (opt == 'd')
        {
            syslog(LOG_INFO, "Entering daemon mode");
            daemon_mode = 1;
        }
    }

	memset(&new_action, 0, sizeof(new_action));
	new_action.sa_handler = handle_signal;
	
	if (sigaction(SIGTERM, &new_action, NULL) != 0)
	{
		syslog(LOG_ERR, "ERROR: Could not register SIGTERM signal");
		return -1;
	}
	
	if (sigaction(SIGINT, &new_action, NULL))
	{
		syslog(LOG_ERR, "ERROR: Could not register SIGINT signal");
		return -1;
	}

	// Set constant values
	const char* thePort = "9000";
	struct addrinfo	theHints;
	struct addrinfo* servInfo;
	memset(&theHints, 0, sizeof(theHints));
	theHints.ai_flags = AI_PASSIVE;
	theHints.ai_family = AF_INET; 	// Set family to IPv4
	theHints.ai_socktype = SOCK_STREAM;


	// Get address info
	int returnVal;
	
	if ((returnVal = getaddrinfo(NULL, thePort, &theHints, &servInfo)) != 0)
	{
		syslog(LOG_ERR, "ERROR: Failed to get address info");
        closelog();
        return -1;
		// TODO: Set value for exit
	}
	
	int server_socket = socket(servInfo->ai_family, servInfo->ai_socktype, 0);

	if (server_socket == -1)
	{
		syslog(LOG_ERR, "ERROR: Failed to initialize socket");
		// TODO: Set value for exit
	}

	// Set socket options (SO_REUSEADDR)
	int optval = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	if ((returnVal = bind(server_socket, servInfo->ai_addr, servInfo->ai_addrlen)) == -1)
	{
		syslog(LOG_ERR, "ERROR: Failed to bind to port");
        close(server_socket);
        freeaddrinfo(servInfo);
        closelog();
        return -1;
		// TODO: Set value for exit
	}

    freeaddrinfo(servInfo); // Release info about client, no longer needed. Solved memory leak!

    if ((returnVal = listen(server_socket, 4)) == -1)
	{
		syslog(LOG_ERR, "ERROR: Could not listen on socket");
        closelog();
        return -1;
		// TODO: Set value for exit
	}
	
	struct sockaddr_storage client_addr;
	socklen_t addr_size = sizeof(client_addr);

	if (daemon_mode)
	{
        daemonize();
	}

    #if !USE_AESD_CHAR_DEVICE
    pthread_t timeID;   // Initialize timestamp thread
    pthread_create(&timeID, NULL, append_timestamp, NULL);
    #endif

	// Endless loop for accepting new clients
	while (!signaled)
	{
        int client_fd = accept(server_socket, (struct sockaddr *)&client_addr, &addr_size);

        if (client_fd == -1)
        {
            if (signaled)
            {
                break;
            }
            if (errno == EINTR)
            {
                continue;
            }

            syslog(LOG_ERR, "accept failed %m");
            continue;
        }

        char client_ip[16];
        inet_ntop(AF_INET, &(((struct sockaddr_in *)&client_addr)->sin_addr), client_ip, 16);
        syslog(LOG_INFO, "Client connected successfully");

        workerArgs_t *args = malloc(sizeof(*args));
        if (!args)
        {
            syslog(LOG_ERR, "malloc failed dropping connection");
            close(client_fd);
            continue;
        }

        threadData_t *theNode = thread_list_add(0, client_fd, client_ip);

        if (!theNode)
        {
            syslog(LOG_ERR, "thread_list_add failed dropping connection");
            free(args);
            close(client_fd);
            continue;
        }

        args->client_fd = client_fd;
        args->node = theNode;

        pthread_t tID;
        
        if (pthread_create(&tID, NULL, connect_thread, args) != 0)
        {
            syslog(LOG_ERR, "pthread_create failed %m");
            thread_list_remove(theNode);
            free(args);
            close(client_fd);
            continue;
        }

        pthread_mutex_lock(&thread_list_lock);
        theNode->threadID = tID;
        pthread_mutex_unlock(&thread_list_lock);

    }



    //     fd_set rfds;
    //     struct timeval theTime;
    //     FD_ZERO(&rfds);
    //     FD_SET(server_socket, &rfds);

    //     theTime.tv_sec = 1;
    //     theTime.tv_usec = 0;

    //     int result = select(server_socket + 1, &rfds, NULL, NULL, &theTime);
    //     if (result == -1)
    //     {
    //         if (errno = EINTR)
    //         {
    //             continue;
    //         }
    //         break;
    //     }
    //     else if (result == 0)
    //     {
    //         continue;
    //     }

    //     int client_fd = accept(server_socket, (struct sockaddr *)&client_addr, &addr_size);

    //     if (client_fd < 0)
    //     {
    //         if (errno == EINTR)
    //         {
    //             continue;
    //         }
    //         break;
    //     }

    //     threadData_t *theNode = malloc(sizeof(threadData_t));

    //     if (theNode == NULL)
    //     {
    //         close(client_fd);
    //         continue;
    //     }

    //     theNode->client_fd = client_fd;
    //     theNode->complete = 0;
    //     // Retrieve IP address from client

    //     inet_ntop(AF_INET, &(((struct sockaddr_in *)&client_addr)->sin_addr), theNode->clientIP, 16);
    //     syslog(LOG_INFO, "Accepted connection from %s", theNode->clientIP);

    //     if (pthread_create(&(theNode->threadID), NULL, connect_thread, theNode) != 0)
    //     {
    //         close(client_fd);
    //         free(theNode);
    //         continue;
    //     }

    //     pthread_mutex_lock(&file_mutex);
    //     SLIST_INSERT_HEAD(&head, theNode, entries);
    //     pthread_mutex_unlock(&file_mutex);

    //     threadData_t *tempNode;
    //     threadData_t *nextNode;
    //     pthread_mutex_lock(&file_mutex);
    //     SLIST_FOREACH_SAFE(tempNode, &head, entries, nextNode)
    //     {
    //         if (tempNode->complete)
    //         {
    //             SLIST_REMOVE(&head, tempNode, threadData, entries);
    //             pthread_join(tempNode->threadID, NULL);
    //             free(tempNode);
    //         }
    //     }
    //     pthread_mutex_unlock(&file_mutex);

    // }

    syslog(LOG_INFO, "Signal received, exiting...");
    shutdown_all_threads();

    #if !USE_AESD_CHAR_DEVICE
    pthread_join(timeID, NULL);

    remove(DATA_FILE);

    #endif

    // threadData_t *theThread;

    // while (!SLIST_EMPTY(&head))
    // {
    //     theThread = SLIST_FIRST(&head);
    //     SLIST_REMOVE_HEAD(&head, entries);
    //     pthread_join(theThread->threadID, NULL);
    //     free(theThread);
    // }

    // #if !USE_AESD_CHAR_DEVICE
    // if (access(DATA_FILE, F_OK) == 0)
    // {
    //     unlink(DATA_FILE);
    // }
    // #endif

    // if (server_socket >= 0)
    // {
    //     close(server_socket);
    // }

    pthread_mutex_destroy(&thread_list_lock);
    pthread_mutex_destroy(&file_mutex);

	closelog();	
	return 0;
}