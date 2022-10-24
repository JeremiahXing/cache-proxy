#include <stdio.h>
#include <strings.h>
#include "csapp.h"
#include "sbuf.h"
#include "cache.h"


/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define SBUFSIZE 32

const int NTHREADS=8;
sbuf_t sbuf; /* Shared buffer of connected descriptors */

static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/105.0.0.0 Safari/537.36\r\n";
static const char *connection_hdr = "Connection: close\r\n";
static const char *proxy_connection_hdr = "Proxy-Connection: close\r\n";


int handle_uri(char *uri, char *hostname, char *path, int *port);
void handle_proxy(int fd);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void *thread(void *vargp);
void (*cache_insert)(char* hostname, char *path, int port, char *content, size_t size) = cache_insert_LRU; //default is LRU replacement
cache_block* (*cache_find)(char* hostname, char *path, int port) = cache_find_LRU; //default is LRU replacement
int cache_state = 0; //default is LRU replacement

int main(int argc, char **argv) {
  int i, listenfd, connfd;
  socklen_t clientlen;
  struct sockaddr_storage clientaddr; 
  char client_hostname[MAXLINE], client_port[MAXLINE];
  pthread_t tid;

  if (argc < 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(0);
  }
  
  if (argc == 3) {
    if (strcasecmp(argv[2], "LFU") == 0) {
        cache_insert = cache_insert_LFU;
        cache_find = cache_find_LFU;
        cache_state = 1;
    }
  }

    
    
  listenfd = Open_listenfd(argv[1]);
  printf(">Server started listening port %s\n", argv[1]);
  sbuf_init(&sbuf, SBUFSIZE);
  printf(">Shared buffer initialized\n");

  cache_init();
  printf(">Cache initialized\n");
    if (cache_state == 0) {
        printf(">Cache replacement policy: LRU\n");
    } else {
        printf(">Cache replacement policy: LFU\n");
    }

  for (i = 0; i < NTHREADS; i++){     /* Create worker threads */
    int *id = Malloc(sizeof(int));
    *id = i;
    Pthread_create(&tid, NULL, thread, (void *)id);
  }
  printf(">Worker threads created\n");


  Signal(SIGPIPE, SIG_IGN);  // Ignore SIGPIPE
  while (1) {
    clientlen = sizeof(struct sockaddr_storage);  /* Important! */
    connfd = Accept(listenfd, (SA *) &clientaddr, &clientlen);
    Getnameinfo((SA *) &clientaddr, clientlen, client_hostname, MAXLINE, client_port, MAXLINE, 0);
    sbuf_insert(&sbuf, connfd);  /* Insert connfd in buffer */
    printf(">cilentfd %d in queue \n", connfd);
    printf(">Accepted connection from (%s, %s)\n", client_hostname, client_port);
  }
  sbuf_deinit(&sbuf);
  cache_deinit();
  exit(0);
}

void *thread(void *vargp) {
  Pthread_detach(pthread_self());
  int id = *((int *)vargp);
  Free(vargp);
  while(1){
    int connfd = sbuf_remove(&sbuf);
    printf("Working thread[%d] >>>>>>>>>>>>>>>>>>>>>>>>> handling cilentfd[%d]\n", id, connfd);
    handle_proxy(connfd);
    Close(connfd);
    printf("Working thread[%d] <<<<<<<<<<<<<<<<<<<<<<<<<< cilentfd[%d] closed!\n\n", id, connfd);
  }
  return NULL;
}

void handle_proxy(int fd) {
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char hostname[MAXLINE], path[MAXLINE], port[MAXLINE];
  char server_buf[MAXLINE+10], obj[MAX_OBJECT_SIZE];
  rio_t rio_cilent, rio_server;
  int serverfd;
  int port_int;
  size_t n, obj_len = 0;

  rio_readinitb(&rio_cilent, fd);
  rio_readlineb(&rio_cilent, buf, MAXLINE);

  sscanf(buf, "%s %s %s", method, uri, version);
  printf("Request line:\n");
  printf("%s", buf);
  if (strcasecmp(method, "GET")) {
    clienterror(fd, method, "501", "Not Implemented", "Proxy does not implement this method");
    return;
  }
  if (!handle_uri(uri, hostname, path, &port_int)) {
    clienterror(fd, uri, "400", "Bad Request", "Proxy could not parse the request");
    return;
  } else {
    // connect to server
    printf("hostname: %s, url: %s, port: %d\n", hostname, path, port_int);
    sprintf(port, "%d", port_int);

    // read response from server
    //TODO Cache
    
    //check cache
    cache_block* block = cache_find(hostname, path, port_int);
    if (block != NULL) {
      printf("Cache hit!\n");
      rio_writen(fd, block->content, block->size);
      //test cache
      // printf("--------------------------------------------------------------------------------\n");
      // for (int i = 0; i < block->size; i++) {
      //   if(block->content[i] != 0) {
      //     printf("%c", block->content[i]);
      //   } else {
      //     printf(" ");
      //   }
      // }
      // printf("\n--------------------------------------------------------------------------------\n");
      //test cache
      printf("Respond %ld bytes object:\n", block->size);
      //Close(serverfd);
      return;
    } else {
      printf("Cache miss!\n");
      //print_cache();

      serverfd = open_clientfd(hostname, port);
      if (serverfd < 0) {
        clienterror(fd, hostname, "404", "Not Found", "Proxy could not connect to this server");
        return;
      }

      // send request to server
      rio_readinitb(&rio_server, serverfd);

      sprintf(server_buf, "GET %s HTTP/1.0\r\n", path);
      rio_writen(serverfd, server_buf, strlen(server_buf));
      sprintf(server_buf, "Host: %s\r\n", hostname);
      rio_writen(serverfd, server_buf, strlen(server_buf));
      rio_writen(serverfd, (void*)user_agent_hdr, strlen(user_agent_hdr));
      rio_writen(serverfd, (void*)connection_hdr, strlen(connection_hdr));
      rio_writen(serverfd, (void*)proxy_connection_hdr, strlen(proxy_connection_hdr));
      rio_writen(serverfd, "\r\n", 2);
      while ((n = rio_readnb(&rio_server, server_buf, MAXLINE)) > 0) {
        obj_len += n;
        if (obj_len <= MAX_OBJECT_SIZE) {
          memcpy(obj + obj_len - n, server_buf, n);
        }
        rio_writen(fd, server_buf, n); // send response to client
      }
      if (obj_len <= MAX_OBJECT_SIZE) {
        cache_insert(hostname, path, port_int, obj, obj_len);
        printf("Cache insert %ld bytes object:\n", obj_len);
      } else {
        printf("Object too large to cache!\n");
      }
      printf("Respond %ld bytes object:\n", obj_len);
      //Close(serverfd);
    }
  }
}


int handle_uri(char *uri, char *hostname, char *path, int *port) {
  const char *temp_head, *temp_tail;
  char scheme[10], port_str[10];
  size_t temp_len;

  temp_head = uri;

  // get scheme
  temp_tail = strstr(temp_head, "://");
  if (temp_tail == NULL) {
    return 0;
  }
  temp_len = temp_tail - temp_head;
  strncpy(scheme, temp_head, temp_len);
  scheme[temp_len] = '\0';
  if (strcasecmp(scheme, "http")) {
    return 0;
  }

  // get hostname
  temp_head = temp_tail + 3;
  temp_tail = temp_head;
  while (*temp_tail != '\0') {
    if (*temp_tail == ':' || *temp_tail == '/') {
      break;
    }
    temp_tail++;
  }
  temp_len = temp_tail - temp_head;
  strncpy(hostname, temp_head, temp_len);
  hostname[temp_len] = '\0';

  // get port
  temp_head = temp_tail;
  if (*temp_head == ':') {
    temp_head++;
    temp_tail = temp_head;
    while (*temp_tail != '\0') {
      if (*temp_tail == '/') {
        break;
      }
      temp_tail++;
    }
    temp_len = temp_tail - temp_head;
    strncpy(port_str, temp_head, temp_len);
    port_str[temp_len] = '\0';
    *port = atoi(port_str);
  } else {
    *port = 80;
  }

  // get path
  temp_head = temp_tail;
  if (*temp_head == '/') {
    temp_tail = temp_head;
    while (*temp_tail != '\0') {
      temp_tail++;
    }
    temp_len = temp_tail - temp_head;
    strncpy(path, temp_head, temp_len);
    path[temp_len] = '\0';
    return 1;
  } else {
    path[0] = '/';
    path[1] = '\0';
    return 1;
  }
}


void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
  char buf[MAXLINE];

  /* Print the HTTP response headers */
  sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
  rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-type: text/html\r\n\r\n");
  rio_writen(fd, buf, strlen(buf));

  /* Print the HTTP response body */
  sprintf(buf, "<html><title>Proxy Error</title>");
  rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "<body bgcolor=""ffffff"">\r\n");
  rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "%s: %s\r\n", errnum, shortmsg);
  rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "<p>%s: %s\r\n", longmsg, cause);
  rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "<hr><em>My proxy server</em>\r\n");
  rio_writen(fd, buf, strlen(buf));
}
