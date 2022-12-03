
//    include & define                                // 

#include <stdlib.h>
#include <string.h>

#include <stdio.h>
#include "csapp.h"


#define DEFPORT 8080    // Default port

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

#define log(func) fprintf(stderr, #func" error: %s\n%s%s:%d%s\n", \
        strerror(errno), \
        status.prot, \
        status.hostname, \
        status.port, \
        status.path)

//   Global variables & Structures  // 

extern sem_t sem_log;   // Semaphore variables
extern sem_t sem_dns;   // Semaphore variables
FILE* log_file;         // File pointer for log file

//  When method is POST //
struct status_line {
    char line[MAXLINE];
    char method[20];
    char prot[20];
    char hostname[MAXLINE];
    int  port;
    char path[MAXLINE];
    char version[20];
};

//  When method is GET  //
struct cache_block {
	long timeid;
	char *content;
	int content_length;
	char *line;
	struct cache_block *next, *prev;
};
static int cache_size;
static long timecount;
static sem_t sem;
static struct cache_block *head;
void cache_init();
int cache_find(char *line, char *buf);
void cache_add(char *line, char *objectbuf, int objectlen);

void format_log_entry(char* logstring, struct sockaddr_in* sockaddr, char* uri, int size);

int parseline(char* line, struct status_line* status);
int send_request(rio_t* rio, char* buf,
    struct status_line* status, int serverfd, int clientfd);
int transmit(int readfd, int writefd, char* buf, int* count
    , char* objectbuf, int* objectlen
);
int interrelate(int serverfd, int clientfd, char* buf, int idling
    , char* objectbuf, struct status_line* status
);
void* proxy(void* vargp);

struct thread_args
{
    int fd;
    struct sockaddr_in sock;
};

sem_t sem_log;
sem_t sem_dns;

int global_len = 0;



//   Main function starts   // 

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    signal(SIGPIPE, SIG_IGN);
//  Designate port number   //
    int port = atoi(argv[1]);
//  Semaphore Initialize    //
    sem_init(&sem_log, 0, 1);
    sem_init(&sem_dns, 0, 1);
// Log file opened  //
    log_file = fopen("./proxy.log", "a");

//  Ready to take in FD (of PORT)   //
    int listenfd = Open_listenfd(port);    // FD for listen below

    struct thread_args args;

    int connfd; // FD for acceptance
    struct sockaddr_in clientaddr;  // Socket Information in this
    socklen_t addrlen = sizeof clientaddr;

//  If the method is GET we use cache   //
    cache_init();

    while ("serve forever") {   // Continuously receive, equivalent to while(1)


        printf("listening..\n");
// Ready to create a new thread //
        struct thread_args* argsp = (struct thread_args*)malloc(sizeof(struct thread_args));
        do
        {
            connfd = Accept(listenfd, (SA*)(&(argsp->sock)), &addrlen);
            argsp->fd = connfd; // Connect and allocate the created fd to new thread
        } while (connfd < 0);

        pthread_t tid;  //thread id 
        Pthread_create(&tid, NULL, proxy, (void*)argsp);
// A new thread is created and runs proxy()   //
    }
}


//   If the method is GET, its cacheable    // 
int cacheable(struct status_line *status) {
	return !strcmp(status->method, "GET");
}


//   parsor for line from client request    //

int parseline(char* line, struct status_line* status) {
    status->port = 80;
    strcpy(status->line, line);
// line example : "POST http://example.com/example HTTP/1.1"  //
    if (sscanf(line, "%s %[a-z]://%[^/]%s %s",
        status->method,
        status->prot,
        status->hostname,
        status->path,
        status->version) != 5) {
        if (sscanf(line, "%s %s %s",
            status->method,
            status->hostname,
            status->version) != 3)
            return -1;
        *status->prot = *status->path = 0;
    }
    else
        strcat(status->prot, "://");

    char* pos = strchr(status->hostname, ':');  // If there is port number in hostname
    if (pos) {
        *pos = 0;
        status->port = atoi(pos + 1);   // Which port to use
    }
    return 0;
}


//   Send request to server by buffer from client     // 

int send_request(rio_t* rio, char* buf,
    struct status_line* status, int serverfd, int clientfd) {
    int len;
    if (strcmp(status->method, "CONNECT")) {
        len = snprintf(buf, MAXLINE, "%s %s %s\r\n" \
            "Connection: close\r\n",
            status->method,
            *status->path ? status->path : "/",
            status->version);   // "Connection: close <method> <path> <version>" goes into buf
                                // If there is no path,  '/' goes instead.
        if ((len = rio_writen(serverfd, buf, len)) < 0)
            return len;
        while (len != 2) {
            if ((len = rio_readlineb(rio, buf, MAXLINE)) < 0) // Send rio's content into buf
                return len;
            if (memcmp(buf, "Proxy-Connection: ", 18) == 0 ||
                memcmp(buf, "Connection: ", 12) == 0)
                continue;
            if ((len = rio_writen(serverfd, buf, len)) < 0) // Send buf's content into serverfd
                return len;
        }
        if (rio->rio_cnt &&
            (len = rio_writen(serverfd,
                rio->rio_bufptr, rio->rio_cnt)) < 0)
            return len;
        return 20;
    }
    else {
        len = snprintf(buf, MAXLINE,
            "%s 200 OK\r\n\r\n", status->version);
        if ((len = rio_writen(clientfd, buf, len)) < 0)
            return len;
        return 300;
    }
}


// transmit between two different socket descriptors     //
// global_len : the total bytes length of transmited data// 

int transmit(int readfd, int writefd, char* buf, int* count

    , char* objectbuf, int* objectlen

) {
    int len;
    if ((len = read(readfd, buf, MAXBUF)) > 0) {
        global_len += len;// global_len : the total bytes length of transmited data
        if (objectbuf && objectlen && *objectlen != -1) {
            if (*objectlen + len < MAX_OBJECT_SIZE) {
                memcpy(objectbuf + *objectlen, buf, len);
                *objectlen += len;
            }
            else
                *objectlen = -1;
        }

        *count = 0;
        len = rio_writen(writefd, buf, len);
    }
    return len;
}


//  Wrapper function for transmit function    // 

int interrelate(int serverfd, int clientfd, char* buf, int idling

    , char* objectbuf, struct status_line* status

) {
    int count = 0;
    int nfds = (serverfd > clientfd ? serverfd : clientfd) + 1;
    int flag;
//  fd_set, FD_() functions are used to deal with fd efficiently and to use select() //
//  rlist is for read, xlist is for exception.  //
    fd_set rlist, xlist;
    FD_ZERO(&rlist);
    FD_ZERO(&xlist);


    int objectlen = 0;


    while (1) {
        count++;
//  Initialize fd   //
        FD_SET(clientfd, &rlist);
        FD_SET(serverfd, &rlist);
        FD_SET(clientfd, &xlist);
        FD_SET(serverfd, &xlist);

        struct timeval timeout = { 2L, 0L };
//  select() for efficient multiplexing    //
        if ((flag = select(nfds, &rlist, NULL, &xlist, &timeout)) < 0)
            return flag;
        if (flag) {
            if (FD_ISSET(serverfd, &xlist) || FD_ISSET(clientfd, &xlist))
                break;
            if (FD_ISSET(serverfd, &rlist) &&
                ((flag = transmit(serverfd, clientfd,
                    buf, &count

                    , objectbuf, &objectlen

                )) < 0))
                return flag;
            if (flag == 0)
                break;
            if (FD_ISSET(clientfd, &rlist) &&
                ((flag = transmit(clientfd, serverfd,
                    buf, &count

                    , NULL, NULL

                )) < 0))
                return flag;
            if (flag == 0)
                break;
        }
        if (count >= idling)
            break;
    }
//  Use cache if its possible   //
    if (objectlen > 0 && cacheable(status))
		cache_add(status->line, objectbuf, objectlen);

    return 0;
}


//  Proxy function (main key function)                //
//  Wrapping send_request(), interrelate() functions  //

void* proxy(void* vargs) {
    Pthread_detach(Pthread_self());     // Thread is detached

    int serverfd;
//  Recognize the client by clientfd    //
    int clientfd = *(int*)vargs;
//  "argsp" from main function is put in "args" in proxy()  //
    struct thread_args* args = (struct thread_args*)vargs;
    int fd = args->fd;  // Copy accepted fd of argsp (= connfd) into fd
    struct sockaddr_in sock;
    memcpy(&sock, &(args->sock), sizeof(struct sockaddr_in));   // Copy socket address of argsp into sock
// Copied all data into local variables args is ready to be free    //
    free(args);

    rio_t rio;
    rio_readinitb(&rio, clientfd);  // Read from clientfd

    struct status_line status;  // Data read from clientfd will be put in status

    char buf[MAXLINE];
    int flag;


    char objectbuf[MAX_OBJECT_SIZE];

    char method_dummy[MAXLINE], uri[MAXLINE], version_dummy[MAXLINE];

    int contents_len = 0;


    if ((flag = rio_readlineb(&rio, buf, MAXLINE)) > 0) // Reading data and error handling
    {

        sscanf(buf, "%s %s %s", method_dummy, uri, version_dummy);  // Read and separate data from buf

        if (parseline(buf, &status) < 0)
            fprintf(stderr, "parseline error: '%s'\n", buf);    // Error handling when failed to parse
        
        else if (cacheable(&status) &&
				(flag = cache_find(status.line, objectbuf))) {
			if (rio_writen(clientfd, objectbuf, flag) < 0)
				log(cache_write);
		}

        else if ((serverfd =
            open_clientfd(status.hostname, status.port)) < 0)   // Try to connect to server by hostname and port
            log(open_clientfd); // Error handling when failed to get client fd
        else {

            if ((flag = send_request(&rio, buf,
                &status, serverfd, clientfd)) < 0)
                log(send_request);  // Error handling when failed to send request
            else if (interrelate(serverfd, clientfd, buf, flag

                , objectbuf, &status

            ) < 0)
                log(interrelate);   // Error handling when failed interrelate


            close(serverfd);
        }
    }


    printf("ready to write log\n");
    format_log_entry(buf, &sock, uri, global_len);//global_len : the total bytes length of transmited data
    fprintf(log_file, "%s\n", buf);//write buffer to log file
    fflush(log_file);

    global_len = 0;//reset to 0, when data-transmition is over



    close(clientfd);
    return NULL;
}

//    cache initialization function                   // 

void cache_init() {
	head = NULL;
	cache_size = 0;
	timecount = 0;
	Sem_init(&sem, 0, 1);
}

//    Semaphore start for cache // 

int cache_find(char *line, char *buf) {
	int len = 0;
	P(&sem);
	struct cache_block *ptr = head;
	for (; ptr; ptr = ptr->next)
		if (!strcmp(ptr->line, line)) {
			ptr->timeid = timecount++;
			memcpy(buf, ptr->content, ptr->content_length);
			len = ptr->content_length;
			break;
		}
	V(&sem);
	return len;
}


//    contents copying to cache when the method is GET, Semaphore used for exclusiveness  // 

void cache_add(char *line, char *objectbuf, int objectlen) {
	struct cache_block *ptr;
	P(&sem);
	while (cache_size + objectlen >= MAX_CACHE_SIZE) {
		struct cache_block *ev = head;
		for (ptr = head; ptr; ptr = ptr->next) {
			if (ptr->timeid < ev->timeid)
				ev = ptr;
		}
		cache_size -= ev->content_length;
		if (ev->prev)
			ev->prev->next = ev->next;
		if (ev->next)
			ev->next->prev = ev->prev;
		if (ev == head)
			head = ev->next;
		free(ev->content);
		free(ev->line);
		free(ev);
	}
	cache_size += objectlen;
	ptr = (struct cache_block *)Malloc(sizeof (struct cache_block));
	ptr->timeid = timecount++;
	ptr->content = (char *)Malloc(objectlen);
	ptr->content_length = objectlen;
	memcpy(ptr->content, objectbuf, objectlen);
	ptr->line = (char *)Malloc(strlen(line) + 1);
	strcpy(ptr->line, line);
	if (head)
		head->prev = ptr;
	ptr->next = head;
	ptr->prev = NULL;
	head = ptr;
	V(&sem);
}
/*
 * format_log_entry - Create a formatted log entry in logstring.
 *
 * The inputs are the socket address of the requesting client
 * (sockaddr), the URI from the request (uri), and the size in bytes
 * of the response from the server (size).
 */
void format_log_entry(char* logstring, struct sockaddr_in* sockaddr,
    char* uri, int size)
{
    time_t now;
    char time_str[MAXLINE];
    unsigned long host;
    unsigned char a, b, c, d;

    /* Get a formatted time string */
    now = time(NULL);
    strftime(time_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", localtime(&now));

    /*
     * Convert the IP address in network byte order to dotted decimal
     * form. Note that we could have used inet_ntoa, but chose not to
     * because inet_ntoa is a Class 3 thread unsafe function that
     * returns a pointer to a static variable (Ch 13, CS:APP).
     */
    host = ntohl(sockaddr->sin_addr.s_addr);
    a = host >> 24;
    b = (host >> 16) & 0xff;
    c = (host >> 8) & 0xff;
    d = host & 0xff;


    /* Return the formatted log entry string */
    sprintf(logstring, "%s: %d.%d.%d.%d %s %d", time_str, a, b, c, d, uri, size);
}


