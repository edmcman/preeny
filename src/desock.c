#define _GNU_SOURCE

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <poll.h>

#include "logging.h"

#define PREENY_MAX_FD 8192
#define PREENY_SOCKET_OFFSET 500
#define READ_BUF_SIZE 65536

#define PREENY_SOCKET(x) (x+PREENY_SOCKET_OFFSET)
#define PREENY_UNSOCKET(x) (x-PREENY_SOCKET_OFFSET)

int preeny_desock_shutdown_flag = 0;
pthread_t *preeny_socket_threads_to_front[PREENY_MAX_FD] = { 0 };
pthread_t *preeny_socket_threads_to_back[PREENY_MAX_FD] = { 0 };
int preeny_socket_acceptfd[PREENY_MAX_FD] = { 0 };

int preeny_socket_sync(int from, int to, int timeout)
{
	struct pollfd poll_in = { from, POLLIN, 0 };
	char read_buf[READ_BUF_SIZE];
	int total_n;
	char error_buf[1024];
	int n;
	int r;

	r = poll(&poll_in, 1, timeout);
	if (r < 0)
	{
		strerror_r(errno, error_buf, 1024);
		preeny_debug("read poll() received error '%s' on fd %d\n", error_buf, from);
		return 0;
	}
	else if (poll_in.revents == 0)
	{
		preeny_debug("read poll() timed out on fd %d\n", from);
		return 0;
	}

	total_n = read(from, read_buf, READ_BUF_SIZE);
	if (total_n < 0)
	{
		strerror_r(errno, error_buf, 1024);
		preeny_info("synchronization of fd %d to %d shutting down due to read error '%s'\n", from, to, error_buf);
		return -1;
	}
	else if (total_n == 0 && from == 0)
	{
          preeny_info("synchronization of fd %d to %d shutting down due to EOF\n", from, to);
          int ret;

          shutdown(to, SHUT_WR);

          while (ret = recv(PREENY_UNSOCKET(to), &ret, 0, MSG_PEEK|MSG_DONTWAIT) != -1) {
            preeny_debug("recv(to) returns %d\n", ret);
          }
          //preeny_debug("ERROR %s\n", strerror(errno));
          if (errno = EBADF) {
            preeny_debug("Remote side of %d has been shutdown\n", PREENY_UNSOCKET(to));
            exit(0);
          } else {
            perror("Unexpected preeny error:");
            exit(1);
          }
          return -1;
	}
	preeny_debug("read %d bytes from %d (will write to %d)\n", total_n, from, to);

	n = 0;
	while (n != total_n)
	{
		r = write(to, read_buf, total_n - n);
		if (r < 0)
		{
			strerror_r(errno, error_buf, 1024);
			preeny_info("synchronization of fd %d to %d shutting down due to read error '%s'\n", from, to, error_buf);
			return -1;
		}
		n += r;
	}

	preeny_debug("wrote %d bytes to %d (had read from %d)\n", total_n, to, from);
	return total_n;
}

__attribute__((destructor)) void preeny_desock_shutdown()
{
	int i;
	int to_sync[PREENY_MAX_FD] = { };

	preeny_debug("shutting down desock...\n");
	preeny_desock_shutdown_flag = 1;


	for (i = 0; i < PREENY_MAX_FD; i++)
	{
		if (preeny_socket_threads_to_front[i])
		{
			preeny_debug("sending SIGINT to thread %d...\n", i);
			pthread_join(*preeny_socket_threads_to_front[i], NULL);
			pthread_join(*preeny_socket_threads_to_back[i], NULL);
			preeny_debug("... sent!\n");
			to_sync[i] = 1;
		}
	}

	for (i = 0; i < PREENY_MAX_FD; i++)
	{
		if (to_sync[i])
		{
			//while (preeny_socket_sync(0, PREENY_SOCKET(i), 10) > 0);
			while (preeny_socket_sync(PREENY_SOCKET(i), 1, 0) > 0);
		}
	}

	preeny_debug("... shutdown complete!\n");
}

void preeny_socket_sync_loop(int from, int to)
{
	char error_buf[1024];
	int r;

	preeny_debug("starting forwarding from %d to %d!\n", from, to);

	while (!preeny_desock_shutdown_flag)
	{
		r = preeny_socket_sync(from, to, 15);
		if (r < 0) return;
	}
}

#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"

void *preeny_socket_sync_to_back(void *fd)
{
	int front_fd = (int)fd;
	int back_fd = PREENY_SOCKET(front_fd);
	preeny_socket_sync_loop(back_fd, 1);
	return NULL;
}

void *preeny_socket_sync_to_front(void *fd)
{
	int front_fd = (int)fd;
	int back_fd = PREENY_SOCKET(front_fd);
	preeny_socket_sync_loop(0, back_fd);
	return NULL;
}

//
// originals
//
int (*original_socket)(int, int, int);
int (*original_bind)(int, const struct sockaddr *, socklen_t);
int (*original_listen)(int, int);
int (*original_accept)(int, struct sockaddr *, socklen_t *);
int (*original_connect)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int (*original_setsockopt)(int sockfd, int level, int optname,
                           const void *optval, socklen_t optlen);
int (*original_fcntl)(int fd, int cmd, ...);
__attribute__((constructor)) void preeny_desock_orig()
{
	original_socket = dlsym(RTLD_NEXT, "socket");
	original_listen = dlsym(RTLD_NEXT, "listen");
	original_accept = dlsym(RTLD_NEXT, "accept");
	original_bind = dlsym(RTLD_NEXT, "bind");
	original_connect = dlsym(RTLD_NEXT, "connect");
        original_setsockopt = dlsym(RTLD_NEXT, "setsockopt");
	original_fcntl = dlsym(RTLD_NEXT, "fcntl");
}

int socket(int domain, int type, int protocol)
{
        int acceptfds[2]; // These fds are returned by accept
        int socketfds[2]; // These are returned by socket
        int front_socket;
	int back_socket;

	if (domain != AF_INET && domain != AF_INET6)
	{
	  preeny_info("Ignoring non-internet socket: %d\n", domain);
	  return original_socket(domain, type, protocol);
	}

	preeny_debug("Intercepted socket()!\n");

        int r = socketpair(AF_UNIX, type, 0, acceptfds);
        if (r != 0) {
          perror("preeny socket emulation failed:");
          return -1;
        }

        r = socketpair(AF_UNIX, type, 0, socketfds);
        if (r != 0) {
          perror("preeny socket emulation failed:");
          return -1;
        }

        // Write to socket fds to make it look like we have a new connection
	static int first = 1;
	if (first) {
	  if (send(socketfds[1], socketfds, 1, 0) != 1) {
	    perror("preeny socket emulation failed:");
	    return -1;
	  }
	  first = 0;
	}

	preeny_debug("... created socket pair (%d, %d)\n", socketfds[0], socketfds[1]);

	front_socket = acceptfds[0];
	back_socket = dup2(acceptfds[1], PREENY_SOCKET(front_socket));
	close(acceptfds[1]);

	preeny_debug("... dup into socketpair (%d, %d)\n", front_socket, back_socket);

	preeny_socket_threads_to_front[socketfds[0]] = malloc(sizeof(pthread_t));
	preeny_socket_threads_to_back[socketfds[0]] = malloc(sizeof(pthread_t));

        preeny_socket_acceptfd[socketfds[0]] = acceptfds[0];

	r = pthread_create(preeny_socket_threads_to_front[socketfds[0]], NULL, (void*(*)(void*))preeny_socket_sync_to_front, (void *)front_socket);
	if (r)
	{
		perror("failed creating front-sync thread");
		return -1;
	}

	r = pthread_create(preeny_socket_threads_to_back[socketfds[0]], NULL, (void*(*)(void*))preeny_socket_sync_to_back, (void *)front_socket);
	if (r)
	{
		perror("failed creating back-sync thread");
		return -1;
	}

	return socketfds[0];
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
         static int first = 1;
         //initialize a sockaddr_in for the peer
	 struct sockaddr_in peer_addr;
	 memset(&peer_addr, '0', sizeof(struct sockaddr_in));

	//Set the contents in the peer's sock_addr. 
	//Make sure the contents will simulate a real client that connects with the intercepted server, as the server may depend on the contents to make further decisions. 
	//The followings set-up should be fine with Nginx.
	 peer_addr.sin_family = AF_INET;
	 peer_addr.sin_addr.s_addr = htonl(INADDR_ANY);
         peer_addr.sin_port = htons(9000); 

	//copy the initialized peer_addr back to the original sockaddr. Note the space for the original sockaddr, namely addr, has already been allocated
	if (addr) memcpy(addr, &peer_addr, sizeof(struct sockaddr_in));

	if (preeny_socket_threads_to_front[sockfd]) {
          if (first) {
            int dupfd = preeny_socket_acceptfd[sockfd];
            preeny_debug("accept returning fd %d\n", dupfd);
            first = 0;
            return dupfd;
          } else {
            preeny_debug("accept is called again\n");
	    errno = EWOULDBLOCK;
            return -1;
          }
        }
	else return original_accept(sockfd, addr, addrlen);
}

int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
       return accept(sockfd, addr, addrlen);
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	if (preeny_socket_threads_to_front[sockfd])
	{
		preeny_info("Emulating bind on port %d\n", ntohs(((struct sockaddr_in*)addr)->sin_port));
		return 0;
	}
	else
	{
		return original_bind(sockfd, addr, addrlen);
	}
}

int listen(int sockfd, int backlog)
{
	if (preeny_socket_threads_to_front[sockfd]) return 0;
	else return original_listen(sockfd, backlog);
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	if (preeny_socket_threads_to_front[sockfd]) return 0;
	else return original_connect(sockfd, addr, addrlen);
}

int setsockopt(int sockfd, int level, int optname,
               const void *optval, socklen_t optlen)
{
  if (preeny_socket_threads_to_front[sockfd]) return 0;
  else return original_setsockopt(sockfd, level, optname, optval, optlen);
}

int fcntl(int fd, int cmd, int arg)
{
  if (preeny_socket_threads_to_front[fd]) {
    if (cmd == 0 /*F_DUPFD*/) {
      int newfd = original_fcntl(fd, cmd, arg);
      if (newfd < 0) {
	perror("fcntl failed:");
	return newfd;
      }
      preeny_debug("Dup'ing %d to %d\n", fd, newfd);
      preeny_socket_threads_to_front[newfd] = preeny_socket_threads_to_front[fd];
      preeny_socket_threads_to_back[newfd] = preeny_socket_threads_to_back[fd];
      preeny_socket_acceptfd[newfd] = preeny_socket_acceptfd[fd];
      return newfd;
    }
  } else return original_fcntl(fd, cmd, arg);
}
