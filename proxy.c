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

struct cache_line {
    int valid;
    char *tag;
    char *block;
};

struct cache_set {
    struct cache_line *line;
    int *use;
};

struct cache {
    struct cache_set *set;
};

static struct cache cache;
static int set_num, line_num;
static sem_t mutex;

void *handle_thread(void *vargp);
void handle_client(int connfd, char *host, int port, struct sockaddr_in *clientAddress, int threadIdent);
int parse_uri(char *uri, char *hostname, char *pathname, int *port);
int get_from_cache(char *url, int clientfd);
void get_from_server(int connfd, char *host, char *port, char *url, char *buf_to_server, char *cache_buf);
void init_cache();
static void update_use(int *cache_use, int current, int len);
static int load_cache(char *tag, char *response);
static void save_cache(char *tag, char *response);

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

    if ((clientfd = Open_listenfd(argv[1])) < 0) {
        fprintf(stderr, "Error: %s\n", strerror(errno));
        exit(1);
    }

    sem_init(&mutex, 0, 1);
    set_num = 1;
    line_num = 10;
    init_cache();

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

        P(&mutex);
        thread->threadIdent = id++;
        V(&mutex);
        thread->hap = inet_ntoa(clientAddress.sin_addr);

        Pthread_create(&tid, NULL, handle_thread, thread);
    }
    printf("Shutting down...\n");
    Close(clientfd);
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
    char char_port[MAXLINE];
    sprintf(char_port, "%d", port);

    char buf_to_server[MAXLINE];
    strcpy(buf_to_server, "GET /");
    strcat(buf_to_server, path);
    strcat(buf_to_server, " HTTP/1.0\r\n");
    strcat(buf_to_server, "Host: ");
    strcat(buf_to_server, host);
    strcat(buf_to_server, "\r\n\r\n");

    char cache_buf[MAX_OBJECT_SIZE];
    if (load_cache(uri, cache_buf) == 1) {
        printf("Hit!\n");
        if (rio_writen(connfd, cache_buf, sizeof(cache_buf)) < 0) {
            fprintf(stderr, "Error: cache load!\n");
            return;
        }
        memset(cache_buf, 0, sizeof(cache_buf));
    } else {
        get_from_server(connfd, host, char_port, uri, buf_to_server, cache_buf);
    }

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

void get_from_server(int connfd, char *host, char *port, char *url, char *buf_to_server, char *cache_buf) {
    int serverfd, len, len_sum = 0;
    if ((serverfd = Open_clientfd(host, port)) < 0) {
        fprintf(stderr, "open server fd error\n");
        return;
    }

    rio_t rio_to_server;
    Rio_readinitb(&rio_to_server, serverfd);
    Rio_writen(serverfd, buf_to_server, strlen(buf_to_server));

    memset(cache_buf, 0, sizeof(cache_buf));
    char *buf_to_client = Malloc(MAXLINE);
    while ((len = Rio_readnb(&rio_to_server, buf_to_client, MAXLINE)) != 0) {
        Rio_writen(connfd, buf_to_client, len);
        strcat(cache_buf, buf_to_client);
        len_sum += len;
        memset(buf_to_client, 0, sizeof(buf_to_client));
    }
    if (len_sum <= MAX_OBJECT_SIZE) {
        P(&mutex);
        save_cache(url, cache_buf);
        V(&mutex);
    }
    close(serverfd);
}

void init_cache() {
    int i, j;
    cache.set = Malloc(sizeof(struct cache_set) * set_num);
    for (i = 0; i < set_num; i++) {
        cache.set[i].line = Malloc(sizeof(struct cache_line) * line_num);
        cache.set[i].use = Malloc(sizeof(int) * line_num);
        for (j = 0; j < line_num; j++) {
            cache.set[i].use[j] = j;
            cache.set[i].line[j].valid = 0;
            cache.set[i].line[j].tag = Malloc(MAXLINE);
            cache.set[i].line[j].block = Malloc(MAX_OBJECT_SIZE);
        }
    }
}

static void update_use(int *cache_use, int current, int len) {
    int i, j;
    for (i = 0; i < len; i++)
        if (cache_use[i] == current)
            break;
    for (j = i; j > 0; j--)
        cache_use[j] = cache_use[j - 1];
    cache_use[0] = current;
}

static int load_cache(char *tag, char *response) {
    int index, i;
    index = 0;
    for (i = 0; i < line_num; i++) {
        if (cache.set[index].line[i].valid == 1 &&
            (strcmp(cache.set[index].line[i].tag, tag) == 0)) {
            P(&mutex);
            update_use(cache.set[index].use, i, line_num);
            V(&mutex);
            strcpy(response, cache.set[index].line[i].block);
            break;
        }
    }
    if (i == line_num) {
        return 0;
    } else {
        return 1;
    }
}

static void save_cache(char *tag, char *response) {
    int index, eviction;
    index = 0;
    eviction = cache.set[index].use[line_num - 1];
    strcpy(cache.set[index].line[eviction].tag, tag);
    strcpy(cache.set[index].line[eviction].block, response);
    if (cache.set[index].line[eviction].valid == 0) {
        cache.set[index].line[eviction].valid = 1;
    }
    update_use(cache.set[index].use, eviction, line_num);
}