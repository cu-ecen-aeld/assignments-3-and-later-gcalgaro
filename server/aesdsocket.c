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

// Define addrinfo struct

//struct addrinfo
//{
//        int     ai_flags;
//        int     ai_family;
//        int     ai_socktype;
//        int     ai_protocol;
//        size_t  ai_addrlen;
//        struct sockaddr *ai_addr;
//        char    *ai_canonname;
//
//        struct addrinfo *ai_next;
//};

#define DATA_FILE "/var/tmp/aesdsocketdata"

// extern bool signaled;	// Used for signal interrupts

static int server_fd = -1;
static volatile sig_atomic_t signaled = 0;

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
	socklen_t optlen;
	if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) != 0)
	{
		perror("setsockopt");
	}	

	getsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &optval, &optlen);
	
	if (optval != 0)
	{
		syslog(LOG_DEBUG, "SO_REUSEADDR enabled");
	}

	if ((returnVal = bind(server_socket, servInfo->ai_addr, servInfo->ai_addrlen)) == -1)
	{
		syslog(LOG_ERR, "ERROR: Failed to bind to port"); 
		// TODO: Set value for exit
	}
	
	char theIP[16]; 	// Character array to hold IP address string

	int client_socket;
	struct sockaddr_storage client_addr;
	socklen_t addr_size = sizeof(client_addr);

	if (daemon_mode)
	{
        daemonize();
	}

	// Endless loop for accepting new clients
	while (!signaled)
	{
		if ((returnVal = listen(server_socket, 4)) == -1)
		{
			syslog(LOG_ERR, "ERROR: Could not listen on socket");
			// TODO: Set value for exit
		}

		if ((client_socket = accept(server_socket, (struct sockaddr *) &client_addr, &addr_size)) == -1)
        	{
                	syslog(LOG_ERR, "ERROR: Could not accept socket from client");
                	// TODO: Set value for exit
        	}


		// Retrieve IP address from client
        getpeername(client_socket, (struct sockaddr *) &client_addr, &addr_size);
        inet_ntop(AF_INET, (struct sockaddr_in *) &client_socket, theIP, 16);
        syslog(LOG_INFO, "Accepted connection from %s", theIP);
		
		char recv_buf[1024];
        char *packet = NULL;
        size_t packet_size = 0;		

		while (1)
        {
            ssize_t bytes = recv(client_socket, recv_buf, sizeof(recv_buf), 0);

            if (bytes <= 0)
			{
                break;
			}

            size_t offset = 0;
            char *newline;

            while ((newline = memchr(recv_buf + offset, '\n', bytes - offset)) != NULL)
            {
                size_t chunk_len = newline - (recv_buf + offset) + 1;
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
	
				FILE *fp = fopen(DATA_FILE, "a+");

                if (fp)
                {
                    	fwrite(packet, 1, packet_size, fp);
                    	fclose(fp);
                }

                fp = fopen(DATA_FILE, "r");

                if (fp)
                {
                    char file_buf[1024];
                    size_t r;

                    while ((r = fread(file_buf, 1, sizeof(file_buf), fp)) > 0)
                    {
                        send(client_socket, file_buf, r, 0);
                    }

                    fclose(fp);
                }

                free(packet);
                packet = NULL;
                packet_size = 0;
                offset += chunk_len;
            }

			if (offset < (size_t)bytes)
            {
                size_t remaining = bytes - offset;
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
        close(client_socket);
        syslog(LOG_INFO, "Closed connection from %s", theIP);
    }

	closelog();	
	return 0;
}
