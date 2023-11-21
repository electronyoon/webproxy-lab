#include <stdio.h>

#include "csapp.h"

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

void doit(int connfd);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void parse_uri(char *uri, char *host, char *path, int *port);
void get_http_headers(char *http_header, char *host, char *path, int port, rio_t *client_rio);
int get_endserver(char *host, int port, char *http_header);

#define DEBUG
int main(int argc, char **argv) {
    int listenfd, connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    char host[MAXLINE], port[MAXLINE];

#ifdef DEBUG
    strcpy(port, "15213");
    char buf[MAXLINE];
    rio_t rio;
    listenfd = Open_listenfd(port);
#else
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    strcpy(port, argv[1]);
    listenfd = Open_listenfd(argv[1]);
#endif
    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *)&clientaddr, clientlen, host, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", host, port);
        doit(connfd);
        // rio_readlineb(&rio, buf, MAXLINE);
        // printf("%s", buf);
        Close(connfd);
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
        printf("proxy received %d bytes, then send\n", (int)n);
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
        strcpy(charport, "8080");
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