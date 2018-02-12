#include <openssl/md5.h>
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
const char *PASSW = "cis505";
const char *READY = "+OK POP3 server ready [localhost]\r\n";
const char *SEQ_ERR = "-ERR Bad sequence of commands\r\n";
const char *OK = "+OK\r\n";
const char *SYN_ERR = "-ERR Syntax error in parameters or arguments\r\n";
const char *NO_MESS = "-ERR no such message\r\n";
const char *UNSUPPORTED = "-ERR Not supported\r\n";
const char *SERV_UNAVAIL =
    "-ERR [localhost] Service not available, closing transmission channel\r\n";
const char *OVER_SIZE = "-ERR Too much mail data\r\n";

vector<pthread_t> THREADS;
pthread_mutex_t lock;
vector<unsigned int> SOCKETS;
unordered_set<string> MBOXES;
unsigned int listen_fd;
string user_dir;
bool DEBUG;
bool RUNNING;

/* A class for the message that can check, set and reset deleted status and store message content.*/
class Message
{
private:
    string content;
    bool deleted;
public:
    Message(string content)
    {
        this->content = content;
        deleted = false;
    }
    string get_content();
    bool is_deleted();
    void set_delete();
    void rset_delete();
};

string Message::get_content()
{
    return content;
}

bool Message::is_deleted()
{
    return deleted;
}

void Message::set_delete()
{
    deleted = true;
}

void Message::rset_delete()
{
    deleted = false;
}

void computeDigest(char *data, int dataLengthBytes,
                   unsigned char *digestBuffer)
{
    /* The digest will be written to digestBuffer, which must be at least MD5_DIGEST_LENGTH bytes long */

    MD5_CTX c;
    MD5_Init(&c);
    MD5_Update(&c, data, dataLengthBytes);
    MD5_Final(digestBuffer, &c);
}

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

/* Helper function used to parse command content */
void parse(char *buffer, char *dest, int limit)
{
    int i = 0, j = 0, len = strlen(buffer);
    while (buffer[i] != ' ' && i < len)
    {
        i++;
    }
    i++;
    while (buffer[i] != '\r' && i < len && j < limit)
    {
        dest[j] = buffer[i];
        j++;
        i++;
    }
}

/* USER command handler that checks the state, user and send response. If user exists then sets user. */
void do_user(unsigned int fd, int &status, char *buffer, char *user,
             string &message)
{
    if (status != 0 || strlen(user) != 0)
    {
        message = SEQ_ERR;
        write(fd, SEQ_ERR, strlen(SEQ_ERR));
    }
    else
    {
        char one_user[65] = { };
        parse(buffer, one_user, 64);
        string mbox = (string) one_user + ".mbox";
        if (MBOXES.find(mbox) != MBOXES.end())
        {
            strcpy(user, one_user);
            message = "+OK " + string(one_user) + " is a valid mailbox\r\n";
        }
        else
        {
            message = "-ERR Sorry, never heard of mailbox for "
                      + string(one_user) + " here\r\n";
        }
        const char *res = message.c_str();
        write(fd, res, strlen(res));
    }
}

/* PASS command handler that checks the state and password. If all correct then reads mail. */
void do_pass(unsigned int fd, int &status, char *buffer, char *user,
             vector<string> &titles, vector<Message> &messages, string &message)
{
    if (status != 0 || strlen(user) == 0)
    {
        message = SEQ_ERR;
        write(fd, SEQ_ERR, strlen(SEQ_ERR));
    }
    else
    {
        char password[65] = { };
        parse(buffer, password, 64);
        if (strcmp(password, PASSW) == 0)
        {
            status = 1;
            ifstream mail; // read from user's mail file
            string address = user_dir + "/" + string(user) + ".mbox";
            string content = "", line, title = "From <";
            pthread_mutex_lock(&lock);
            mail.open(address);
            while (getline(mail, line))
            {
                if (line.compare(0, title.size(), title) != 0)
                {
                    content += line;
                    content.pop_back();
                    content += "\r\n";
                }
                else
                {
                    string one_title = line;
                    one_title.pop_back();
                    one_title += "\r\n";
                    titles.push_back(one_title);
                    Message m(content); // one new message
                    messages.push_back(m);
                    content.clear();
                }
            }
            mail.close();
            pthread_mutex_unlock(&lock);
            if (messages.size() > 0)
            {
                messages.erase(messages.begin());
                messages.push_back((Message(content))); // remove the first empty line and insert the last message
            }
            message = "+OK " + string(user) + "'s maildrop has "
                      + to_string(messages.size()) + " messages\r\n";
        }
        else
        {
            message = "-ERR invalid password\r\n";
        }
        const char *res = message.c_str();
        write(fd, res, strlen(res));
    }
}

/* STAT command handler that check the state, updates and displays the number and size of the mailbox. */
void do_stat(unsigned int fd, int &status, char *user, vector<string> &titles, vector<Message> &messages,
             string &message)
{
    if (status != 1)
    {
        message = SEQ_ERR;
        write(fd, SEQ_ERR, strlen(SEQ_ERR));
    }
    else
    {
        ifstream mail; // read from user's mail file
        string address = user_dir + "/" + string(user) + ".mbox";
        string content = "", line, title = "From <";
        messages.clear();
        pthread_mutex_lock(&lock);
        mail.open(address);
        while (getline(mail, line))
        {
            if (line.compare(0, title.size(), title) != 0)
            {
                content += line;
                content.pop_back();
                content += "\r\n";
            }
            else
            {
                string one_title = line;
                one_title.pop_back();
                one_title += "\r\n";
                titles.push_back(one_title);
                Message m(content); // one new message
                messages.push_back(m);
                content.clear();
            }
        }
        mail.close();
        pthread_mutex_unlock(&lock);
        if (messages.size() > 0)
        {
            messages.erase(messages.begin());
            messages.push_back((Message(content))); // remove the first empty line and insert the last message
        }
        int count = 0, size = 0;
        for (int i = 0; i < messages.size(); i++)
        {
            if (!messages[i].is_deleted())
            {
                count++;
                size += messages[i].get_content().length();
            }
        }
        message = "+OK " + to_string(count) + " " + to_string(size) + "\r\n";
        const char *res = message.c_str();
        write(fd, res, strlen(res));
    }
}

/* UIDL command handler that checks the state and shows a list of messages with unique IDs. */
void do_uidl(unsigned int fd, int &status, char *buffer,
             vector<Message> &messages, string &message)
{
    if (status != 1)
    {
        message = SEQ_ERR;
        write(fd, SEQ_ERR, strlen(SEQ_ERR));
    }
    else
    {
        char comm[5] = { };
        parse(buffer, comm, 4);
        if (strlen(comm) == 0)
        {
            write(fd, OK, strlen(OK));
            for (int i = 0; i < messages.size(); i++)
            {
                if (!messages[i].is_deleted())
                {
                    unsigned char digest[MD5_DIGEST_LENGTH] = { };
                    char uid[MD5_DIGEST_LENGTH * 2 + 1] = { };
                    char input[messages[i].get_content().length() + 1] = { };
                    strcpy(input, messages[i].get_content().c_str());
                    computeDigest(input, strlen(input), digest);
                    for (int j = 0; j < MD5_DIGEST_LENGTH; j++)
                    {
                        sprintf(uid + j * 2, "%02x", digest[j]);
                    }
                    string one_id = to_string(i + 1) + " " + string(uid)
                                    + "\r\n";
                    const char *res_id = one_id.c_str();
                    write(fd, res_id, strlen(res_id));
                }
            }
            message = "+OK UIDL all\r\n";
            string end = ".\r\n";
            const char *res = end.c_str();
            write(fd, res, strlen(res));
        }
        else
        {
            int idx = atoi(comm);
            if (idx < 1)
            {
                message = SYN_ERR;
                write(fd, SYN_ERR, strlen(SYN_ERR));
            }
            else if (idx > messages.size()
                     || messages[idx - 1].is_deleted())
            {
                message = NO_MESS;
                write(fd, NO_MESS, strlen(NO_MESS));
            }
            else
            {
                unsigned char digest[MD5_DIGEST_LENGTH] = { };
                char uid[MD5_DIGEST_LENGTH * 2 + 1] = { };
                char input[messages[idx - 1].get_content().length() + 1] = { };
                strcpy(input, messages[idx - 1].get_content().c_str());
                computeDigest(input, strlen(input), digest);
                for (int j = 0; j < MD5_DIGEST_LENGTH; j++)
                {
                    sprintf(uid + j * 2, "%02x", digest[j]);
                }
                message = "+OK " + to_string(idx) + " " + string(uid) + "\r\n";
                const char *res = message.c_str();
                write(fd, res, strlen(res));
            }
        }
    }
}

/* LIST command handler that checks the state and shows the size of a message or all messages. */
void do_list(unsigned int fd, int &status, char *buffer,
             vector<Message> &messages, string &message)
{
    if (status != 1)
    {
        message = SEQ_ERR;
        write(fd, SEQ_ERR, strlen(SEQ_ERR));
    }
    else
    {
        char comm[5] = { };
        parse(buffer, comm, 4);
        if (strlen(comm) == 0)
        {
            int count = 0, size = 0;
            vector<int> one_sizes;
            for (int i = 0; i < messages.size(); i++)
            {
                if (!messages[i].is_deleted())
                {
                    count++;
                    size += messages[i].get_content().length();
                    one_sizes.push_back(messages[i].get_content().length());
                }
            }
            string total = "+OK " + to_string(count) + " messages ("
                           + to_string(size) + " octets)\r\n";
            const char *res = total.c_str();
            write(fd, res, strlen(res));
            for (int i = 0; i < one_sizes.size(); i++)
            {
                string one = to_string(i + 1) + " " + to_string(one_sizes[i])
                             + "\r\n";
                res = one.c_str();
                write(fd, res, strlen(res));
            }
            message = "+OK LIST all\r\n";
            string end = ".\r\n";
            res = end.c_str();
            write(fd, res, strlen(res));
        }
        else
        {
            int idx = atoi(comm);
            if (idx < 1)
            {
                message = SYN_ERR;
                write(fd, SYN_ERR, strlen(SYN_ERR));
            }
            else if (idx > messages.size()
                     || messages[idx - 1].is_deleted())
            {
                message = NO_MESS;
                write(fd, NO_MESS, strlen(NO_MESS));
            }
            else
            {
                message = "+OK " + to_string(idx) + " "
                          + to_string(messages[idx - 1].get_content().length())
                          + "\r\n";
                const char *res = message.c_str();
                write(fd, res, strlen(res));
            }
        }
    }
}

/* RETR command handler that checks the state and retrieves a particular message. */
void do_retr(unsigned int fd, int &status, char *buffer,
             vector<Message> &messages, string &message)
{
    if (status != 1)
    {
        message = SEQ_ERR;
        write(fd, SEQ_ERR, strlen(SEQ_ERR));
    }
    else
    {
        char comm[5] = { };
        parse(buffer, comm, 4);
        if (strlen(comm) == 0)
        {
            message = SYN_ERR;
            write(fd, SYN_ERR, strlen(SYN_ERR));
        }
        else
        {
            int idx = atoi(comm);
            if (idx < 1)
            {
                message = SYN_ERR;
                write(fd, SYN_ERR, strlen(SYN_ERR));
            }
            else if (idx > messages.size()
                     || messages[idx - 1].is_deleted())
            {
                message = NO_MESS;
                write(fd, NO_MESS, strlen(NO_MESS));
            }
            else
            {
                message = "+OK "
                          + to_string(messages[idx - 1].get_content().length())
                          + " octets\r\n";
                const char *res = message.c_str();
                write(fd, res, strlen(res));
                string retrieve = messages[idx - 1].get_content();
                int i = 0;
                while (i < retrieve.length())
                {
                    int j = retrieve.find('\n', i);
                    string line = retrieve.substr(i, j + 1 - i);
                    const char *out = line.c_str();
                    write(fd, out, strlen(out));
                    i = j + 1;
                }
                string end = ".\r\n";
                res = end.c_str();
                write(fd, res, strlen(res));
            }
        }
    }
}

/* DELE command handler that checks the state and deletes a particular message. */
void do_dele(unsigned int fd, int &status, char *buffer,
             vector<Message> &messages, string &message)
{
    if (status != 1)
    {
        message = SEQ_ERR;
        write(fd, SEQ_ERR, strlen(SEQ_ERR));
    }
    else
    {
        char comm[5] = { };
        parse(buffer, comm, 4);
        if (strlen(comm) == 0)
        {
            message = SYN_ERR;
            write(fd, SYN_ERR, strlen(SYN_ERR));
        }
        else
        {
            int idx = atoi(comm);
            if (idx < 1)
            {
                message = SYN_ERR;
                write(fd, SYN_ERR, strlen(SYN_ERR));
            }
            else if (idx > messages.size())
            {
                message = NO_MESS;
                write(fd, NO_MESS, strlen(NO_MESS));
            }
            else if (messages[idx - 1].is_deleted())
            {
                message = "-ERR message " + to_string(idx)
                          + " already deleted\r\n";
                const char *res = message.c_str();
                write(fd, res, strlen(res));
            }
            else
            {
                messages[idx - 1].set_delete();
                message = "+OK message " + to_string(idx) + " deleted\r\n";
                const char *res = message.c_str();
                write(fd, res, strlen(res));
            }
        }
    }
}

/* RSET command handler that checks the state and unmark all deleted messages. */
void do_rset(unsigned int fd, int &status, vector<Message> &messages,
             string &message)
{
    if (status != 1)
    {
        message = SEQ_ERR;
        write(fd, SEQ_ERR, strlen(SEQ_ERR));
    }
    else
    {
        for (int i = 0; i < messages.size(); i++)
        {
            if (messages[i].is_deleted())
            {
                messages[i].rset_delete();
            }
        }
        message = OK;
        write(fd, OK, strlen(OK));
    }
}

/* QUIT command handler that checks the state, removes all deleted messages and terminates the connection. */
void do_quit(unsigned int fd, int &status, char *user, vector<string> &titles,
             vector<Message> &messages, string &message)
{
    if (status == 0)
    {
        message = "+OK " + string(user) + " POP3 server signing off\r\n";
    }
    else
    {
        int count = 0, pre = 0; // pre is counter for processed messages
        ifstream mail_in;
        ofstream mail_out;
        string address = user_dir + "/" + string(user) + ".mbox";
        string new_message = "", line, title = "From <"; // remember new unprocesed messages
        pthread_mutex_lock(&lock);
        mail_in.open(address);
        while (getline(mail_in, line) && pre <= messages.size())
        {
            if (line.compare(0, title.size(), title) == 0)
            {
                pre++; // ignore processed messages
            }
        }
        if(pre > messages.size())
        {
            new_message += line;
            new_message.pop_back();
            new_message += "\r\n";
            while (getline(mail_in, line))
            {
                new_message += line;
                new_message.pop_back();
                new_message += "\r\n";
            }
        }
        mail_in.close();
        mail_out.open(address);
        for (int i = 0; i < messages.size(); i++)
        {
            if (!messages[i].is_deleted())
            {
                mail_out << titles[i]; // update content to mail file
                mail_out << messages[i].get_content();
                count++;
            }
        }
        mail_out << new_message;
        mail_out.close();
        pthread_mutex_unlock(&lock);
        message = "+OK " + string(user) + " POP3 server signing off (";
        if (count == 0)
        {
            message += "maildrop empty)\r\n";
        }
        else
        {
            message += to_string(count) + " messages left)\r\n";
        }
        status = 2;
    }
    messages.clear();
    titles.clear();
    const char *res = message.c_str();
    write(fd, res, strlen(res));
}

/* Thread function for handling a client */
void *client_t(void *p)
{
    unsigned int fd = *(int *) p;
    write(fd, READY, strlen(READY)); // greeting message

    bool disconnect = false;
    char user[65] = { };
    char buffer[1024 * 8 + 1] = { };
    char *head = buffer;
    vector<Message> messages;
    vector<string> titles;

    int status = 0; // status for a client: 0 authorization, 1 transaction, 2 update

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
            string message;
            if (strcasecmp(command, "USER") == 0)
            {
                do_user(fd, status, buffer, user, message); // user response
                if (DEBUG)
                {
                    fprintf(stderr, "GOOD [%d] Client sent user\n", fd);
                }
            }
            else if (strcasecmp(command, "QUIT") == 0)
            {
                if (status == 0 || status == 1)
                {
                    do_quit(fd, status, user, titles, messages, message); // quit response
                    disconnect = true;
                    if (DEBUG)
                    {
                        fprintf(stderr,
                                "GOOD [%d] Client request to close connection\n",
                                fd);
                    }
                }
                else
                {
                    message = SEQ_ERR;
                    write(fd, SEQ_ERR, strlen(SEQ_ERR));
                    if (DEBUG)
                    {
                        fprintf(stderr, "BAD [%d] Sequence error!\n", fd);
                    }
                }
            }
            else if (strcasecmp(command, "PASS") == 0)
            {
                do_pass(fd, status, buffer, user, titles, messages, message); // pass response
                if (DEBUG)
                {
                    fprintf(stderr, "GOOD [%d] Client sent pass\n", fd);
                }
            }
            else if (strcasecmp(command, "STAT") == 0)
            {
                do_stat(fd, status, user, titles, messages, message); // stat response
                if (DEBUG)
                {
                    fprintf(stderr, "GOOD [%d] Client sent stat\n", fd);
                }
            }
            else if (strcasecmp(command, "UIDL") == 0)
            {
                do_uidl(fd, status, buffer, messages, message); // uidl response
                if (DEBUG)
                {
                    fprintf(stderr, "GOOD [%d] Client sent uidl\n", fd);
                }
            }
            else if (strcasecmp(command, "RETR") == 0)
            {
                do_retr(fd, status, buffer, messages, message); // retr response
                if (DEBUG)
                {
                    fprintf(stderr, "GOOD [%d] Client sent retr\n", fd);
                }
            }
            else if (strcasecmp(command, "DELE") == 0)
            {
                do_dele(fd, status, buffer, messages, message); // dele response
                if (DEBUG)
                {
                    fprintf(stderr, "GOOD [%d] Client sent dele\n", fd);
                }
            }
            else if (strcasecmp(command, "LIST") == 0)
            {
                do_list(fd, status, buffer, messages, message); // list response
                if (DEBUG)
                {
                    fprintf(stderr, "GOOD [%d] Client sent list\n", fd);
                }
            }
            else if (strcasecmp(command, "RSET") == 0)
            {
                do_rset(fd, status, messages, message); // rset response
                if (DEBUG)
                {
                    fprintf(stderr, "GOOD [%d] Client sent rset\n", fd);
                }
            }
            else if (strcasecmp(command, "NOOP") == 0)
            {
                if (status != 1)
                {
                    message = SEQ_ERR;
                    write(fd, SEQ_ERR, strlen(SEQ_ERR));
                    if (DEBUG)
                    {
                        fprintf(stderr, "BAD [%d] Sequence error!\n", fd);
                    }
                }
                else
                {
                    message = OK;
                    write(fd, OK, strlen(OK)); // noop response
                    if (DEBUG)
                    {
                        fprintf(stderr, "GOOD [%d] Client sent noop\n", fd);
                    }
                }
            }
            else
            {
                message = UNSUPPORTED;
                write(fd, UNSUPPORTED, strlen(UNSUPPORTED)); // unknown command
                if (DEBUG)
                {
                    fprintf(stderr,
                            "BAD [%d] Client sent unknown or unsupported command\n",
                            fd);
                }
            }

            if (DEBUG)
            {
                fprintf(stderr, "[%d] C: %s\n", fd, command);
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
    unsigned int port_N = 11000;
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
