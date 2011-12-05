/*
 * Copyright (c) 2011 Yaroslav Stavnichiy <yarosla@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <netdb.h>

#include <ev.h>

#define VERSION "1.0"

void nxweb_die(const char* fmt, ...) {
  va_list ap;
  fprintf(stderr, "FATAL: ");
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(EXIT_FAILURE);
}

static const char* get_current_time(char* buf, int max_buf_size) {
  time_t t;
  struct tm tm;
  time(&t);
  localtime_r(&t, &tm);
  strftime(buf, max_buf_size, "%F %T", &tm); // %F=%Y-%m-%d %T=%H:%M:%S
  return buf;
}

void nxweb_log_error(const char* fmt, ...) {
  char cur_time[32];
  va_list ap;

  get_current_time(cur_time, sizeof(cur_time));
  flockfile(stderr);
  fprintf(stderr, "%s [%u:%p]: ", cur_time, getpid(), (void*)pthread_self());
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
  funlockfile(stderr);
}

int setup_socket(int fd) {
  int flags=fcntl(fd, F_GETFL);
  if (flags<0) return flags;
  if (fcntl(fd, F_SETFL, flags|=O_NONBLOCK)<0) return -1;

  int nodelay=1;
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay))) return -1;

//  struct linger linger;
//  linger.l_onoff=1;
//  linger.l_linger=10; // timeout for completing reads/writes
//  setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));

  return 0;
}

void _nxweb_close_good_socket(int fd) {
//  struct linger linger;
//  linger.l_onoff=0; // gracefully shutdown connection
//  linger.l_linger=0;
//  setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
//  shutdown(fd, SHUT_RDWR);
  close(fd);
}

void _nxweb_close_bad_socket(int fd) {
  struct linger linger;
  linger.l_onoff=1;
  linger.l_linger=0; // timeout for completing writes
  setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
  close(fd);
}

void sleep_ms(int ms) {
  struct timespec req;
  time_t sec=ms/1000;
  ms%=1000;
  req.tv_sec=sec;
  req.tv_nsec=ms*1000000L;
  while(nanosleep(&req, &req)==-1) continue;
}

struct config {
  int num_connections;
  int num_requests;
  int num_threads;
  int keep_alive;
	struct addrinfo *saddr;
  const char* uri_path;
  const char* uri_host;
  char request_data[4096];
  int request_length;
  int progress_step;

  volatile int request_counter;
  int num_requests_started;
  int num_requests_complete;
};

static struct config config={1, 1, 1, 0, 0, 0, 0, "", 0};

typedef struct connection {
  struct ev_loop* loop;
  struct thread_config* tdata;
  int fd;
  ev_io watch_read;
  ev_io watch_write;
  ev_tstamp last_activity;

  int write_pos;
  int read_pos;
  int bytes_to_read;
  int keep_alive;
  int alive_count;
  int success_count;
  int done;

  char buf[32768];
  char* body_ptr;

  enum {C_CONNECTING, C_WRITING, C_READING_HEADERS, C_READING_BODY} state;
} connection;

typedef struct thread_config {
  pthread_t tid;
  connection *conns;
  int id;
  int num_conn;
  struct ev_loop* loop;
  ev_tstamp start_time;
  ev_timer watch_heartbeat;

  int shutdown_in_progress;

  int num_success;
  int num_fail;
  long num_bytes_received;
  long num_overhead_received;
  int num_connect;
  ev_tstamp avg_req_time;
} thread_config;

static inline void inc_success(connection* conn) {
  conn->success_count++;
  conn->tdata->num_success++;
  conn->tdata->num_bytes_received+=conn->bytes_to_read;
  conn->tdata->num_overhead_received+=(conn->body_ptr-conn->buf);
}

static inline void inc_fail(connection* conn) {
  conn->tdata->num_fail++;
}

static inline void inc_connect(connection* conn) {
  conn->tdata->num_connect++;
}

static int open_socket(connection* conn);
static void rearm_socket(connection* conn);

static void write_cb(struct ev_loop *loop, ev_io *w, int revents) {
  connection *conn=((connection*)(((char*)w)-offsetof(connection, watch_write)));

  if (conn->state==C_CONNECTING) {
    if (connect(conn->fd, config.saddr->ai_addr, config.saddr->ai_addrlen)) {
      if (errno==EINPROGRESS || errno==EALREADY) {
        // still connecting
        return;
      }
      else if (errno==EISCONN) {
        // already connected
      }
      else {
        strerror_r(errno, conn->buf, sizeof(conn->buf));
        nxweb_log_error("can't connect [%d] %s", errno, conn->buf);
        _nxweb_close_bad_socket(conn->fd);
        inc_fail(conn);
        open_socket(conn);
        return;
      }
    }
    conn->last_activity=ev_now(loop);
    conn->state=C_WRITING;
    //ev_feed_event(conn->loop, &conn->watch_write, EV_WRITE);
    //return;
  }

  if (conn->state==C_WRITING) {
    int bytes_avail, bytes_sent;
    do {
      bytes_avail=config.request_length-conn->write_pos;
      if (!bytes_avail) {
        conn->state=C_READING_HEADERS;
        conn->read_pos=0;
        ev_io_stop(conn->loop, &conn->watch_write);
        //ev_io_set(&conn->watch_read, conn->fd, EV_READ);
        ev_io_start(conn->loop, &conn->watch_read);
        ev_feed_event(conn->loop, &conn->watch_read, EV_READ);
        return;
      }
      bytes_sent=write(conn->fd, config.request_data+conn->write_pos, bytes_avail);
      if (bytes_sent<0) {
        if (errno!=EAGAIN) {
          strerror_r(errno, conn->buf, sizeof(conn->buf));
          nxweb_log_error("write() returned %d: %d %s", bytes_sent, errno, conn->buf);
          _nxweb_close_bad_socket(conn->fd);
          inc_fail(conn);
          open_socket(conn);
          return;
        }
        return;
      }
      if (bytes_sent) conn->last_activity=ev_now(loop);
      conn->write_pos+=bytes_sent;
    } while (bytes_sent==bytes_avail);
    return;
  }
}

static void parse_headers(connection* conn) {
  *conn->body_ptr='\0';
  conn->body_ptr+=4;

  conn->keep_alive=!strncasecmp(conn->buf, "HTTP/1.1", 8);
  char *p;
  for (p=strchr(conn->buf, '\n'); p; p=strchr(p, '\n')) {
    p++;
    if (!strncasecmp(p, "Content-Length:", 15)) {
      p+=15;
      while (*p==' ' || *p=='\t') p++;
      conn->bytes_to_read=atoi(p);
    }
    else if (!strncasecmp(p, "Connection:", 11)) {
      p+=11;
      while (*p==' ' || *p=='\t') p++;
      conn->keep_alive=!strncasecmp(p, "keep-alive", 10);
    }
  }

  conn->read_pos=conn->read_pos-(conn->body_ptr-conn->buf); // what already read
}

static void read_cb(struct ev_loop *loop, ev_io *w, int revents) {
  connection *conn=((connection*)(((char*)w)-offsetof(connection, watch_read)));

  if (conn->state==C_READING_HEADERS) {
    int room_avail, bytes_received;
    do {
      room_avail=sizeof(conn->buf)-conn->read_pos-1;
      if (!room_avail) {
        // headers too long
        nxweb_log_error("response too long");
        _nxweb_close_bad_socket(conn->fd);
        inc_fail(conn);
        open_socket(conn);
        return;
      }
      bytes_received=read(conn->fd, conn->buf+conn->read_pos, room_avail);
      if (bytes_received<0) {
        if (errno!=EAGAIN) {
          strerror_r(errno, conn->buf, sizeof(conn->buf));
          nxweb_log_error("head [%d] read() returned error: %d %s", conn->alive_count, errno, conn->buf);
          _nxweb_close_bad_socket(conn->fd);
          inc_fail(conn);
          open_socket(conn);
          return;
        }
        return;
      }
      if (bytes_received) conn->last_activity=ev_now(loop);
      conn->read_pos+=bytes_received;
      conn->buf[conn->read_pos]='\0';
      if ((conn->body_ptr=strstr(conn->buf, "\r\n\r\n"))) {
        parse_headers(conn);
        if (conn->bytes_to_read<0) {
          nxweb_log_error("response length unknown");
          _nxweb_close_bad_socket(conn->fd);
          inc_fail(conn);
          open_socket(conn);
          return;
        }
        conn->state=C_READING_BODY;
        if (conn->read_pos>=conn->bytes_to_read) {
          // already read all
          if (!bytes_received) conn->keep_alive=0;
          rearm_socket(conn);
          return;
        }
        if (!bytes_received) { // connection closed
          _nxweb_close_bad_socket(conn->fd);
          inc_fail(conn);
          open_socket(conn);
          return;
        }
        ev_feed_event(conn->loop, &conn->watch_read, EV_READ);
        return;
      }
      if (!bytes_received) { // connection closed
        _nxweb_close_bad_socket(conn->fd);
        inc_fail(conn);
        open_socket(conn);
        return;
      }
    } while (bytes_received==room_avail);
    return;
  }

  if (conn->state==C_READING_BODY) {
    int room_avail, bytes_received;
    do {
      room_avail=conn->bytes_to_read-conn->read_pos;
      if (room_avail==0) {
        // all read
        rearm_socket(conn);
        return;
      }
      if (room_avail>sizeof(conn->buf)) room_avail=sizeof(conn->buf);
      bytes_received=read(conn->fd, conn->buf, room_avail);
      if (bytes_received<0) {
        if (errno!=EAGAIN) {
          strerror_r(errno, conn->buf, sizeof(conn->buf));
          nxweb_log_error("body [%d] read() returned error: %d %s", conn->alive_count, errno, conn->buf);
          _nxweb_close_bad_socket(conn->fd);
          inc_fail(conn);
          open_socket(conn);
          return;
        }
        return;
      }
      if (bytes_received) conn->last_activity=ev_now(loop);
      conn->read_pos+=bytes_received;
    } while (bytes_received==room_avail);
    return;
  }
}

static void shutdown_thread(thread_config* tdata) {
  int i;
  connection* conn;
  ev_tstamp now=ev_now(tdata->loop);
  ev_tstamp time_limit=tdata->avg_req_time*4;
  //fprintf(stderr, "[%.6lf]", time_limit);
  for (i=0; i<tdata->num_conn; i++) {
    conn=&tdata->conns[i];
    if (!conn->done) {
      if (ev_is_active(&conn->watch_read) || ev_is_active(&conn->watch_write)) {
        if ((now - conn->last_activity) > time_limit) {
          // kill this connection
          if (ev_is_active(&conn->watch_write)) ev_io_stop(conn->loop, &conn->watch_write);
          if (ev_is_active(&conn->watch_read)) ev_io_stop(conn->loop, &conn->watch_read);
          _nxweb_close_bad_socket(conn->fd);
          inc_fail(conn);
          conn->done=1;
          //fprintf(stderr, "*");
        }
        else {
          // don't kill this yet, but wake it up
          if (ev_is_active(&conn->watch_read)) {
            ev_feed_event(tdata->loop, &conn->watch_read, EV_READ);
          }
          if (ev_is_active(&conn->watch_write)) {
            ev_feed_event(tdata->loop, &conn->watch_write, EV_WRITE);
          }
          //fprintf(stderr, ".");
        }
      }
    }
  }
}

static int more_requests_to_run() {
  int rc=__sync_add_and_fetch(&config.request_counter, 1);
  if (rc>config.num_requests) {
    return 0;
  }
  if (rc%config.progress_step==0 || rc==config.num_requests) {
    printf("%d requests launched\n", rc);
  }
  return 1;
}

static void heartbeat_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  if (config.request_counter>config.num_requests) {
    thread_config *tdata=((thread_config*)(((char*)w)-offsetof(thread_config, watch_heartbeat)));
    if (!tdata->shutdown_in_progress) {
      ev_tstamp now=ev_now(tdata->loop);
      tdata->avg_req_time=tdata->num_success? (now-tdata->start_time) * tdata->num_conn / tdata->num_success : 0.1;
      if (tdata->avg_req_time>1.) tdata->avg_req_time=1.;
      tdata->shutdown_in_progress=1;
    }
    shutdown_thread(tdata);
  }
}

static void rearm_socket(connection* conn) {
  if (ev_is_active(&conn->watch_write)) ev_io_stop(conn->loop, &conn->watch_write);
  if (ev_is_active(&conn->watch_read)) ev_io_stop(conn->loop, &conn->watch_read);

  inc_success(conn);

  if (!config.keep_alive || !conn->keep_alive) {
    _nxweb_close_good_socket(conn->fd);
    open_socket(conn);
  }
  else {
    if (!more_requests_to_run()) {
      _nxweb_close_good_socket(conn->fd);
      conn->done=1;
      ev_feed_event(conn->tdata->loop, &conn->tdata->watch_heartbeat, EV_TIMER);
      return;
    }
    config.num_requests_started++;
    conn->alive_count++;
    conn->state=C_WRITING;
    conn->write_pos=0;
    ev_io_start(conn->loop, &conn->watch_write);
    ev_feed_event(conn->loop, &conn->watch_write, EV_WRITE);
  }
}

static int open_socket(connection* conn) {

  inc_connect(conn);

  if (ev_is_active(&conn->watch_write)) ev_io_stop(conn->loop, &conn->watch_write);
  if (ev_is_active(&conn->watch_read)) ev_io_stop(conn->loop, &conn->watch_read);

  if (!more_requests_to_run(conn)) {
    conn->done=1;
    ev_feed_event(conn->tdata->loop, &conn->tdata->watch_heartbeat, EV_TIMER);
    return 1;
  }

  config.num_requests_started++;

  conn->fd=socket(config.saddr->ai_family, config.saddr->ai_socktype, config.saddr->ai_protocol);
  if (conn->fd==-1) {
    strerror_r(errno, conn->buf, sizeof(conn->buf));
    nxweb_log_error("can't open socket [%d] %s", errno, conn->buf);
    return -1;
  }
  if (setup_socket(conn->fd)) {
    nxweb_log_error("can't setup socket");
    return -1;
  }
  conn->state=C_CONNECTING;
  conn->write_pos=0;
  conn->alive_count=0;
  conn->done=0;
  ev_io_set(&conn->watch_write, conn->fd, EV_WRITE);
  ev_io_set(&conn->watch_read, conn->fd, EV_READ);
  ev_io_start(conn->loop, &conn->watch_write);
  ev_feed_event(conn->loop, &conn->watch_write, EV_WRITE);
  return 0;
}

static void* thread_main(void* pdata) {
  thread_config* tdata=(thread_config*)pdata;

//  tdata->loop=ev_loop_new(0);
//
//  int i;
//  connection* conn;
//  for (i=0; i<tdata->num_conn; i++) {
//    conn=&tdata->conns[i];
//    conn->tdata=tdata;
//    conn->loop=tdata->loop;
//    ev_io_init(&conn->watch_write, write_cb, -1, EV_WRITE);
//    ev_io_init(&conn->watch_read, read_cb, -1, EV_READ);
//    if (open_socket(conn)) return 0;
//  }

  ev_timer_init(&tdata->watch_heartbeat, heartbeat_cb, 0.1, 0.1);
  ev_timer_start(tdata->loop, &tdata->watch_heartbeat);
  ev_unref(tdata->loop); // don't keep loop running just for heartbeat
  ev_run(tdata->loop, 0);

  ev_loop_destroy(tdata->loop);

  printf("thread %d: %d connect, %d requests, %d success, %d fail, %ld bytes, %ld overhead\n",
         tdata->id, tdata->num_connect, tdata->num_success+tdata->num_fail,
         tdata->num_success, tdata->num_fail, tdata->num_bytes_received,
         tdata->num_overhead_received);

  return 0;
}


static int resolve_host(struct addrinfo** saddr, const char *host_and_port) {
  char* host=strdup(host_and_port);
  char* port=strchr(host, ':');
  if (port) *port++='\0';
  else port="80";

	struct addrinfo hints, *res, *res_first, *res_last;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family=PF_UNSPEC;
	hints.ai_socktype=SOCK_STREAM;

	if (getaddrinfo(host, port, &hints, &res_first)) goto ERR1;

	// search for an ipv4 address, no ipv6 yet
	res_last=0;
	for (res=res_first; res; res=res->ai_next) {
		if (res->ai_family==AF_INET) break;
		res_last=res;
	}

	if (!res) goto ERR2;
	if (res!=res_first) {
		// unlink from list and free rest
		res_last->ai_next = res->ai_next;
		freeaddrinfo(res_first);
		res->ai_next=0;
	}

  free(host);
  *saddr=res;
	return 0;

ERR2:
	freeaddrinfo(res_first);
ERR1:
  free(host);
  return -1;
}

static void show_help(void) {
	printf( "httpress <options> <url>\n"
          "  -n num   number of requests     (default: 1)\n"
          "  -t num   number of threads      (default: 1)\n"
          "  -c num   concurrent connections (default: 1)\n"
          "  -k       keep alive             (default: no)\n"
          "  -h       show this help\n"
          //"  -v       show version\n"
          "\n"
          "example: httpress -n 10000 -c 100 -t 4 -k http://localhost:8080/index.html\n\n");
}

static char host_buf[1024];

static int parse_uri(const char* uri) {
  if (strncmp(uri, "http://", 7)) return -1;
  uri+=7;
  const char* p=strchr(uri, '/');
  if (!p) {
    config.uri_host=uri;
    config.uri_path="/";
    return 0;
  }
  if ((p-uri)>sizeof(host_buf)-1) return -1;
  strncpy(host_buf, uri, (p-uri));
  host_buf[(p-uri)]='\0';
  config.uri_host=host_buf;
  config.uri_path=p;
  return 0;
}

int main(int argc, char* argv[]) {
  config.num_connections=1;
  config.num_requests=1;
  config.num_threads=1;
  config.keep_alive=0;
  config.uri_path="/benchmark-inprocess";
  config.uri_host="127.0.0.1:8088";
  config.request_counter=0;
  config.num_requests_started=0;
  config.num_requests_complete=0;

  int c;
	while ((c=getopt(argc, argv, ":hvkn:t:c:"))!=-1) {
		switch (c) {
			case 'h':
				show_help();
				return 0;
			case 'v':
				printf("version:    " VERSION "\n");
				printf("build-date: " __DATE__ " " __TIME__ "\n\n");
				return 0;
			case 'k':
				config.keep_alive=1;
				break;
			case 'n':
				config.num_requests=atoi(optarg);
				break;
			case 't':
				config.num_threads=atoi(optarg);
				break;
			case 'c':
				config.num_connections=atoi(optarg);
				break;
			case '?':
				fprintf(stderr, "unkown option: -%c\n\n", optopt);
				show_help();
				return EXIT_FAILURE;
		}
	}

	if ((argc-optind)<1) {
		fprintf(stderr, "%s", "missing url argument\n\n");
		show_help();
		return EXIT_FAILURE;
	} else if ((argc-optind)>1) {
		fprintf(stderr, "%s", "too many arguments\n\n");
		show_help();
		return EXIT_FAILURE;
	}

	if (config.num_requests<1 || config.num_requests>1000000000) nxweb_die("wrong number of requests");
	if (config.num_connections<1 || config.num_connections>1000000 || config.num_connections>config.num_requests) nxweb_die("wrong number of connections");
	if (config.num_threads<1 || config.num_threads>100000 || config.num_threads>config.num_connections) nxweb_die("wrong number of threads");

  config.progress_step=config.num_requests/4;
  if (config.progress_step>50000) config.progress_step=50000;

	if (parse_uri(argv[optind])) nxweb_die("can't parse url");


  // Block signals for all threads
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGPIPE);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGQUIT);
  sigaddset(&set, SIGHUP);
  if (pthread_sigmask(SIG_BLOCK, &set, NULL)) {
    nxweb_log_error("Can't set pthread_sigmask");
    exit(EXIT_FAILURE);
  }

	if(resolve_host(&config.saddr, config.uri_host)) {
    nxweb_log_error("Can't resolve host %s", config.uri_host);
    exit(EXIT_FAILURE);
	}

  snprintf(config.request_data, sizeof(config.request_data),
           "GET %s HTTP/1.1\r\n"
           "Host: %s\r\n"
           "Connection: %s\r\n"
           "\r\n",
           config.uri_path, config.uri_host, config.keep_alive?"keep-alive":"close"
          );
  config.request_length=strlen(config.request_data);

  connection* conn_pool=calloc(config.num_connections, sizeof(connection));
  if (!conn_pool) nxweb_die("can't allocate connection pool");
  connection* cptr=conn_pool;

  thread_config* threads=calloc(config.num_threads, sizeof(thread_config));
  if (!conn_pool) nxweb_die("can't allocate thread pool");

	ev_tstamp ts_start=ev_time();
  int i;
  thread_config* tdata;
  for (i=0; i<config.num_threads; i++) {
    tdata=&threads[i];
    tdata->id=i+1;
    tdata->start_time=ts_start;
    tdata->conns=cptr;
    tdata->num_conn=(config.num_connections-(cptr-conn_pool))/(config.num_threads-i);
    cptr+=tdata->num_conn;

    tdata->loop=ev_loop_new(0);

    int j;
    connection* conn;
    for (j=0; j<tdata->num_conn; j++) {
      conn=&tdata->conns[j];
      conn->tdata=tdata;
      conn->loop=tdata->loop;
      ev_io_init(&conn->watch_write, write_cb, -1, EV_WRITE);
      ev_io_init(&conn->watch_read, read_cb, -1, EV_READ);
      open_socket(conn);
    }

    pthread_create(&threads[i].tid, 0, thread_main, &threads[i]);
    //sleep_ms(10);
  }

  // Unblock signals for the main thread;
  // other threads have inherited sigmask we set earlier
  sigdelset(&set, SIGPIPE); // except SIGPIPE
  if (pthread_sigmask(SIG_UNBLOCK, &set, NULL)) {
    nxweb_log_error("Can't unset pthread_sigmask");
    exit(EXIT_FAILURE);
  }

  int total_success=0;
  int total_fail=0;
  long total_bytes=0;
  long total_overhead=0;
  int total_connect=0;

  for (i=0; i<config.num_threads; i++) {
    pthread_join(threads[i].tid, 0);
    total_success+=threads[i].num_success;
    total_fail+=threads[i].num_fail;
    total_bytes+=threads[i].num_bytes_received;
    total_overhead+=threads[i].num_overhead_received;
    total_connect+=threads[i].num_connect;
  }

  int real_concurrency=0;
  int real_concurrency1=0;
  int real_concurrency1_threshold=config.num_requests/config.num_connections/10;
  if (real_concurrency1_threshold<2) real_concurrency1_threshold=2;
  for (i=0; i<config.num_connections; i++) {
    connection* conn=&conn_pool[i];
    if (conn->success_count) real_concurrency++;
    if (conn->success_count>=real_concurrency1_threshold) real_concurrency1++;
  }

	ev_tstamp ts_end=ev_time();
	ev_tstamp duration=ts_end-ts_start;
	int sec=duration;
	duration=(duration-sec)*1000;
	int millisec=duration;
	duration=(duration-millisec)*1000;
	//int microsec=duration;
	int rps=total_success/(ts_end-ts_start);
	int kbps=(total_bytes+total_overhead) / (ts_end-ts_start) / 1024;
  ev_tstamp avg_req_time=(ts_end-ts_start) * config.num_connections / total_success;

  printf("\nTOTALS:  %d connect, %d requests, %d success, %d fail, %d (%d) real concurrency\n",
         total_connect, total_success+total_fail, total_success, total_fail, real_concurrency, real_concurrency1);
  printf("TRAFFIC: %ld avg bytes, %ld avg overhead, %ld bytes, %ld overhead\n",
         total_bytes/total_success, total_overhead/total_success, total_bytes, total_overhead);
  printf("TIMING:  %d.%03d seconds, %d rps, %d kbps, %.1fms avg req time\n",
         sec, millisec, /*microsec,*/ rps, kbps, (float)(avg_req_time*1000));

  freeaddrinfo(config.saddr);
  free(conn_pool);
  free(threads);

  return EXIT_SUCCESS;
}
