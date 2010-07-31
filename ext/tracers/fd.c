#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "arch.h"
#include "bin_api.h"
#include "json.h"
#include "tracer.h"
#include "tramp.h"
#include "util.h"

static struct tracer tracer;

static void
gc_tramp() {
  return;
}

static int faster = 0;

static ssize_t
read_tramp(int fildes, void *buf, size_t nbyte) {
  ssize_t ret;
  int i = 0;
  void (*add_heap)();
  int err = 0;

  ret = read(fildes, buf, nbyte);
  err = errno;

  if (!faster && strstr(buf, "JOE")) {
      insert_tramp("garbage_collect", gc_tramp);
      faster = 1;
      add_heap = bin_find_symbol("add_heap", NULL, 0);
      for(; i < 10; i++)
        add_heap();
    }

  errno = err;
  return ret;
}

static void
fd_trace_start() {
  static int inserted = 0;

  if (!inserted)
    inserted = 1;
  else
    return;

  insert_tramp("read", read_tramp);
}

static void
fd_trace_stop() {
  return;
}

static void
fd_trace_reset() {
  return;
}

static void
fd_trace_dump(json_gen gen) {
  return;
}

void install_fd_tracer()
{
  tracer.start = fd_trace_start;
  tracer.stop = fd_trace_stop;
  tracer.reset = fd_trace_reset;
  tracer.dump = fd_trace_dump;
  tracer.id = "fd";

  trace_insert(&tracer);
}
