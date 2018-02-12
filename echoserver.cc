#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <iostream>
#include <cstring>
#include <vector>
#include <pthread.h>

using namespace std;

/* Const messages and global variables */
const char *QUIT = "+OK Goodbye!\r\n";
const char *GREETING = "+OK Server ready (Author: Gongyao Chen / gongyaoc)\r\n";
const char *UNKNOWN_COMM = "-ERR Unknown command\r\n";
const char *SHUTDOWN = "-ERR Server shutting down\r\n";
const char *OVER_SIZE = "-ERR Input exceed maximum input size (1024)\r\n";

vector<pthread_t> THREADS;
vector<unsigned int> SOCKETS;
unsigned int listen_fd;
bool DEBUG;
bool RUNNING;

/* Set nonblocking read() function */
void set_nonblocking(unsigned int fd)
{
    int flags;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        perror("fcntl(F_GETFL) failed.\n");
        exit(1);
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        perror("fcntl(F_SETFL) failed.\n");
        exit(1);
    }
}

/* Signal handler for ctrl-c, clean threads and quit */
void sig_handler(int arg)
{
    RUNNING = false;
    if (DEBUG)
    {
        printf("\nServer socket closed\n");
    }
    for (int i = 0; i < SOCKETS.size(); i++)
    {
        write(SOCKETS[i], SHUTDOWN, strlen(SHUTDOWN));
        close(SOCKETS[i]);
    }
    SOCKETS.clear();
    close(listen_fd);
    for (int i = 0; i < THREADS.size(); i++)
    {
        pthread_join(THREADS[i], NULL);
        //pthread_kill(THREADS[i],0);
    }
    THREADS.clear();
}

/* Thread function for handling a client */
void *client_t(void *p)
{
    unsigned int fd = *(int *) p;
    write(fd, GREETING, strlen(GREETING)); // greeting message

    bool disconnect = false;
    char buffer[1025] = { };
    char *head = buffer;

    /* Start to echo */
    while (RUNNING)
    {
        int len = strlen(buffer);
        int recv_len = read(fd, head, 1024 - len);
        //		if (DEBUG) {
        //			if (recv_len > 0) {
        //				printf("Client %d received %d char, is %s\n", fd, recv_len,
        //						head);
        //			}
        //		}
        char *tail = 0;
        /* Process a message when the end of a line is found */
        while ((tail = strstr(buffer, "\r\n")) != NULL)
        {
            tail += 2;
            char command[5] = { };
            for (int i = 0; i < 4; i++)
            {
                command[i] = buffer[i];
            }
            if (strcasecmp(command, "ECHO") == 0)
            {
                char message[1025] = { };
                strcpy(message, "+OK ");
                char *re_head = buffer + 5;
                int mess_len = tail - re_head;
                strncpy(&message[4], re_head, mess_len);
                write(fd, message, strlen(message)); // echo response
                if (DEBUG)
                {
                    fprintf(stderr, "[%d] C: %s\n", fd, command);
                    fprintf(stderr, "[%d] S: %s", fd, message);
                }
            }
            else if (strcasecmp(command, "QUIT") == 0)
            {
                write(fd, QUIT, strlen(QUIT)); // quit response
                disconnect = true;
                if (DEBUG)
                {
                    fprintf(stderr, "[%d] C: %s\n", fd, command);
                    fprintf(stderr, "[%d] S: %s", fd, QUIT);
                    fprintf(stderr, "[%d] Client request to close connection\n",
                            fd);
                }
                break;
            }
            else
            {
                write(fd, UNKNOWN_COMM, strlen(UNKNOWN_COMM)); // unknown command
                if (DEBUG)
                {
                    fprintf(stderr, "[%d] C: %s\n", fd, command);
                    fprintf(stderr, "[%d] S: %s", fd, UNKNOWN_COMM);
                    fprintf(stderr, "[%d] Client sent unknown command\n", fd);
                }
            }

            /* Move remain messages to the head */
            char *new_head = buffer;
            while (new_head != tail)
            {
                *new_head = '\0';
                new_head++;
            }
            new_head = buffer;
            while (*tail != '\0')
            {
                *new_head = *tail;
                *tail = '\0';
                new_head++;
                tail++;
            }
        }
        if (disconnect)
        {
            break;
        }
        head = buffer;
        int i = 0;
        while (*head != '\0')
        {
            head++; // find next start point for reading
            i++;
            if (i >= 1024)   // buffer is full
            {
                if (DEBUG)
                {
                    fprintf(stderr, "Out of buffer bound.\n");
                }
                write(fd, OVER_SIZE, strlen(OVER_SIZE));
                write(fd, buffer, 1024);
                memset(buffer, 0, 1024);
                head = buffer;
                break;
            }
        }
    }

    /* Close client connection */
    if (RUNNING)
    {
        close(fd);
    }
    if (DEBUG)
    {
        fprintf(stderr, "[%d] Connection closed\n", fd);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    /* Your code here */

    /* Handling shutdown signal */
    signal(SIGINT, sig_handler);

    /* Parsing command line arguments */
    int ch = 0;
    unsigned int port_N = 10000;
    while ((ch = getopt(argc, argv, "p:av")) != -1)
    {
        switch (ch)
        {
        case 'a':
            fprintf(stderr, "Full name: Gongyao Chen, SEAS login: gongyaoc\n");
            exit(0);
        case 'v':
            DEBUG = true;
            break;
        case 'p':
            port_N = atoi(optarg);
            if (port_N <= 0)
            {
                fprintf(stderr, "Invalid port number: %s\n",
                        optarg);
                exit(1);
            }
            break;
        case '?':
            fprintf(stderr, "Error: Invalid choose: %c\n", (char) optopt);
            exit(1);
        default:
            fprintf(stderr, "Error: Please input [-p port_num] [-a] [-v]\n");
            exit(1);
        }
    }

    struct sockaddr_in server_addr, client_addr; // Structures to represent the server and client

    /* Create a new socket */
    if ((listen_fd = socket(PF_INET, SOCK_STREAM, 0)) == -1)
    {
        fprintf(stderr, "Socket open error.\n");
        exit(1);
    }
    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &reuse,
                   sizeof(int)) == -1)
    {
        fprintf(stderr, "Socket set error.\n");
        exit(1);
    }
    //set_nonblocking(listen_fd);

    /* Configure the server */
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htons(INADDR_ANY);
    server_addr.sin_port = htons(port_N);

    /* Use the socket and associate it with the port number */
    if (bind(listen_fd, (struct sockaddr *) &server_addr,
             sizeof(struct sockaddr)) == -1)
    {
        fprintf(stderr, "Unable to bind.\n");
        exit(1);
    }

    /* Start to listen client connections */
    if (listen(listen_fd, 100) == -1)
    {
        fprintf(stderr, "Unable to listen.\n");
        exit(1);
    }
    RUNNING = true;
    if (DEBUG)
    {
        printf("Server configured to listen on port %d\n", port_N);
    }
    fflush(stdout);

    //	fd_set readfds;
    //	int maxfd = 1;
    //	struct timeval timeout;

    while (RUNNING)
    {
        /* Set up client connections */
        socklen_t clientaddrlen = sizeof(client_addr);

        /* Set the accept function to nonblock */
        //		FD_ZERO(&readfds);
        //		FD_SET(listen_fd, &readfds);
        //		timeout.tv_sec = 5;
        //		timeout.tv_usec = 0;
        //		int res = select(maxfd, &readfds, NULL, NULL, &timeout);
        //		if (res <= 0) {
        //			printf("No coming...\n");
        //			continue;
        //		}
        unsigned int comm_fd = accept(listen_fd,
                                      (struct sockaddr *) &client_addr, &clientaddrlen);
        if (!RUNNING || comm_fd == -1)
        {
            break;
        }
        set_nonblocking(comm_fd);
        SOCKETS.push_back(comm_fd);
        if (DEBUG)
        {
            fprintf(stderr, "[%d] New connection\n", comm_fd);
        }

        /* Assign the client to a thread */
        pthread_t thread;
        pthread_create(&thread, NULL, &client_t, &comm_fd);
        THREADS.push_back(thread);
    }

    if (DEBUG)
    {
        printf("Server successfully shut down.\n");
    }
    return 0;
}
