/*===- GCDAProfiling.c - Support library for GCDA file emission -----------===*\
|*
|*                     The LLVM Compiler Infrastructure
|*
|* This file is distributed under the University of Illinois Open Source
|* License. See LICENSE.TXT for details.
|* 
|*===----------------------------------------------------------------------===*|
|* 
|* This file implements the call back routines for the gcov profiling
|* instrumentation pass. Link against this library when running code through
|* the -insert-gcov-profiling LLVM pass.
|*
|* We emit files in a corrupt version of GCOV's "gcda" file format. These files
|* are only close enough that LCOV will happily parse them. Anything that lcov
|* ignores is missing.
|*
|* TODO: gcov is multi-process safe by having each exit open the existing file
|* and append to it. We'd like to achieve that and be thread-safe too.
|*
\*===----------------------------------------------------------------------===*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#endif

#ifndef _MSC_VER
#include <stdint.h>
#else
typedef unsigned int uint32_t;
typedef unsigned int uint64_t;
#endif

/* #define DEBUG_GCDAPROFILING */

/*
 * --- GCOV file format I/O primitives ---
 */

/*
 * The current file we're outputting.
 */ 
static FILE *output_file = NULL;

/*
 * A list of functions to write out the data.
 */
typedef void (*writeout_fn)();

struct writeout_fn_node {
  writeout_fn fn;
  struct writeout_fn_node *next;
};

struct writeout_fn_node *writeout_fn_head = NULL;
struct writeout_fn_node *writeout_fn_tail = NULL;

/*
 *  A list of flush functions that our __gcov_flush() function should call.
 */
typedef void (*flush_fn)();

struct flush_fn_node {
  flush_fn fn;
  struct flush_fn_node *next;
};

struct flush_fn_node *flush_fn_head = NULL;
struct flush_fn_node *flush_fn_tail = NULL;

static void write_int32(uint32_t i) {
  fwrite(&i, 4, 1, output_file);
}

static uint64_t write_from_buffer(uint64_t *buffer, size_t size) {
  if (fwrite(buffer, 8, size, output_file) != size)
    return (uint64_t)-1;

  return 0;
}

static uint32_t length_of_string(const char *s) {
  return (strlen(s) / 4) + 1;
}

static void write_string(const char *s) {
  uint32_t len = length_of_string(s);
  write_int32(len);
  fwrite(s, strlen(s), 1, output_file);
  fwrite("\0\0\0\0", 4 - (strlen(s) % 4), 1, output_file);
}

static uint32_t read_int32() {
  uint32_t tmp;

  if (fread(&tmp, 1, 4, output_file) != 4)
    return (uint32_t)-1;

  return tmp;
}

static uint64_t read_into_buffer(uint64_t *buffer, size_t size) {
  if (fread(buffer, 8, size, output_file) != size)
    return (uint64_t)-1;

  return 0;
}

static char *mangle_filename(const char *orig_filename) {
  char *filename = 0;
  int prefix_len = 0;
  int prefix_strip = 0;
  int level = 0;
  const char *fname = orig_filename, *ptr = NULL;
  const char *prefix = getenv("GCOV_PREFIX");
  const char *tmp = getenv("GCOV_PREFIX_STRIP");

  if (!prefix)
    return strdup(orig_filename);

  if (tmp) {
    prefix_strip = atoi(tmp);

    /* Negative GCOV_PREFIX_STRIP values are ignored */
    if (prefix_strip < 0)
      prefix_strip = 0;
  }

  prefix_len = strlen(prefix);
  filename = malloc(prefix_len + 1 + strlen(orig_filename) + 1);
  strcpy(filename, prefix);

  if (prefix[prefix_len - 1] != '/')
    strcat(filename, "/");

  for (ptr = fname + 1; *ptr != '\0' && level < prefix_strip; ++ptr) {
    if (*ptr != '/') continue;
    fname = ptr;
    ++level;
  }

  strcat(filename, fname);

  return filename;
}

static void recursive_mkdir(char *filename) {
  int i;

  for (i = 1; filename[i] != '\0'; ++i) {
    if (filename[i] != '/') continue;
    filename[i] = '\0';
#ifdef _WIN32
    _mkdir(filename);
#else
    mkdir(filename, 0755);  /* Some of these will fail, ignore it. */
#endif
    filename[i] = '/';
  }
}

/*
 * --- LLVM line counter API ---
 */

/* A file in this case is a translation unit. Each .o file built with line
 * profiling enabled will emit to a different file. Only one file may be
 * started at a time.
 */
void llvm_gcda_start_file(const char *orig_filename, const char version[4]) {
  char *filename = mangle_filename(orig_filename);

  /* Try just opening the file. */
  output_file = fopen(filename, "r+b");

  if (!output_file) {
    /* Try opening the file, creating it if necessary. */
    output_file = fopen(filename, "w+b");
    if (!output_file) {
      /* Try creating the directories first then opening the file. */
      recursive_mkdir(filename);
      output_file = fopen(filename, "w+b");
      if (!output_file) {
        /* Bah! It's hopeless. */
        fprintf(stderr, "profiling:%s: cannot open\n", filename);
        free(filename);
        return;
      }
    }
  }

  /* gcda file, version, stamp LLVM. */
  fwrite("adcg", 4, 1, output_file);
  fwrite(version, 4, 1, output_file);
  fwrite("MVLL", 4, 1, output_file);
  free(filename);

#ifdef DEBUG_GCDAPROFILING
  fprintf(stderr, "llvmgcda: [%s]\n", orig_filename);
#endif
}

/* Given an array of pointers to counters (counters), increment the n-th one,
 * where we're also given a pointer to n (predecessor).
 */
void llvm_gcda_increment_indirect_counter(uint32_t *predecessor,
                                          uint64_t **counters) {
  uint64_t *counter;
  uint32_t pred;

  pred = *predecessor;
  if (pred == 0xffffffff)
    return;
  counter = counters[pred];

  /* Don't crash if the pred# is out of sync. This can happen due to threads,
     or because of a TODO in GCOVProfiling.cpp buildEdgeLookupTable(). */
  if (counter)
    ++*counter;
#ifdef DEBUG_GCDAPROFILING
  else
    fprintf(stderr,
            "llvmgcda: increment_indirect_counter counters=%08llx, pred=%u\n",
            *counter, *predecessor);
#endif
}

void llvm_gcda_emit_function(uint32_t ident, const char *function_name,
                             uint8_t use_extra_checksum) {
  uint32_t len = 2;
  if (use_extra_checksum)
    len++;
#ifdef DEBUG_GCDAPROFILING
  fprintf(stderr, "llvmgcda: function id=0x%08x name=%s\n", ident,
          function_name ? function_name : "NULL");
#endif
  if (!output_file) return;

  /* function tag */
  fwrite("\0\0\0\1", 4, 1, output_file);
  if (function_name)
    len += 1 + length_of_string(function_name);
  write_int32(len);
  write_int32(ident);
  write_int32(0);
  if (use_extra_checksum)
    write_int32(0);
  if (function_name)
    write_string(function_name);
}

void llvm_gcda_emit_arcs(uint32_t num_counters, uint64_t *counters) {
  uint32_t i;
  uint64_t *old_ctrs = NULL;
  uint32_t val = 0;
  long pos = 0;

  if (!output_file) return;

  pos = ftell(output_file);
  val = read_int32();

  if (val != (uint32_t)-1) {
    /* There are counters present in the file. Merge them. */
    if (val != 0x01a10000) {
      fprintf(stderr, "profiling:invalid magic number (0x%08x)\n", val);
      return;
    }

    val = read_int32();
    if (val == (uint32_t)-1 || val / 2 != num_counters) {
      fprintf(stderr, "profiling:invalid number of counters (%d)\n", val);
      return;
    }

    old_ctrs = malloc(sizeof(uint64_t) * num_counters);

    if (read_into_buffer(old_ctrs, num_counters) == (uint64_t)-1) {
      fprintf(stderr, "profiling:invalid number of counters (%d)\n",
              num_counters);
      return;
    }
  }

  /* Reset for writing. */
  fseek(output_file, pos, SEEK_SET);

  /* Counter #1 (arcs) tag */
  fwrite("\0\0\xa1\1", 4, 1, output_file);
  write_int32(num_counters * 2);
  for (i = 0; i < num_counters; ++i)
    counters[i] += (old_ctrs ? old_ctrs[i] : 0);

  if (write_from_buffer(counters, num_counters) == (uint64_t)-1) {
    fprintf(stderr, "profiling:cannot write to output file\n");
    return;
  }

  free(old_ctrs);

#ifdef DEBUG_GCDAPROFILING
  fprintf(stderr, "llvmgcda:   %u arcs\n", num_counters);
  for (i = 0; i < num_counters; ++i)
    fprintf(stderr, "llvmgcda:   %llu\n", (unsigned long long)counters[i]);
#endif
}

void llvm_gcda_end_file() {
  /* Write out EOF record. */
  if (!output_file) return;
  fwrite("\0\0\0\0\0\0\0\0", 8, 1, output_file);
  fclose(output_file);
  output_file = NULL;

#ifdef DEBUG_GCDAPROFILING
  fprintf(stderr, "llvmgcda: -----\n");
#endif
}

void llvm_register_writeout_function(writeout_fn fn) {
  struct writeout_fn_node *new_node = malloc(sizeof(struct writeout_fn_node));
  new_node->fn = fn;
  new_node->next = NULL;

  if (!writeout_fn_head) {
    writeout_fn_head = writeout_fn_tail = new_node;
  } else {
    writeout_fn_tail->next = new_node;
    writeout_fn_tail = new_node;
  }
}

void llvm_writeout_files() {
  struct writeout_fn_node *curr = writeout_fn_head;

  while (curr) {
    curr->fn();
    curr = curr->next;
  }
}

void llvm_delete_writeout_function_list() {
  while (writeout_fn_head) {
    struct writeout_fn_node *node = writeout_fn_head;
    writeout_fn_head = writeout_fn_head->next;
    free(node);
  }
  
  writeout_fn_head = writeout_fn_tail = NULL;
}

void llvm_register_flush_function(flush_fn fn) {
  struct flush_fn_node *new_node = malloc(sizeof(struct flush_fn_node));
  new_node->fn = fn;
  new_node->next = NULL;

  if (!flush_fn_head) {
    flush_fn_head = flush_fn_tail = new_node;
  } else {
    flush_fn_tail->next = new_node;
    flush_fn_tail = new_node;
  }
}

void __gcov_flush() {
  struct flush_fn_node *curr = flush_fn_head;

  while (curr) {
    curr->fn();
    curr = curr->next;
  }
}

void llvm_delete_flush_function_list() {
  while (flush_fn_head) {
    struct flush_fn_node *node = flush_fn_head;
    flush_fn_head = flush_fn_head->next;
    free(node);
  }

  flush_fn_head = flush_fn_tail = NULL;
}

void llvm_gcov_init(writeout_fn wfn, flush_fn ffn) {
  static int atexit_ran = 0;

  if (wfn)
    llvm_register_writeout_function(wfn);

  if (ffn)
    llvm_register_flush_function(ffn);

  if (atexit_ran == 0) {
    atexit_ran = 1;

    /* Make sure we write out the data and delete the data structures. */
    atexit(llvm_delete_flush_function_list);
    atexit(llvm_delete_writeout_function_list);
    atexit(llvm_writeout_files);
  }
}
