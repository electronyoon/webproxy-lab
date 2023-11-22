#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "csapp.h"

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

typedef struct {
    int fd;
    char *host;
    char *port;
    int threadIdent;
    struct sockaddr_in clientAddress;
    int connport;
    char *hap;
} thread_args;

void *handle_thread(void *vargp);
void handle_client(int connfd, char *host, int port, struct sockaddr_in *clientAddress, int threadIdent);
int parse_uri(char *uri, char *hostname, char *pathname, int *port);
int get_from_cache(char *url, int clientfd);
void get_from_server(char *host, char *port, char *url, char buf_to_server[MAXLINE], int clientfd, rio_t rio_to_client);
// static struct CacheList *cache = (CacheList *) Calloc(1, sizeof(CacheList));
static sem_t mutex;

int main(int argc, char **argv) {
    int clientfd, clientlen, port, id = 0;
    struct sockaddr_in clientAddress;
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    // argv[1] = "15213";

    Signal(SIGPIPE, SIG_IGN);

    // cache_init(cache);

    if ((clientfd = Open_listenfd(argv[1])) < 0) {
        fprintf(stderr, "Error: %s\n", strerror(errno));
        exit(1);
    }

    while (1) {
        thread_args *thread;
        thread = Malloc(sizeof(thread_args));
        clientlen = sizeof(clientAddress);
        thread->fd = Accept(clientfd, (SA *)&clientAddress, (socklen_t *)&clientlen);
        thread->connport = port;
        thread->clientAddress = clientAddress;

        char *host[MAXLINE], *port[MAXLINE];
        Getnameinfo((SA *)&clientAddress, clientlen, host, MAXLINE, port, MAXLINE, 0);
        thread->host = host;
        thread->port = port;

        sem_t mutex;
        sem_init(&mutex, 0, 1);
        P(&mutex);
        thread->threadIdent = id++;
        V(&mutex);
        thread->hap = inet_ntoa(clientAddress.sin_addr);

        Pthread_create(&tid, NULL, handle_thread, thread);
    }
    printf("Shutting down...\n");
    Close(clientfd);
    // cache_destruct(cache);
    return EXIT_SUCCESS;
}

void *handle_thread(void *vargp) {
    int connfd, port, threadIdent;
    char *hap, *host;
    struct sockaddr_in clientAddress;
    thread_args *thread = (struct thread_args *)vargp;

    clientAddress = thread->clientAddress;
    connfd = thread->fd;
    port = thread->connport;
    hap = thread->hap;
    host = thread->host;
    threadIdent = thread->threadIdent;

    Free(vargp);
    pthread_detach(pthread_self());
    printf("Thread %d received request from %s\n", threadIdent, hap);

    handle_client(connfd, host, port, &clientAddress, threadIdent);
    Close(connfd);
    return NULL;
}

void handle_client(int connfd, char *host, int port, struct sockaddr_in *clientAddress, int threadIdent) {
    char method[MAXLINE], uri[MAXLINE], version[MAXLINE], path[MAXLINE];

    rio_t rio_to_client;
    char buf_to_client[MAXLINE];
    Rio_readinitb(&rio_to_client, connfd);
    Rio_readlineb(&rio_to_client, buf_to_client, MAXLINE);
    sscanf(buf_to_client, "%s %s %s", method, uri, version);
    printf("Method: %s\n", method);
    printf("URI: %s\n", uri);
    if (strcasecmp(method, "GET")) {
        printf("Invalid Request. Can only handle GET.\n");
        return;
    }
    
    parse_uri(uri, host, path, &port);
    printf("Host: %s\n", host);
    printf("Path: %s\n", path);
    printf("Port: %d\n", port);

    char buf_to_server[MAXLINE];
    strcpy(buf_to_server, "GET /");
    strcat(buf_to_server, path);
    strcat(buf_to_server, " HTTP/1.0\r\n");
    strcat(buf_to_server, "Host: ");
    strcat(buf_to_server, host);
    strcat(buf_to_server, "\r\n\r\n");

    // if (!get_from_cache(req, connfd)) {
    //     get_from_server(req, new_req_buf, connfd, rio_to_client);
    // }
    char char_port[MAXLINE];
    sprintf(char_port, "%d", port);
    get_from_server(host, char_port, uri, buf_to_server, connfd, rio_to_client);

    return NULL;
}

int parse_uri(char *uri, char *hostname, char *pathname, int *port) {
    char *hostbegin;
    char *hostend;
    char *pathbegin;
    int len;

    if (strncasecmp(uri, "http://", 7) != 0) {
        hostname[0] = '\0';
    }
    hostbegin = uri + 7;
    hostend = strpbrk(hostbegin, " :/\r\n\0");
    len = hostend - hostbegin;
    strncpy(hostname, hostbegin, len);
    hostname[len] = '\0';

    *port = 8000;
    if (*hostend == ':')
        *port = atoi(hostend + 1);
    pathbegin = strchr(hostbegin, '/');
    if (pathbegin == NULL) {
        pathname[0] = '\0';
    } else {
        pathbegin++;
        strcpy(pathname, pathbegin);
    }
    if (strlen(pathname) == 0)
        strcat(pathname, "/");

    return 0;
}

void get_from_server(char *host, char *port, char *url, char buf_to_server[MAXLINE], int clientfd, rio_t rio_to_client) {
    int serverfd = Open_clientfd(host, port);
    rio_t rio_to_server;
    Rio_readinitb(&rio_to_server, serverfd);
    Rio_writen(serverfd, buf_to_server, strlen(buf_to_server));

    char *buf = Malloc(MAXLINE);
    char *p, *temp = Calloc(1, MAX_CACHE_SIZE);
    int n, size = 0;
    int can_cache = 1;
    while ((n = Rio_readnb(&rio_to_server, buf, MAXLINE)) != 0) {
        printf("proxy received %d bytes, then send\n", n);
        printf("buf: %s\n", buf);
        Rio_writen(clientfd, buf, n);
        if (size + n <= MAX_OBJECT_SIZE) {
            memcpy(temp + size, buf, n);
            size += n;
        } else {
            can_cache = 0;
        }
    }
    // if (can_cache) {
    //     cache_url(url, temp, size, cache);
    // }
    Close(serverfd);
    Free(temp);
    Free(p);
    Free(buf);
}

int get_from_cache(char *url, int clientfd) {
    // struct CachedItem *node = find(url, cache);
    // if (node) {
    //     move_to_front(url, cache);
    //     Rio_writen(clientfd, node->item, node->size);
    //     return 1;
    // }
    return 0;
}