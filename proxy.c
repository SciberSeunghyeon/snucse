//
// proxy.c - CS:APP Web proxy
//
// Student Information:
//     김지현, 2013-11392
//
// 0.  argv[1]에서 프록시를 작동시킬 포트 번호를 얻어낸다.
// 1.  소켓 하나를 초기화시켜 연결이 들어올때까지 기다린다.
// 2.  연결이 맺어질경우 맨 첫번째줄만 먼저 읽은다음 거기서 URI를 얻어낸다.
//     얻어낸 URI를 hostname과 포트번호, path로 분리한다음, hostname에 대응되는
//     IP주소가 몇인지 DNS Lookup을 수행한다.
// 3.  freeaddrinfo() 를 호출해 DNS Lookup 결과를 정리한다.
// 4.  클라이언트쪽 소켓으로부터 EOF에 도달할때까지 모든 정보를 읽어서, 2번에서
//     읽었던 첫번째줄 뒤에 붙인다. 이것으로 클라이언트가 전송한 온전한
//     payload를 버퍼에 저장하였다.
// 5.  클라이언트와 연결을 종료한다.
// 6.  1번으로 돌아간다.
//
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

// Max text line length
#define MAXLINE 8192


//
// Function prototypes
//
static ssize_t read_line(int fd, void *buf, size_t bufsize);
static ssize_t write_all(int fd, const void *buf, size_t count);
static int parse_uri(char *uri, char *target_addr, int *port);
static void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, const char *uri, size_t size);


static int yes(int ret) {
  if (ret != -1) { return ret; }
  perror("Critical error");
  exit(1);
}


//
// Main routine for the proxy program
//
int main(int argc, char **argv) {
  // Check arguments
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <port number>\n", argv[0]);
    return 1;
  }

  // Initialize incoming socket
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(atoi(argv[1]));
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  const int sock = yes(socket(addr.sin_family, SOCK_STREAM, 0));

  const int opt = true;
  yes(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)));
  yes(bind(sock, (struct sockaddr*)&addr, sizeof(addr)));
  yes(listen(sock, SOMAXCONN));

  // Print message
  printf("Listening on \e[33m%s:%d\e[0m ...\n\n", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

  // Initialize read/write buffer
  const size_t bufsize = 100;
  void *const buf = malloc(bufsize);

  while (true) {
    int ret;

    // 클라이언트와 커넥션 맺을때까지 대기
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client = accept(sock, (struct sockaddr*)&client_addr, &client_len);
    if (client == -1) { perror("accept"); continue; }
    printf("Connected\n  %s:%d", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    // Read first line
    ssize_t count = read_line(client, buf, bufsize);

    // Parse URI
    char hostname[MAXLINE] = {};
    int port;
    ret = parse_uri(memmem(buf, count, "http://", 7), hostname, &port);
    if (ret == -1) { fprintf(stderr, "parse_uri: There was no \"http://\" on the first line of the payload\n"); goto close_client; }

    // DNS Lookup
    struct addrinfo *result;
    ret = getaddrinfo(hostname, NULL, NULL, &result);
    if (ret) { fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret)); goto close_client; }

    // Set port number
    struct sockaddr_in *addr = (struct sockaddr_in*)result->ai_addr;
    addr->sin_port = htons(port);

    // Print lookup result
    printf(" -> \e[36m%s\e[0m (%s:%d)\n", hostname, inet_ntoa(addr->sin_addr), ntohs(addr->sin_port));

    // Make a new connection toward the server
    const int sock = socket(result->ai_family, result->ai_socktype, 0);
    if (sock == -1) { perror("socket"); freeaddrinfo(result); goto close_client; }
    ret = connect(sock, result->ai_addr, result->ai_addrlen);
    if (ret == -1) { perror("connect"); freeaddrinfo(result); goto close_sock; }

    // Deallocate DNS Lookup result
    freeaddrinfo(result);

    // Proxy the first line
    ret = write_all(sock, buf, count);
    if (ret == -1) { perror("write_all"); goto close_sock; }
    printf("    client     ->     server    (%ld bytes)\n", count);

    int up[2];
    ret = pipe(up);
    if (ret == -1) { perror("pipe"); goto close_sock; }

    int flags = fcntl(client, F_GETFL, 0);
    if (flags == -1) { perror("fcntl"); goto close_pipe_up; }
    ret = fcntl(client, F_SETFL, flags | O_NONBLOCK);
    if (ret == -1) { perror("fcntl"); goto close_pipe_up; }

    ssize_t len = splice(client, NULL, up[1], NULL, 10000, SPLICE_F_MOVE | SPLICE_F_MORE);
    if (len == -1) { perror("splice"); goto close_pipe_up; }
    printf("    client -> pipe              (%ld bytes)\n", len);

    len = splice(up[0], NULL, sock, NULL, len, SPLICE_F_MOVE | SPLICE_F_MORE);
    if (len == -1) { perror("splice"); goto close_pipe_up; }
    printf("              pipe -> server    (%ld bytes)\n", len);

    // Release resources
close_pipe_up:
    close(up[0]);
    close(up[1]);
close_sock:
    close(sock);
close_client:
    close(client);
    printf("Closed\n\n");
  }

  // Release resources
  free(buf);
  yes(close(sock));
  return 0;
}


//
// 주어진 버퍼가 다 차거나, fd에서 EOF나 LF를 줄때까지 읽기를 계속한다.
//
// If return value == bufsize, the buffer is full
// Otherwise, met EOF or error
//
static ssize_t read_line(int fd, void *buf, size_t bufsize) {
  void *current = buf;
  size_t remain = bufsize;
  bool newline = false;

  do {
    ssize_t count = read(fd, current, remain);
    if (count == 0 || count == -1) { break; }

    void *pos = memchr(current, '\n', count);
    if (pos != NULL) { newline = true; }

    remain -= count;
    current = (void*)((uintptr_t)current + count);
  } while (remain > 0 && !newline);
  return bufsize - remain;
}


//
// 버퍼에 있는 모든 내용물을 fd에 쓴다.
//
// If return value == count, successfully wrote everything
// Otherwise, error
//
static ssize_t write_all(int fd, const void *buf, size_t count) {
  const void *current = buf;
  size_t remain = count;

  do {
    ssize_t count = write(fd, current, remain);
    if (count == -1) { return -1; }

    remain -= count;
    current = (void*)((uintptr_t)current + count);
  } while (remain > 0);
  return count - remain;
}


//
// parse_uri - URI parser
//
// Given a URI from an HTTP proxy GET request (i.e., a URL), extract the host
// name and port. The memory for hostname must already be allocated and should
// be at least MAXLINE bytes. Return -1 if there are any problems.
//
int parse_uri(char *uri, char *hostname, int *port) {
  if (uri == NULL || strncasecmp(uri, "http://", 7) != 0) {
    hostname[0] = '\0';
    return -1;
  }

  // Extract the host name
  char *hostbegin = uri + 7;
  char *hostend = strpbrk(hostbegin, " :/\r\n\0");
  int len = hostend - hostbegin;
  strncpy(hostname, hostbegin, len);
  hostname[len] = '\0';

  // Extract the port number
  *port = 80;
  if (*hostend == ':') { *port = atoi(hostend + 1); }

  return 0;
}


//
// format_log_entry - Create a formatted log entry in logstring.
//
// The inputs are the socket address of the requesting client
// (sockaddr), the URI from the request (uri), and the size in bytes
// of the response from the server (size).
//
void format_log_entry(char *logstring, struct sockaddr_in *addr, const char *uri, size_t size) {
  // Get a formatted time string
  time_t now = time(NULL);
  char time_str[MAXLINE];
  strftime(time_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", localtime(&now));

  // Return the formatted log entry string
  sprintf(logstring, "%s: %s %s %lu", time_str, inet_ntoa(addr->sin_addr), uri, size);
}
