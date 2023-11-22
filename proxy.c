#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "csapp.h"

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define MAXEPOLLSIZE 1000
#define BACKLOG 200  // how many pending connections queue will hold
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

void doit(int connfd);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void parse_uri(char *uri, char *host, char *path, int *port);
void get_http_headers(char *http_header, char *host, char *path, int port, rio_t *client_rio);
int get_endserver(char *host, int port, char *http_header);
int set_non_blocking(int sockfd);

// #define DEBUG
int main(int argc, char **argv) {
    socklen_t clientlen;
    char host[MAXLINE], port[MAXLINE];
    int status;
    int sockfd, new_fd, epollfd, nfds, n, curfds;
    struct addrinfo hints;
    struct addrinfo *listp;  // will point to the results
    struct addrinfo *p;
    struct sockaddr_storage client_addr;
    struct epoll_event ev;
    struct epoll_event *events;
    socklen_t addr_size;

#ifdef DEBUG
    strcpy(port, "15213");
    char buf[MAXLINE];
    rio_t rio;
    // listenfd = Open_listenfd(port);
    argv[1] = port;
#else
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    strcpy(port, argv[1]);
    // listenfd = Open_listenfd(argv[1]);
#endif

    memset(&hints, 0, sizeof hints);  // make sure the struct is empty
    hints.ai_family = AF_UNSPEC;      // don't care IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;  // TCP stream sockets
    hints.ai_flags = AI_PASSIVE;      // fill in my IP for me
    if ((status = getaddrinfo(NULL, argv[1], &hints, &listp)) != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
        return 2;
    }

    // listp now points to a linked list of 1 or more struct addrinfos
    for (p = listp; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            perror("server: socket");
            continue;
        }
        set_non_blocking(sockfd);
        if ((bind(sockfd, p->ai_addr, p->ai_addrlen)) == -1) {
            close(sockfd);
            perror("server: bind");
            continue;
        }
        break;
    }

    if (p == NULL) {
        fprintf(stderr, "server: failed to bind\n");
        return 2;
    }

    freeaddrinfo(listp);

    if (listen(sockfd, BACKLOG) == -1) {
        perror("listen");
        exit(1);
    }

    printf("server: waiting for connections...\n");

    epollfd = epoll_create(MAXEPOLLSIZE);
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = sockfd;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sockfd, &ev) < 0) {
        perror("epoll_ctl: listen_sock");
        exit(EXIT_FAILURE);
    } else {
        printf("success insert listening socket into epoll.\n");
    }

    events = calloc(MAXEPOLLSIZE, sizeof ev);
    curfds = 1;
    while (1) {
        nfds = epoll_wait(epollfd, events, curfds, -1);
        if (nfds == -1) {
            perror("epoll_pwait");
            exit(EXIT_FAILURE);
        }
        for (n = 0; n < nfds; ++n) {
            if (events[n].data.fd == sockfd) {
                addr_size = sizeof client_addr;
                new_fd = accept(sockfd, (struct sockaddr *)&client_addr, &addr_size);
                if (new_fd == -1) {
                    if ((errno == EAGAIN) ||
                        (errno == EWOULDBLOCK)) {
                        break;
                    } else {
                        perror("accept");
                        break;
                    }
                }
                printf("server: connection established...\n");
                set_non_blocking(new_fd);
                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = new_fd;
                if (epoll_ctl(epollfd, EPOLL_CTL_ADD, new_fd, &ev) < 0) {
                    printf("Failed to insert socket into epoll.\n");
                }
                curfds++;
            } else {
                int connfd = events[n].data.fd;
                doit(connfd);
                // if (send(events[n].data.fd, "Hello, world!", 13, 0) == -1) {
                //     perror("send");
                //     break;
                // }
                epoll_ctl(epollfd, EPOLL_CTL_DEL, events[n].data.fd, &ev);
                curfds--;
                close(events[n].data.fd);
            }
        }
    }
    free(events);
    close(sockfd);

    printf("Shutting down...\n");
    return EXIT_SUCCESS;
}

int set_non_blocking(int sockfd) {
    int flags, s;
    flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl");
        return -1;
    }
    flags |= O_NONBLOCK;
    s = fcntl(sockfd, F_SETFL, flags);
    if (s == -1) {
        perror("fcntl");
        return -1;
    }
    return 0;
}

void doit(int connfd) {
    int serverfd;
    int end_serverfd;
    char server_http_header[MAXLINE];
    rio_t rio, server_rio;

    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char host[MAXLINE], path[MAXLINE];
    int port;

    Rio_readinitb(&rio, connfd);
    Rio_readlineb(&rio, buf, MAXLINE);
    sscanf(buf, "%s %s %s", method, uri, version);

    if (strcasecmp(method, "GET")) {
        printf("Proxy does not implement the method");
        clienterror(connfd, method, "501", "Not implemented", "Tiny does not implement this method");
        return;
    }

    parse_uri(uri, host, path, &port);

    get_http_headers(server_http_header, host, path, port, &rio);

    end_serverfd = get_endserver(host, port, server_http_header);
    if (end_serverfd < 0) {
        printf("Connection failed\n");
        return;
    }

    Rio_readinitb(&server_rio, end_serverfd);
    Rio_writen(end_serverfd, server_http_header, strlen(server_http_header));

    size_t n;
    // while (Rio_readlineb(&server_rio, buf, MAXLINE) > 0) {
    while ((n = Rio_readlineb(&server_rio, buf, MAXLINE)) != 0) {
        printf("proxy received %d bytes, then send\n", n);
        Rio_writen(connfd, buf, n);
    }
    Close(end_serverfd);
    return;
}

void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXLINE], body[MAXBUF];
    sprintf(body, "<html><title>Proxy Error</title>");
    sprintf(body,
            "%s<body bgcolor="
            "ffffff"
            ">\r\n",
            body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Proxy Web server</em>\r\n", body);

    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}

void parse_uri(char *uri, char *host, char *path, int *port) {
    char *host_ptr = strstr(uri, "//") ? strstr(uri, "//") + 2 : uri;
    char *port_ptr = strchr(host_ptr, ':');
    char *path_ptr = strchr(host_ptr, '/');
    char *charport;
    strcpy(path, path_ptr);

    if (port_ptr) {
        strncpy(charport, port_ptr + 1, path_ptr - port_ptr - 1);
        strncpy(host, host_ptr, port_ptr - host_ptr);
    } else {
        strcpy(charport, "8000");
        strncpy(host, host_ptr, path_ptr - host_ptr);
    }
    *port = atoi(charport);
    return;
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