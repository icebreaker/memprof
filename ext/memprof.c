#include <ruby.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sysexits.h>

#include <st.h>
#include <intern.h>
#include <node.h>

#include "arch.h"
#include "bin_api.h"
#include "tracer.h"
#include "tramp.h"
#include "util.h"

/*
 * bleak_house stuff
 */
static VALUE eUnsupported;
static int track_objs = 0;
static int memprof_started = 0;
static st_table *objs = NULL;

/*
 * stuff needed for heap dumping
 */
static VALUE (*rb_classname)(VALUE);
static RUBY_DATA_FUNC *rb_bm_mark;
static RUBY_DATA_FUNC *rb_blk_free;
static RUBY_DATA_FUNC *rb_thread_mark;
struct memprof_config memprof_config;

/*
 * memprof config struct init
 */
static void init_memprof_config_base();
static void init_memprof_config_extended();

struct obj_track {
  VALUE obj;
  char *source;
  int line;
  int len;
  struct timeval time[];
};

static VALUE gc_hook;
static void **ptr_to_rb_mark_table_add_filename = NULL;
static void (*rb_mark_table_add_filename)(char*);
static void (*rb_add_freelist)(VALUE);

static void
init_memprof_config_base() {
  memset(&memprof_config, 0, sizeof(memprof_config));
  memprof_config.offset_heaps_slot_limit = SIZE_MAX;
  memprof_config.offset_heaps_slot_slot = SIZE_MAX;
  memprof_config.pagesize = getpagesize();
  assert(memprof_config.pagesize);
}

static void
init_memprof_config_extended() {
  /* If we don't have add_freelist, find the functions it gets inlined into */
  memprof_config.add_freelist               = bin_find_symbol("add_freelist", NULL, 0);

  /*
   * Sometimes gc_sweep gets inlined in garbage_collect
   * (e.g., on REE it gets inlined into garbage_collect_0).
   */
  if (memprof_config.add_freelist == NULL) {
    memprof_config.gc_sweep                 = bin_find_symbol("gc_sweep",
                                                &memprof_config.gc_sweep_size, 0);
    if (memprof_config.gc_sweep == NULL)
      memprof_config.gc_sweep               = bin_find_symbol("garbage_collect_0",
                                                &memprof_config.gc_sweep_size, 0);
    if (memprof_config.gc_sweep == NULL)
      memprof_config.gc_sweep               = bin_find_symbol("garbage_collect",
                                                &memprof_config.gc_sweep_size, 0);

    memprof_config.finalize_list            = bin_find_symbol("finalize_list",
                                                &memprof_config.finalize_list_size, 0);
    memprof_config.rb_gc_force_recycle      = bin_find_symbol("rb_gc_force_recycle",
                                                &memprof_config.rb_gc_force_recycle_size, 0);
    memprof_config.freelist                 = bin_find_symbol("freelist", NULL, 0);
  }

  memprof_config.classname                  = bin_find_symbol("classname", NULL, 0);
  memprof_config.bm_mark                    = bin_find_symbol("bm_mark", NULL, 0);
  memprof_config.blk_free                   = bin_find_symbol("blk_free", NULL, 0);
  memprof_config.thread_mark                = bin_find_symbol("thread_mark", NULL, 0);
  memprof_config.rb_mark_table_add_filename = bin_find_symbol("rb_mark_table_add_filename", NULL, 0);

  /* Stuff for dumping the heap */
  memprof_config.heaps                      = bin_find_symbol("heaps", NULL, 0);
  memprof_config.heaps_used                 = bin_find_symbol("heaps_used", NULL, 0);
  memprof_config.finalizer_table            = bin_find_symbol("finalizer_table", NULL, 0);

#ifdef sizeof__RVALUE
  memprof_config.sizeof_RVALUE              = sizeof__RVALUE;
#else
  memprof_config.sizeof_RVALUE              = bin_type_size("RVALUE");
#endif
#ifdef sizeof__heaps_slot
  memprof_config.sizeof_heaps_slot          = sizeof__heaps_slot;
#else
  memprof_config.sizeof_heaps_slot          = bin_type_size("heaps_slot");
#endif
#ifdef offset__heaps_slot__limit
  memprof_config.offset_heaps_slot_limit    = offset__heaps_slot__limit;
#else
  memprof_config.offset_heaps_slot_limit    = bin_type_member_offset("heaps_slot", "limit");
#endif
#ifdef offset__heaps_slot__slot
  memprof_config.offset_heaps_slot_slot     = offset__heaps_slot__slot;
#else
  memprof_config.offset_heaps_slot_slot     = bin_type_member_offset("heaps_slot", "slot");
#endif
#ifdef offset__BLOCK__body
  memprof_config.offset_BLOCK_body          = offset__BLOCK__body;
#else
  memprof_config.offset_BLOCK_body          = bin_type_member_offset("BLOCK", "body");
#endif
#ifdef offset__BLOCK__var
  memprof_config.offset_BLOCK_var           = offset__BLOCK__var;
#else
  memprof_config.offset_BLOCK_var           = bin_type_member_offset("BLOCK", "var");
#endif
#ifdef offset__BLOCK__cref
  memprof_config.offset_BLOCK_cref          = offset__BLOCK__cref;
#else
  memprof_config.offset_BLOCK_cref          = bin_type_member_offset("BLOCK", "cref");
#endif
#ifdef offset__BLOCK__prev
  memprof_config.offset_BLOCK_prev          = offset__BLOCK__prev;
#else
  memprof_config.offset_BLOCK_prev          = bin_type_member_offset("BLOCK", "prev");
#endif
#ifdef offset__BLOCK__self
  memprof_config.offset_BLOCK_self          = offset__BLOCK__self;
#else
  memprof_config.offset_BLOCK_self          = bin_type_member_offset("BLOCK", "self");
#endif
#ifdef offset__BLOCK__klass
  memprof_config.offset_BLOCK_klass         = offset__BLOCK__klass;
#else
  memprof_config.offset_BLOCK_klass         = bin_type_member_offset("BLOCK", "klass");
#endif
#ifdef offset__BLOCK__orig_thread
  memprof_config.offset_BLOCK_orig_thread   = offset__BLOCK__orig_thread;
#else
  memprof_config.offset_BLOCK_orig_thread   = bin_type_member_offset("BLOCK", "orig_thread");
#endif
#ifdef offset__BLOCK__wrapper
  memprof_config.offset_BLOCK_wrapper       = offset__BLOCK__wrapper;
#else
  memprof_config.offset_BLOCK_wrapper       = bin_type_member_offset("BLOCK", "wrapper");
#endif
#ifdef offset__BLOCK__block_obj
  memprof_config.offset_BLOCK_block_obj     = offset__BLOCK__block_obj;
#else
  memprof_config.offset_BLOCK_block_obj     = bin_type_member_offset("BLOCK", "block_obj");
#endif
#ifdef offset__BLOCK__scope
  memprof_config.offset_BLOCK_scope         = offset__BLOCK__scope;
#else
  memprof_config.offset_BLOCK_scope         = bin_type_member_offset("BLOCK", "scope");
#endif
#ifdef offset__BLOCK__dyna_vars
  memprof_config.offset_BLOCK_dyna_vars     = offset__BLOCK__dyna_vars;
#else
  memprof_config.offset_BLOCK_dyna_vars     = bin_type_member_offset("BLOCK", "dyna_vars");
#endif
#ifdef offset__METHOD__klass
  memprof_config.offset_METHOD_klass        = offset__METHOD__klass;
#else
  memprof_config.offset_METHOD_klass        = bin_type_member_offset("METHOD", "klass");
#endif
#ifdef offset__METHOD__rklass
  memprof_config.offset_METHOD_rklass       = offset__METHOD__rklass;
#else
  memprof_config.offset_METHOD_rklass       = bin_type_member_offset("METHOD", "rklass");
#endif
#ifdef offset__METHOD__recv
  memprof_config.offset_METHOD_recv         = offset__METHOD__recv;
#else
  memprof_config.offset_METHOD_recv         = bin_type_member_offset("METHOD", "recv");
#endif
#ifdef offset__METHOD__id
  memprof_config.offset_METHOD_id           = offset__METHOD__id;
#else
  memprof_config.offset_METHOD_id           = bin_type_member_offset("METHOD", "id");
#endif
#ifdef offset__METHOD__oid
  memprof_config.offset_METHOD_oid          = offset__METHOD__oid;
#else
  memprof_config.offset_METHOD_oid          = bin_type_member_offset("METHOD", "oid");
#endif
#ifdef offset__METHOD__body
  memprof_config.offset_METHOD_body         = offset__METHOD__body;
#else
  memprof_config.offset_METHOD_body         = bin_type_member_offset("METHOD", "body");
#endif

  int heap_errors_printed = 0;

  if (memprof_config.heaps == NULL)
    heap_errors_printed += fprintf(stderr,
      "Failed to locate heaps\n");
  if (memprof_config.heaps_used == NULL)
    heap_errors_printed += fprintf(stderr,
      "Failed to locate heaps_used\n");
  if (memprof_config.sizeof_RVALUE == 0)
    heap_errors_printed += fprintf(stderr,
      "Failed to determine sizeof(RVALUE)\n");
  if (memprof_config.sizeof_heaps_slot == 0)
    heap_errors_printed += fprintf(stderr,
      "Failed to determine sizeof(heaps_slot)\n");
  if (memprof_config.offset_heaps_slot_limit == SIZE_MAX)
    heap_errors_printed += fprintf(stderr,
      "Failed to determine offset of heaps_slot->limit\n");
  if (memprof_config.offset_heaps_slot_slot == SIZE_MAX)
    heap_errors_printed += fprintf(stderr,
      "Failed to determine offset of heaps_slot->slot\n");

  if (heap_errors_printed)
    fprintf(stderr, "You won't be able to dump your heap!\n");

  int errors_printed = 0;

  /* If we can't find add_freelist, we need to make sure we located the functions that it gets inlined into. */
  if (memprof_config.add_freelist == NULL) {
    if (memprof_config.gc_sweep == NULL) {
      errors_printed += fprintf(stderr,
        "Failed to locate add_freelist (it's probably inlined, but we couldn't find it there either!)\n");
      errors_printed += fprintf(stderr,
        "Failed to locate gc_sweep, garbage_collect_0, or garbage_collect\n");
    }
    if (memprof_config.gc_sweep_size == 0)
      errors_printed += fprintf(stderr,
        "Failed to determine the size of gc_sweep/garbage_collect_0/garbage_collect: %zd\n",
        memprof_config.gc_sweep_size);
    if (memprof_config.finalize_list == NULL)
      errors_printed += fprintf(stderr,
        "Failed to locate finalize_list\n");
    if (memprof_config.finalize_list_size == 0)
      errors_printed += fprintf(stderr,
        "Failed to determine the size of finalize_list: %zd\n",
        memprof_config.finalize_list_size);
    if (memprof_config.rb_gc_force_recycle == NULL)
      errors_printed += fprintf(stderr,
        "Failed to locate rb_gc_force_recycle\n");
    if (memprof_config.rb_gc_force_recycle_size == 0)
      errors_printed += fprintf(stderr,
        "Failed to determine the size of rb_gc_force_recycle: %zd\n",
        memprof_config.rb_gc_force_recycle_size);
    if (memprof_config.freelist == NULL)
      errors_printed += fprintf(stderr,
        "Failed to locate freelist\n");
  }

  if (memprof_config.classname == NULL)
    errors_printed += fprintf(stderr,
      "Failed to locate classname\n");

  if (errors_printed) {
    VALUE ruby_build_info = rb_eval_string("require 'rbconfig'; RUBY_DESCRIPTION + '\n' + RbConfig::CONFIG['CFLAGS'];");
    /* who knows what could happen */
    if (TYPE(ruby_build_info) == T_STRING)
      fprintf(stderr, "%s\n", StringValuePtr(ruby_build_info));

    fprintf(stderr, "\nTry installing debug symbols (apt-get install libruby1.8-dbg or similar), or using a "
                    "Ruby built with RVM (http://rvm.beginrescueend.com/)\n\n");

    errx(EX_SOFTWARE, "If that doesn't work, please email this output to bugs@memprof.com");
  }
}

void
Init_memprof()
{
  objs = st_init_numtable();
  init_memprof_config_base();
  bin_init();
  init_memprof_config_extended();
  create_tramp_table();

  install_fd_tracer();
  trace_invoke_all(TRACE_START);
  return;
}
