/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Lua sandbox unit tests @file */

#include "hindsight.h"

#include <errno.h>
#include <lauxlib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/times.h>
#include <unistd.h>

#include "hindsight_config.h"
#include "hindsight_sandbox_loader.h"

static bool g_stop = false;

static void stop_signal(int sig)
{
  fprintf(stderr, "stop signal received %d\n", sig);
  signal(SIGINT, SIG_DFL);
  g_stop = true;
}


//static void issue_stop(hs_plugins* plugins)
//{
//  for (int i = 0; i < plugins->cnt; ++i) {
//    hs_stop_sandbox(plugins->list[i]->lsb);
//  }
//}
//

static int file_exists(const char* fn)
{
  FILE* fh = fopen(fn, "r");
  if (fh) {
    fclose(fh);
    return 1;
  }
  return 0;
}


static void init_plugins(hs_plugins* plugins, hindsight_config* cfg)
{
  char fqfn[260];

  plugins->cnt = 0;
  plugins->hs_cfg = cfg;

  if (pthread_mutex_init(&plugins->lock, NULL)) {
    perror("pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }

  if (!hs_get_fqfn(cfg->output_path, "hindsight.cp", fqfn, sizeof(fqfn))) {
    exit(EXIT_FAILURE);
  }
  plugins->cp_values = luaL_newstate();
  if (!plugins->cp_values) {
    fprintf(stderr, "checkpoint luaL_newstate failed\n");
    exit(EXIT_FAILURE);
  } else {
    lua_pushvalue(plugins->cp_values, LUA_GLOBALSINDEX);
    lua_setglobal(plugins->cp_values, "_G");
  }

  if (file_exists(fqfn)) {
    if (luaL_dofile(plugins->cp_values, fqfn)) {
      fprintf(stderr, "Loading %s failed: %s\n", fqfn,
              lua_tostring(plugins->cp_values, -1));
      exit(EXIT_FAILURE);
    }
  }

  plugins->cp_fh = fopen(fqfn, "wb");
  if (!plugins->cp_fh) {
    fprintf(stderr, "%s: ", fqfn);
    perror(NULL);
    exit(EXIT_FAILURE);
  }

  lua_getglobal(plugins->cp_values, "last_output_id");
  if (lua_type(plugins->cp_values, -1) == LUA_TNUMBER) {
    plugins->output_id = (long long)lua_tonumber(plugins->cp_values, 1);
  } else {
    plugins->output_id = 0;
  }
  lua_pop(plugins->cp_values, 1);
  hs_open_output_file(plugins);

  plugins->list = NULL;
  plugins->threads = NULL;
}


static clock_t free_plugins(hs_plugins* plugins)
{
  struct tms tms1;
  void* thread_result;
  for (int i = 0; i < plugins->cnt; ++i) {
    int ret = pthread_join(plugins->threads[i], &thread_result);
    if (ret) {
      perror("pthread_join failed");
    }
  }
  clock_t t = times(&tms1);
  free(plugins->threads);
  plugins->threads = NULL;

  hs_write_checkpoint(plugins);

  for (int i = 0; i < plugins->cnt; ++i) {
    hs_free_plugin(plugins->list[i]);
    free(plugins->list[i]);
  }
  free(plugins->list);
  plugins->list = NULL;

  if (plugins->cp_fh) {
    fclose(plugins->cp_fh);
    plugins->cp_fh = NULL;
  }

  if (plugins->output_fh) {
    fclose(plugins->output_fh);
    plugins->output_fh = NULL;
  }

  if (plugins->cp_values) {
    lua_close(plugins->cp_values);
    plugins->cp_values = NULL;
  }

  pthread_mutex_destroy(&plugins->lock);
  plugins->hs_cfg = NULL;
  plugins->cnt = 0;
  return t;
}


static void output_lua_string(FILE* fh, const char* s)
{
  size_t len = strlen(s);
  for (unsigned i = 0; i < len; ++i) {
    switch (s[i]) {
    case '\n':
      fwrite("\\n", 2, 1, fh);
      break;
    case '\r':
      fwrite("\\r", 2, 1, fh);
      break;
    case '"':
      fwrite("\\\"", 2, 1, fh);
      break;
    case '\\':
      fwrite("\\\\", 2, 1, fh);
      break;
    default:
      fwrite(s + i, 1, 1, fh);
      break;
    }
  }
}


void hs_free_plugin(hs_plugin* p)
{
  if (!p) return;

  char* e = lsb_destroy(p->lsb, NULL);
  if (e) {
    fprintf(stderr, "lsb_destroy() received: %s\n", e);
    free(e);
  }
  p->lsb = NULL;
  p->plugins = NULL;

  free(p->filename);
  p->filename = NULL;

  free(p->state);
  p->state = NULL;

  free(p->cp_string);
  p->cp_string = NULL;
}


bool hs_get_fqfn(const char* path,
                 const char* name,
                 char* fqfn,
                 size_t fqfn_len)
{
  int ret = snprintf(fqfn, fqfn_len, "%s/%s", path, name);
  if (ret < 0 || ret > (int)fqfn_len - 1) {
    fprintf(stderr, "%s: fully qualiifed path is greater than %zu\n",
            name, fqfn_len);
    return false;
  }

  return true;
}


void hs_write_checkpoint(hs_plugins* plugins)
{
  if (fseek(plugins->cp_fh, 0, SEEK_SET)) {
    fprintf(stderr, "checkpoint fseek() error: %d\n", ferror(plugins->cp_fh));
    return;
  }

  fprintf(plugins->cp_fh, "last_output_id = %lld\n", plugins->output_id);
  for (int i = 0; i < plugins->cnt; ++i) {
    hs_plugin* p = plugins->list[i];
    if (p->cp_string) {
      fprintf(plugins->cp_fh, "_G[\"%s\"] = \"", p->filename);
      output_lua_string(plugins->cp_fh, p->cp_string);
      fwrite("\"\n", 2, 1, plugins->cp_fh);
    } else if (p->cp_offset) {
      fprintf(plugins->cp_fh, "_G[\"%s\"] = %lld\n", p->filename, p->cp_offset);
    }
  }
  fflush(plugins->cp_fh);
}


void hs_open_output_file(hs_plugins* plugins)
{
  static char fqfn[260];
  if (plugins->output_fh) {
    fclose(plugins->output_fh);
    plugins->output_fh = NULL;
  }
  int ret = snprintf(fqfn, sizeof(fqfn), "%s/%lld.log",
                     plugins->hs_cfg->output_path,
                     plugins->output_id);
  if (ret < 0 || ret > (int)sizeof(fqfn) - 1) {
    fprintf(stderr, "output filename exceeds %zu\n", sizeof(fqfn));
    exit(EXIT_FAILURE);
  }
  plugins->output_fh = fopen(fqfn, "ab+");
  if (!plugins->output_fh) {
    fprintf(stderr, "%s: ", fqfn);
    perror(NULL);
    exit(EXIT_FAILURE);
  } else {
    fseek(plugins->output_fh, 0, SEEK_END);
    plugins->output_offset = ftell(plugins->output_fh);
  }
}


int main(int argc, char* argv[])
{
  if (argc != 2) {
    fprintf(stderr, "usage: %s <cfg>\n", argv[0]);
    return EXIT_FAILURE;
  }

  hindsight_config cfg;
  if (hs_load_config(argv[1], &cfg)) {
    return EXIT_FAILURE;
  }
  if (cfg.mode != HS_MODE_INPUT) {
    hs_free_config(&cfg);
    fprintf(stderr, "only 'input' mode has been implemented\n");
    return EXIT_FAILURE;
  }

  signal(SIGINT, stop_signal);

  hs_plugins plugins;
  init_plugins(&plugins, &cfg);
  hs_load_sandboxes(cfg.run_path, &cfg, &plugins);

  struct tms tms1;

  clock_t t = times(&tms1);
  // todo uncomment after benchmarking and remove the clock_t return from free_plugins
//  int cnt = 0;
//  while (!g_stop) {
//    if (cnt % 60 == 0) {
//      fprintf(stderr, "todo scan the load directory\n");
//      cnt = 1;
//    } else {
//      ++cnt;
//    }
//    sleep(1);
//  }
//  issue_stop(&plugins);
  clock_t t1 = free_plugins(&plugins);
  t = t1 - t;
  fprintf(stderr, "execution time %g seconds\n", ((float)t) / sysconf(_SC_CLK_TCK));
  hs_free_config(&cfg);
  return 0;
}