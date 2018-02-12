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
#include <fstream>
#include <string>
#include <vector>
#include <unordered_set>
#include <pthread.h>
#include <dirent.h>

using namespace std;

/* Const messages and global variables */
const char *READY = "220 localhost Service ready\r\n";
const char *CLOSE = "221 localhost Service closing transmission channel\r\n";
const char *HELO = "250 localhost\r\n";
const char *OK = "250 OK\r\n";
const char *START = "354 Start mail input; end with <CRLF>.<CRLF>\r\n";
const char *SERV_UNAVAIL =
    "421 localhost Service not available, closing transmission channel\r\n";
const char *UNRECOGNIZED = "500 Syntax error, command unrecognized\r\n";
const char *SYN_ERR = "501 Syntax error in parameters or arguments\r\n";
const char *SEQ_ERR = "503 Bad sequence of commands\r\n";
const char *MAIL_UNAVAIL =
    "550 Requested action not taken: mailbox unavailable\r\n";
const char *OVER_SIZE = "552 Too much mail data\r\n";

vector<pthread_t> THREADS;
pthread_mutex_t lock;
vector<unsigned int> SOCKETS;
unordered_set<string> MBOXES;
unsigned int listen_fd;
string user_dir;
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
    for (int i = 0; i < SOCKETS.size(); i++)
    {
        write(SOCKETS[i], SERV_UNAVAIL, strlen(SERV_UNAVAIL));
        close(SOCKETS[i]);
    }
    SOCKETS.clear();
    close(listen_fd);
    if (DEBUG)
    {
        printf("\nServer socket closed\n");
    }
    for (int i = 0; i < THREADS.size(); i++)
    {
        pthread_join(THREADS[i], NULL);
        //pthread_kill(THREADS[i],0);
    }
    THREADS.clear();
}

/* Add the mailboxes of the local users to a set from a directory */
void get_mboxes()
{
    DIR *dir;
    struct dirent *ptr;
    const char *file = user_dir.c_str();
    if ((dir = opendir(file)) == NULL)
    {
        fprintf(stderr, "Mailbox directory open error.\n");
        exit(1);
    }
    while ((ptr = readdir(dir)) != NULL)
    {
        if (strcmp(ptr->d_name, ".") == 0 || strcmp(ptr->d_name, "..") == 0)
        {
            continue; // current dir or parent dir
        }
        MBOXES.insert(ptr->d_name);
    }
    closedir(dir);
}

/* ECHO command handler that checks the state and send response. If no argument is after HELO then send 501 error. */
void do_helo(unsigned int fd, int &status, char *buffer, string &message)
{
    if (status > 1)
    {
        message = SEQ_ERR;
        write(fd, SEQ_ERR, strlen(SEQ_ERR));
    }
    else
    {
        char *head = buffer, *tail = strstr(buffer, "\r\n");
        if (tail - head <= 5)
        {
            message = SYN_ERR;
            write(fd, SYN_ERR, strlen(SYN_ERR));
        }
        else
        {
            message = HELO;
            write(fd, HELO, strlen(HELO));
            status = 1;
        }
    }
}

/* MAIL FROM command handler that checks the state and set the sender. */
void do_mail(unsigned int fd, int &status, char *buffer, char *sender,
             string &message)
{
    if (status != 1)
    {
        message = SEQ_ERR;
        write(fd, SEQ_ERR, strlen(SEQ_ERR));
    }
    else
    {
        int i = 0, j = 0, len = strlen(buffer);
        while (buffer[i] != '<' && i < len)
        {
            i++;
        }
        i++;
        while (buffer[i] != '>' && i < len)
        {
            sender[j] = buffer[i];
            j++;
            i++;
        }
        message = OK;
        write(fd, OK, strlen(OK));
        status = 2;
    }
}

/* RCPT TO command handler that checks the state and checks if the recipients and hosts exist, then set the recipients. */
void do_rcpt(unsigned int fd, int &status, char *buffer, vector<string> &rcpts,
             string &message)
{
    if (status < 2 || status > 3)
    {
        message = SEQ_ERR;
        write(fd, SEQ_ERR, strlen(SEQ_ERR));
    }
    else
    {
        char one_rcpt[65] = { }, one_host[65] = { };
        int i = 0, j = 0, len = strlen(buffer);
        while (buffer[i] != '<' && i < len)
        {
            i++;
        }
        i++;
        while (buffer[i] != '@' && i < len)
        {
            one_rcpt[j] = buffer[i];
            j++;
            i++;
        }
        i++;
        j = 0;
        while (buffer[i] != '>' && i < len)
        {
            one_host[j] = buffer[i];
            j++;
            i++;
        }
        string mbox = (string) one_rcpt + ".mbox";
        if (strcmp(one_host, "localhost") != 0
                || MBOXES.find(mbox) == MBOXES.end())
        {
            message = MAIL_UNAVAIL;
            write(fd, MAIL_UNAVAIL, strlen(MAIL_UNAVAIL));
        }
        else
        {
            bool has = false;
            for (int i = 0; i < rcpts.size(); i++)
            {
                if (rcpts[i] == mbox)
                {
                    has = true;
                    if (DEBUG)
                    {
                        fprintf(stderr, "[%d] Duplicate recipients\n", fd);
                    }
                    break;
                }
            }
            if (!has)
            {
                rcpts.push_back(mbox);
            }
            message = OK;
            write(fd, OK, strlen(OK));
            status = 3;
        }
    }
}

/* DATA command handler that checks the state and read full message and write to recipients' files. */
void do_data(unsigned int fd, int &status, char *buffer, char *sender,
             vector<string> &rcpts, string &content, char *tail, bool &data,
             string &message)
{
    if (status < 3 || status > 4)
    {
        message = SEQ_ERR;
        write(fd, SEQ_ERR, strlen(SEQ_ERR));
    }
    else if (!data)
    {
        message = START;
        write(fd, START, strlen(START));
        data = true;
        status = 4;
    }
    else if (strcmp(buffer, ".\r\n") == 0)
    {
        for (int i = 0; i < rcpts.size(); i++)
        {
            ofstream mail;
            time_t cur = time(NULL);
            string address = user_dir + "/" + rcpts[i];
            pthread_mutex_lock(&lock);
            mail.open(address, ios::app);
            string title = "From <" + (string) sender + "> " + ctime(&cur);
            mail << title << content;
            mail.close();
            pthread_mutex_unlock(&lock);
        }
        message = OK;
        write(fd, OK, strlen(OK));
        data = false;
        status = 5;
        status = 1;
    }
    else
    {
        string l = "";
        char *head = buffer;
        while (head != tail)
        {
            l += *head;
            head++;
        }
        content += l;
        message = "Reading to content: " + l;
    }
}

/* RSET command handler that checks the state and discard all recipients, sender and content. */
void do_rset(unsigned int fd, int &status, char *sender, vector<string> &rcpts,
             string &content, string &message)
{
    if (status == 0)
    {
        message = SEQ_ERR;
        write(fd, SEQ_ERR, strlen(SEQ_ERR));
    }
    else
    {
        memset(sender, 0, 64);
        rcpts.clear();
        content.clear();
        message = OK;
        write(fd, OK, strlen(OK));
        status = 1;
    }
}

/* Thread function for handling a client */
void *client_t(void *p)
{
    unsigned int fd = *(int *) p;
    write(fd, READY, strlen(READY)); // greeting message

    bool disconnect = false;
    char sender[65] = { };
    char buffer[1024 * 8 + 1] = { };
    char *head = buffer;
    vector<string> rcpts;
    string content = "";
    bool data = false;

    int status = 0; // status for a client: 0 new connect, 1 HELO/REST, 2 MAIL, 3 RCPT, 4 DATA Receiving, 5 DATA processed

    /* Start to respond */
    while (RUNNING)
    {
        int len = strlen(buffer);
        int recv_len = read(fd, head, 1024 * 8 - len);
        //		if (DEBUG) {
        //			if (recv_len > 0) {
        //				printf("Client %d received %d char, is %s\n", fd, recv_len,
        //						head);
        //			}
        //		}
        char *tail = 0;
        /* Process a message when the end of a line is found */
        while (!disconnect && (tail = strstr(buffer, "\r\n")) != NULL)
        {
            tail += 2;
            char command[5] = { };
            for (int i = 0; i < 4; i++)
            {
                command[i] = buffer[i];
            }
            string message, operation;
            if (strcasecmp(command, "HELO") == 0)
            {
                do_helo(fd, status, buffer, message); // helo response
                if (DEBUG)
                {
                    fprintf(stderr, "GOOD [%d] Client sent helo\n", fd);
                }
                operation = "HELO";
            }
            else if (strcasecmp(command, "QUIT") == 0)
            {
                message = CLOSE;
                write(fd, CLOSE, strlen(CLOSE)); // quit response
                disconnect = true;
                if (DEBUG)
                {
                    fprintf(stderr,
                            "GOOD [%d] Client request to close connection\n",
                            fd);
                }
                operation = "QUIT";
            }
            else if (strcasecmp(command, "MAIL") == 0)
            {
                for (int i = 5; i < 9; i++)
                {
                    command[i - 5] = buffer[i];
                }
                if (strcasecmp(command, "FROM") == 0)
                {
                    do_mail(fd, status, buffer, sender, message); // mail from response
                    if (DEBUG)
                    {
                        fprintf(stderr, "GOOD [%d] Client sent mail from\n",
                                fd);
                    }
                    operation = "MAIL FROM";
                }
                else
                {
                    message = UNRECOGNIZED;
                    write(fd, UNRECOGNIZED, strlen(UNRECOGNIZED)); // unknown command
                    if (DEBUG)
                    {
                        fprintf(stderr,
                                "BAD [%d] Client sent unknown command\n", fd);
                    }
                    operation = "MAIL ";
                    operation += command;
                }
            }
            else if (strcasecmp(command, "RCPT") == 0)
            {
                for (int i = 5; i < 7; i++)
                {
                    command[i - 5] = buffer[i];
                }
                command[2] = '\0';
                if (strcasecmp(command, "TO") == 0)
                {
                    do_rcpt(fd, status, buffer, rcpts, message); // rcpt to response
                    if (DEBUG)
                    {
                        fprintf(stderr, "GOOD [%d] Client sent rcpt to\n", fd);
                    }
                    operation = "RCPT TO";
                }
                else
                {
                    message = UNRECOGNIZED;
                    write(fd, UNRECOGNIZED, strlen(UNRECOGNIZED)); // unknown command
                    if (DEBUG)
                    {
                        fprintf(stderr,
                                "BAD [%d] Client sent unknown command\n", fd);
                    }
                    operation = "RCPT ";
                    operation += command;
                }
            }
            else if (strcasecmp(command, "DATA") == 0 || data)
            {
                do_data(fd, status, buffer, sender, rcpts, content, tail, data,
                        message); // data response
                if (DEBUG)
                {
                    fprintf(stderr, "GOOD [%d] Client sent data\n", fd);
                }
                operation = "DATA";
            }
            else if (strcasecmp(command, "RSET") == 0)
            {
                do_rset(fd, status, sender, rcpts, content, message); // rset response
                if (DEBUG)
                {
                    fprintf(stderr, "GOOD [%d] Client sent rset\n", fd);
                }
                operation = "RSET";
            }
            else if (strcasecmp(command, "NOOP") == 0)
            {
                if (status == 0)
                {
                    message = SEQ_ERR;
                    write(fd, SEQ_ERR, strlen(SEQ_ERR));
                }
                else
                {
                    message = OK;
                    write(fd, OK, strlen(OK)); // noop response
                }
                if (DEBUG)
                {
                    fprintf(stderr, "GOOD [%d] Client sent noop\n", fd);
                }
                operation = "NOOP";
            }
            else
            {
                message = UNRECOGNIZED;
                write(fd, UNRECOGNIZED, strlen(UNRECOGNIZED)); // unknown command
                if (DEBUG)
                {
                    fprintf(stderr, "BAD [%d] Client sent unknown command\n",
                            fd);
                }
                operation = command;
            }

            if (DEBUG)
            {
                fprintf(stderr, "[%d] C: %s\n", fd, operation.c_str());
                fprintf(stderr, "[%d] S: %s", fd, message.c_str());
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
            if (i >= 1024 * 8)   // buffer is full
            {
                if (DEBUG)
                {
                    fprintf(stderr, "Out of buffer bound.\n");
                }
                write(fd, OVER_SIZE, strlen(OVER_SIZE));
                write(fd, buffer, 1024 * 8);
                memset(buffer, 0, 1024 * 8);
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
    unsigned int port_N = 2500;
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
                fprintf(stderr, "Invalid port number: %s\n", optarg);
                exit(1);
            }
            break;
        case '?':
            fprintf(stderr, "Error: Invalid choose: %c\n", (char) optopt);
            exit(1);
        default:
            fprintf(stderr,
                    "Error: Please input [-p port_num] [-a] [-v] [mailbox directory]\n");
            exit(1);
        }
    }
    if (optind == argc)
    {
        fprintf(stderr, "Error: Please input [mailbox directory]\n");
        exit(1);
    }
    user_dir = argv[optind];
    get_mboxes();

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
    pthread_mutex_init(&lock, NULL);

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
