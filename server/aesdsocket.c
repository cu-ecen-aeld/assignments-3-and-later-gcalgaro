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

#ifndef SLIST_FOREACH_SAFE
#define SLIST_FOREACH_SAFE(var, head, field, tvar)           \
    for ((var) = SLIST_FIRST((head));                        \
         (var) && ((tvar) = SLIST_NEXT((var), field), 1);    \
         (var) = (tvar))
#endif

#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024

// extern bool signaled;	// Used for signal interrupts

static int server_fd = -1;
static volatile sig_atomic_t signaled = 0;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct threadData 
{
    pthread_t threadID;
    int client_fd;
    char clientIP[16];
    volatile int complete;
    SLIST_ENTRY(threadData) entries;
} threadData_t;

SLIST_HEAD(slisthead, threadData) head = SLIST_HEAD_INITIALIZER(head);

static void handle_signal(int signo)
{
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

void * connect_thread(void * threadParam)
{
    threadData_t *theData = (threadData_t *)threadParam;
    char *recv_buf = NULL;
    size_t totalBytesRecv = 0;
    char chunk[BUFFER_SIZE];
    ssize_t bytesRead = 0;
    int newline = 0;

    while (!newline && !signaled)
    {
        bytesRead = recv(theData->client_fd, chunk, sizeof(chunk), 0);

        if (bytesRead < 0)
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

        char *newbuf = realloc(recv_buf, totalBytesRecv + bytesRead);

        if (newbuf == NULL)
        {
            free(recv_buf);
            close(theData->client_fd);
            theData->complete = 1;
            return NULL;
        }

        recv_buf = newbuf;
        memcpy(recv_buf + totalBytesRecv, chunk, bytesRead);
        totalBytesRecv += bytesRead;

        if (totalBytesRecv > 0 && recv_buf[totalBytesRecv - 1] == '\n')
        {
            newline = 1;
        }
    }

    if (newline && !signaled)
    {
        pthread_mutex_lock(&file_mutex);
        FILE *f = fopen(DATA_FILE, "a+");

        if (f != NULL)
        {
            fwrite(recv_buf, 1, totalBytesRecv, f);
            fflush(f);
            fseek(f, 0, SEEK_SET);
            char send_buf[BUFFER_SIZE];
            size_t bytesToSend;
            
            while ((bytesToSend = fread(send_buf, 1, sizeof(send_buf), f)) > 0)
            {
                send(theData->client_fd, send_buf, bytesToSend, 0);
            }

            fclose(f);

        }

        pthread_mutex_unlock(&file_mutex);
    }

    free(recv_buf);
    close(theData->client_fd);
    syslog(LOG_INFO, "Closed connection from %s", theData->clientIP);
    theData->complete = 1;
    return NULL;
}

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

	memset(&new_action, 0, sizeof(struct sigaction));
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

    pthread_t timeID;   // Initialize timestamp thread
    pthread_create(&timeID, NULL, append_timestamp, NULL);

	// Endless loop for accepting new clients
	while (!signaled)
	{
        fd_set rfds;
        struct timeval theTime;
        FD_ZERO(&rfds);
        FD_SET(server_socket, &rfds);

        theTime.tv_sec = 1;
        theTime.tv_usec = 0;

        int result = select(server_socket + 1, &rfds, NULL, NULL, &theTime);
        if (result == -1)
        {
            if (errno = EINTR)
            {
                continue;
            }
            break;
        }
        else if (result == 0)
        {
            continue;
        }

        int client_fd = accept(server_socket, (struct sockaddr *)&client_addr, &addr_size);

        if (client_fd < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

        threadData_t *theNode = malloc(sizeof(threadData_t));

        if (theNode == NULL)
        {
            close(client_fd);
            continue;
        }

        theNode->client_fd = client_fd;
        theNode->complete = 0;
        // Retrieve IP address from client
        //getpeername(client_socket, (struct sockaddr *) &client_addr, &addr_size);





// void get_ip_str(const struct sockaddr *sa, char *s, size_t maxlen) {
//     if (sa->sa_family == AF_INET) {
//         inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr), s, maxlen);
//     } else {
//         inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr), s, maxlen);
//     }
// }

// &(((struct sockaddr_in *)client_addr)->sin_addr)

        inet_ntop(AF_INET, &(((struct sockaddr_in *)&client_addr)->sin_addr), theNode->clientIP, 16);
        syslog(LOG_INFO, "Accepted connection from %s", theNode->clientIP);

        if (pthread_create(&(theNode->threadID), NULL, connect_thread, theNode) != 0)
        {
            close(client_fd);
            free(theNode);
            continue;
        }

        pthread_mutex_lock(&file_mutex);
        SLIST_INSERT_HEAD(&head, theNode, entries);
        pthread_mutex_unlock(&file_mutex);

        threadData_t *tempNode;
        threadData_t *nextNode;
        pthread_mutex_lock(&file_mutex);
        SLIST_FOREACH_SAFE(tempNode, &head, entries, nextNode)
        {
            if (tempNode->complete)
            {
                SLIST_REMOVE(&head, tempNode, threadData, entries);
                pthread_join(tempNode->threadID, NULL);
                free(tempNode);
            }
        }
        pthread_mutex_unlock(&file_mutex);

    }


        






















	// 	if ((returnVal = listen(server_socket, 4)) == -1)
	// 	{
	// 		syslog(LOG_ERR, "ERROR: Could not listen on socket");
	// 		// TODO: Set value for exit
	// 	}

	// 	if ((client_socket = accept(server_socket, (struct sockaddr *) &client_addr, &addr_size)) == -1)
    //     	{
    //             	syslog(LOG_ERR, "ERROR: Could not accept socket from client");
    //             	// TODO: Set value for exit
    //     	}


	// 	// Retrieve IP address from client
    //     getpeername(client_socket, (struct sockaddr *) &client_addr, &addr_size);
    //     inet_ntop(AF_INET, (struct sockaddr_in *) &client_socket, theIP, 16);
    //     syslog(LOG_INFO, "Accepted connection from %s", theIP);
		
	// 	char recv_buf[1024];
    //     char *packet = NULL;
    //     size_t packet_size = 0;		

	// 	while (1)
    //     {
    //         ssize_t bytes = recv(client_socket, recv_buf, sizeof(recv_buf), 0);

    //         if (bytes <= 0)
	// 		{
    //             break;
	// 		}

    //         size_t offset = 0;
    //         char *newline;

    //         while ((newline = memchr(recv_buf + offset, '\n', bytes - offset)) != NULL)
    //         {
    //             size_t chunk_len = newline - (recv_buf + offset) + 1;
    //             char *tmp = realloc(packet, packet_size + chunk_len);
    //             if (!tmp) 
    //             {
    //                 syslog(LOG_ERR, "realloc failed");
    //                 free(packet);
    //                 packet = NULL;
    //                 packet_size = 0;
    //                 break;
    //             }
                
    //             packet = tmp;
    //             memcpy(packet + packet_size, recv_buf + offset, chunk_len);
    //             packet_size += chunk_len;
	
	// 			FILE *fp = fopen(DATA_FILE, "a+");

    //             if (fp)
    //             {
    //                 	fwrite(packet, 1, packet_size, fp);
    //                 	fclose(fp);
    //             }

    //             fp = fopen(DATA_FILE, "r");

    //             if (fp)
    //             {
    //                 char file_buf[1024];
    //                 size_t r;

    //                 while ((r = fread(file_buf, 1, sizeof(file_buf), fp)) > 0)
    //                 {
    //                     send(client_socket, file_buf, r, 0);
    //                 }

    //                 fclose(fp);
    //             }

    //             free(packet);
    //             packet = NULL;
    //             packet_size = 0;
    //             offset += chunk_len;
    //         }

	// 		if (offset < (size_t)bytes)
    //         {
    //             size_t remaining = bytes - offset;
    //             char *tmp = realloc(packet, packet_size + remaining);

    //             if (!tmp)
    //             {
    //                 syslog(LOG_ERR, "realloc failed");
    //                 free(packet);
    //                 packet = NULL;
    //                 packet_size = 0;
    //                 break;
    //             }

    //             packet = tmp;
    //             memcpy(packet + packet_size, recv_buf + offset, remaining);
    //             packet_size += remaining;
    //         }
    //     }

    //     free(packet);
    //     close(client_socket);
    //     syslog(LOG_INFO, "Closed connection from %s", theIP);
    // }


    syslog(LOG_INFO, "Signal received, exiting...");

    pthread_join(timeID, NULL);

    threadData_t *theThread;

    while (!SLIST_EMPTY(&head))
    {
        theThread = SLIST_FIRST(&head);
        SLIST_REMOVE_HEAD(&head, entries);
        pthread_join(theThread->threadID, NULL);
        free(theThread);
    }

    if (access(DATA_FILE, F_OK) == 0)
    {
        unlink(DATA_FILE);
    }

    if (server_socket >= 0)
    {
        close(server_socket);
    }

	closelog();	
	return 0;
}
