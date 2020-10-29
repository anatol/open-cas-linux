/*
 * Copyright (C) 2020 Twitter, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#define KMSG_DEV_PATH "/dev/kmsg"
static int _kmsg_fd;

#define OPENCAS_CONFIG_FILE "/etc/opencas/opencas.conf"

const char *generator_dir;

static void log_init(void) {
  _kmsg_fd = open(KMSG_DEV_PATH, O_WRONLY | O_NOCTTY);
}

static void log_exit(void) {
  if (_kmsg_fd != -1) (void)close(_kmsg_fd);
}

__attribute__((format(printf, 1, 2))) static void print_err(const char *format,
                                                            ...) {
  int n;
  va_list ap;
  /* +3 for '<n>' where n is the log level and +19 for opencas-generator: "
   * prefix */
  char message[PATH_MAX + 22];

  if (_kmsg_fd == -1) return;

  snprintf(message, 23, "<%d>opencas-generator: ", LOG_ERR);

  va_start(ap, format);
  n = vsnprintf(message + 22, PATH_MAX, format, ap);
  va_end(ap);

  if (n < 0 || ((unsigned)n + 1 > PATH_MAX)) return;

  /* The n+23: +22 for "<n>opencas-generator: " prefix and +1 for '\0' suffix */
  write(_kmsg_fd, message, n + 23);
}

#define NETWORK_OPTION "_netdev"

/* indicates if the underlying device is a remote one */
#define FLAG_NETWORK (1 << 0)

struct opencas_cache {
  int id;
  char *device;
  int flags;
};

#define MAX_CACHES 50
int caches_num;
struct opencas_cache caches[MAX_CACHES];

static bool parse_cache(char *line) {
  struct opencas_cache *c = &caches[caches_num];
  char *ptr;

  char *id = strtok_r(line, " \t", &ptr);
  char *device = strtok_r(NULL, " \t", &ptr);
  char *mode = strtok_r(NULL, " \t", &ptr);
  char *options = strtok_r(NULL, " \t", &ptr);

  c->id = atoi(id);
  c->device = strdup(device);
  if (options) {
    /* check if options contains _netdev option */
    char *optr;
    char *o = strtok_r(options, ",", &optr);
    while (o) {
      if (strcmp(o, "_netdev") == 0) {
        c->flags |= FLAG_NETWORK;
        break;
      }
      o = strtok_r(NULL, ",", &optr);
    }
  }

  caches_num++;

  return true;
}

struct opencas_device {
  int core_id;
  int cache_id;
  char *device;
  int flags;
};

#define MAX_DEVICES 600
int devices_num;
struct opencas_device devices[MAX_DEVICES];

static bool parse_device(char *line) {
  struct opencas_device *d = &devices[devices_num];
  char *ptr;

  char *cache_id = strtok_r(line, " \t", &ptr);
  char *core_id = strtok_r(NULL, " \t", &ptr);
  char *device = strtok_r(NULL, " \t", &ptr);
  char *options = strtok_r(NULL, " \t", &ptr);

  d->core_id = atoi(core_id);
  d->cache_id = atoi(cache_id);
  d->device = strdup(device);
  if (options) {
    /* check if options contains _netdev option */
    char *optr;
    char *o = strtok_r(options, ",", &optr);
    while (o) {
      if (strcmp(o, "_netdev") == 0) {
        d->flags |= FLAG_NETWORK;
        break;
      }
      o = strtok_r(NULL, ",", &optr);
    }
  }

  devices_num++;

  return true;
}

#define STAGE_PROLOG 0
#define STAGE_PARSE_CACHES 1
#define STAGE_PARSE_DEVICES 2

static bool parse_config() {
  /* parse opencas.conf */
  FILE *cfg = fopen(OPENCAS_CONFIG_FILE, "r");
  if (!cfg) {
    print_err("cannot open file %s\n", OPENCAS_CONFIG_FILE);
    return false;
  }

  char *buffer = NULL;
  size_t buffer_len = 0;
  ssize_t len;
  int stage = STAGE_PROLOG;
  while ((len = getline(&buffer, &buffer_len, cfg)) != -1) {
    char *line = buffer;

    /* trim the trailing comments */
    char *comment = strchr(line, '#');
    if (comment) {
      len = comment - line;
      *comment = '\0';
    }

    /* trim whitespaces */
    while (len && isspace(line[len - 1])) line[--len] = '\0';
    while (len && isspace(*line)) ++line, --len;

    if (len == 0) continue;

    /* go to line started with [caches] */
    if (strcmp(line, "[caches]") == 0) {
      stage = STAGE_PARSE_CACHES;
      continue;
    }

    if (strcmp(line, "[cores]") == 0) {
      stage = STAGE_PARSE_DEVICES;
      continue;
    }

    if (stage == STAGE_PARSE_CACHES) {
      parse_cache(line);
    } else if (stage == STAGE_PARSE_DEVICES) {
      parse_device(line);
    }
  }

  free(buffer);
  fclose(cfg);

  return true;
}

/* Function to replace a string with another string */
char *str_replace(const char *s, const char *old_word, const char *new_word) {
  char *result;
  int i, cnt = 0;
  int new_len = strlen(new_word);
  int old_len = strlen(old_word);

  /* Counting the number of times old word occur in the string */
  for (i = 0; s[i] != '\0'; i++) {
    if (strstr(&s[i], old_word) == &s[i]) {
      cnt++;

      /* Jumping to index after the old word */
      i += old_len - 1;
    }
  }

  /* Making new string of enough length */
  result = (char *)malloc(i + cnt * (new_len - old_len) + 1);

  i = 0;
  while (*s) {
    /* compare the substring with the result */
    if (strstr(s, old_word) == s) {
      strcpy(&result[i], new_word);
      i += new_len;
      s += old_len;
    } else
      result[i++] = *s++;
  }

  result[i] = '\0';
  return result;
}

/* Input looks like /dev/mapper/luks-nvme0n1p1 and output is like
 * dev-mapper-luks\x2nvme0n1p1
 */
static char *build_devname(char *dev) {
  char *device = dev;
  if (*device != '/') {
    print_err("device %s does not start with '/'\n", device);
    return NULL;
  }

  device++; /* remove leading slash */
  device = str_replace(device, "-", "\\x2d");
  if (!device) {
    print_err("cannot build a devname for %s", dev);
    return NULL;
  }

  /* replace / with - */
  char *p = device;
  while (*p != '\0') {
    if (*p == '/') *p = '-';
    p++;
  }

  return device;
}

static bool generate_units() {
  int ret;

  for (int i = 0; i < devices_num; i++) {
    struct opencas_device *d = &devices[i];
    struct opencas_cache *c = NULL;

    for (int j = 0; j < caches_num; j++) {
      if (d->cache_id == caches[j].id) {
        c = &caches[j];
        break;
      }
    }

    if (!c) {
      print_err(
          "core device with id %d points to non-existing cache with id %d\n",
          d->core_id, d->cache_id);
      return false;
    }

    bool remote = d->flags & FLAG_NETWORK;
    if ((c->flags & FLAG_NETWORK) && !remote) {
      print_err(
          "a non-remote core device %d depends on cache (%d) with _netdev "
          "option\n",
          d->core_id, d->cache_id);
      return false;
    }

    char unitname[PATH_MAX];
    snprintf(unitname, PATH_MAX, "opencas@opencas%d-%d.service", d->cache_id,
             d->core_id);

    char unitpath[PATH_MAX];
    snprintf(unitpath, PATH_MAX, "%s/%s", generator_dir, unitname);

    int fd = creat(unitpath, 0644);
    if (fd < 0) {
      print_err("unable to create file %s\n", unitpath);
      return false;
    }

    char *cache_devname = build_devname(c->device);
    if (!cache_devname) return false;
    char *core_devname = build_devname(d->device);
    if (!core_devname) return false;

    dprintf(
        fd,
        "# Automatically generated by opencas-activation-generator\n"
        "#\n"
        "# This unit is responsible for direct activation of OpenCAS volumes\n"
        "[Unit]\n"
        "Description=OpenCAS Setup for %%I\n"
        "Documentation=man:opencas(5) man:opencas-activation-generator(8) "
        "man:opencas@.service(8)\n"
        "SourcePath=%s\n"
        "DefaultDependencies=no\n"
        "Before=blockdev@dev-opencas%d\\x2%d.target\n"
        "Wants=blockdev@dev-opencas%d\\x2%d.target\n"
        "Conflicts=umount.target\n"
        "Before=umount.target\n"
        "Before=%s\n"
        "Before=%s\n"
        "BindsTo=%s.device %s.device\n" /* cache device and core device */
        "After=%s.device %s.device\n"
        "\n"
        "[Service]\n"
        "Type=oneshot\n"
        "RemainAfterExit=yes\n"
        "ExecStart=/usr/sbin/casadm --add-core --cache-id %d --core-device %s "
        "--core-id %d\n"
        "ExecStop=/usr/sbin/casadm --remove-core --cache-id %d --core-id %d\n",
        OPENCAS_CONFIG_FILE, d->cache_id, d->core_id, d->cache_id, d->core_id,
        remote ? "remote-opencas.target" : "opencas.target",
        remote ? "remote-fs-pre.target" : "local-fs-pre.target", cache_devname,
        core_devname, cache_devname, core_devname, d->cache_id, d->device,
        d->core_id, d->cache_id, d->core_id);

    if (close(fd) < 0) {
      print_err("failed to write unit file %s: %s\n", unitpath,
                strerror(errno));
      return false;
    }

    char dir[PATH_MAX];

    char symlinktarget[PATH_MAX];
    snprintf(symlinktarget, PATH_MAX, "../%s", unitname);

    snprintf(dir, PATH_MAX, "%s/dev-opencas%d\\x2d%d.device.requires",
             generator_dir, d->cache_id, d->core_id);
    ret = mkdir(dir, 0755);
    if (ret != 0 && errno != EEXIST) {
      print_err("cannot create dir %s: %s\n", dir, strerror(errno));
    }

    char symlinlocation[PATH_MAX];
    snprintf(symlinlocation, PATH_MAX, "%s/%s", dir, unitname);

    ret = symlink(symlinktarget, symlinlocation);
    if (ret != 0) {
      print_err("cannot create symlink %s: %s\n", symlinlocation,
                strerror(errno));
    }

    snprintf(dir, PATH_MAX, "%s/%s.target.requires", generator_dir,
             remote ? "remote-opencas" : "opencas");
    ret = mkdir(dir, 0755);
    if (ret != 0 && errno != EEXIST) {
      print_err("cannot create dir %s: %s\n", dir, strerror(errno));
    }

    snprintf(symlinlocation, PATH_MAX, "%s/%s", dir, unitname);
    ret = symlink(symlinktarget, symlinlocation);
    if (ret != 0) {
      print_err("cannot create symlink %s: %s\n", symlinlocation,
                strerror(errno));
    }

    free(cache_devname);
    free(core_devname);
  }

  return true;
}

static void free_resources() {
  for (int i = 0; i < caches_num; i++) {
    free(caches[i].device);
  }
  for (int i = 0; i < devices_num; i++) {
    free(devices[i].device);
  }
}

static bool run(int argc, const char **argv) {
  if (argc != 4) {
    print_err("incorrect number of arguments for activation generator: %d\n",
              argc);
    return false;
  }

  generator_dir = argv[1];

  if (!parse_config()) {
    return false;
  }

  if (!generate_units()) {
    return false;
  }

  free_resources();

  return true;
}

int main(int argc, const char **argv) {
  bool r;

  log_init();
  r = run(argc, argv);
  if (!r) print_err("activation generator failed\n");
  log_exit();

  return r ? EXIT_SUCCESS : EXIT_FAILURE;
}
