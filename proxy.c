#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "csapp.h"

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
struct threadArgs {
    int fd;
    int threadIdent;
    struct sockaddr_in clientAddress;
    int connport;
    char *hap;
};

void doit(int connfd, int port, struct sockaddr_in *sockaddr, int threadIdent);
int parse_uri(char *uri, char *hostname, char *pathname, int *port);
void get_http_headers(char *http_header, char *host, char *path, int port, rio_t *client_rio);
int get_endserver(char *host, int port, char *http_header);
void *threadCode(void *vargp);

int main(int argc, char **argv) {
    int clientfd, clientlen, port, id = 0;
    struct sockaddr_in clientAddress;
    pthread_t tid;

    clientfd = Open_listenfd(argv[1]);
    Signal(SIGPIPE, SIG_IGN);
    while (1) {
        struct threadArgs *thread;
        thread = Malloc(sizeof(struct threadArgs));
        clientlen = sizeof(clientAddress);
        thread->fd = Accept(clientfd, (SA *)&clientAddress, (socklen_t *)&clientlen);
        thread->connport = port;
        thread->clientAddress = clientAddress;

        sem_t mutex;
        sem_init(&mutex, 0, 1);
        P(&mutex);
        thread->threadIdent = id++;
        V(&mutex);
        thread->hap = inet_ntoa(clientAddress.sin_addr);

        Pthread_create(&tid, NULL, threadCode, thread);
    }
    printf("Shutting down...\n");
    Close(clientfd);
    return EXIT_SUCCESS;
}

void *threadCode(void *vargp) {
    int connfd, port, threadIdent;
    char *hap;
    struct sockaddr_in clientAddress;
    struct threadArgs *thread = ((struct threadArgs *)vargp);

    clientAddress = thread->clientAddress;
    connfd = thread->fd;
    port = thread->connport;
    hap = thread->hap;
    threadIdent = thread->threadIdent;

    Free(vargp);
    pthread_detach(pthread_self());
    printf("Thread %d received request from %s\n", threadIdent, hap);

    doit(connfd, port, &clientAddress, threadIdent);
    Close(connfd);
    return NULL;
}

void doit(int connfd, int port, struct sockaddr_in *sockaddr, int threadIdent) {
    int serverfd;
    int end_serverfd;
    char server_http_header[MAXLINE];
    rio_t rio, riohost;

    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char host[MAXLINE], path[MAXLINE];

    Rio_readinitb(&rio, connfd);
    Rio_readlineb(&rio, buf, MAXLINE);
    sscanf(buf, "%s %s %s", method, uri, version);
    printf("Method: %s\n", method);
    printf("URI: %s\n", uri);
    if (strcasecmp(method, "GET")) {
        printf("Invalid Request. Can only handle GET.\n");
        return;
    }
    
    // declare new port and copy port into new port
    int newport = port;
    parse_uri(uri, host, path, &newport);
    printf("Host: %s\n", host);
    printf("Path: %s\n", path);
    int hostfd = 0;
    char newportstr[6];
    sprintf(newportstr, "%d", newport);
    if ((hostfd = Open_clientfd(host, newportstr)) < 0) {
        return;
    }

    sprintf(buf, "%s /%s %s\r\n", method, path, "HTTP/1.0");
    Rio_readinitb(&riohost, hostfd);
    Rio_writen(hostfd, buf, strlen(buf));
    size_t n;
    int client = 0;
    while (!client && ((n = Rio_readlineb(&rio, buf, MAXLINE)) != 0)) {
        printf("Thread %d: Forwarding request to host\n", threadIdent);
        printf("buf(%zu bytes): %s", n, buf);
        Rio_writen(hostfd, buf, n);
        client = (buf[0] == '\r');
    }
    printf("*** End of Request From Client ***\n");

    int size;
    while ((n = Rio_readnb(&riohost, buf, MAXLINE)) != 0) {
        printf("Thread %d: Forwarding response from host\n", threadIdent);
        printf("buf(%zu bytes): %s", n, buf);
        Rio_writen(connfd, buf, n);
        size += n;
        bzero(buf, MAXLINE);
    }
    printf("*** End of Request**\n");
    printf("Thread %d: Forwarded %d bytes from end server to client\n", threadIdent, size);

    sem_t mutex2;
    sem_init(&mutex2, 0, 1);
    P(&mutex2);
    V(&mutex2);
    Close(hostfd);
    return;
}

int parse_uri(char *uri, char *hostname, char *pathname, int *port) {
    char *hostbegin;
    char *hostend;
    char *pathbegin;
    int len;

    if (strncasecmp(uri, "http://", 7) != 0) {
        hostname[0] = '\0';
        return -1;
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

void get_http_headers(char *http_header, char *host, char *path, int port, rio_t *client_rio) {
    char buf[MAXLINE], headers[MAXLINE];

    while (Rio_readlineb(client_rio, buf, MAXLINE) > 0 && strcmp(buf, "\r\n")) {
        if (strstr(buf, "User-Agent"))
            strcat(headers, user_agent_hdr);
        if (strstr(buf, "Host")) {
            strcat(headers, buf);
            if (*host == '\0')
                sscanf(buf, "Host: %[^:\r\n]", host);
            if (port == 0)
                sscanf(buf, "Host: %*[^:]:%d", &port);
        }
    }
    sprintf(http_header, "GET %s HTTP/1.0\r\n", path);
    sprintf(http_header, "%s%s", http_header, headers);
    sprintf(http_header, "%s", http_header);
    sprintf(http_header, "%sConnection: close\r\n", http_header);
    sprintf(http_header, "%sProxy-Connection: close\r\n\r\n", http_header);
    printf("Forwarding connection with headers: \n%s", http_header);
    return;
}

int get_endserver(char *host, int port, char *http_header) {
    char port_str[6];  // max upto 5, one for "\0"
    sprintf(port_str, "%d", port);
    return Open_clientfd(host, port_str);
}