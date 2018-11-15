/*
 * Copyright © 2007-2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <err.h>
#include <assert.h>
#include <getopt.h>
#include <zlib.h>

#include "common/gen_decoder.h"
#include "util/macros.h"

#define CSI "\e["
#define BLUE_HEADER  CSI "0;44m"
#define GREEN_HEADER CSI "1;42m"
#define NORMAL       CSI "0m"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

/* options */

static bool option_full_decode = true;
static bool option_print_all_bb = false;
static bool option_print_offsets = true;
static enum { COLOR_AUTO, COLOR_ALWAYS, COLOR_NEVER } option_color;
static char *xml_path = NULL;

static uint32_t
print_head(unsigned int reg)
{
   printf("    head = 0x%08x, wraps = %d\n", reg & (0x7ffff<<2), reg >> 21);
   return reg & (0x7ffff<<2);
}

static void
print_register(struct gen_spec *spec, const char *name, uint32_t reg)
{
   struct gen_group *reg_spec =
      name ? gen_spec_find_register_by_name(spec, name) : NULL;

   if (reg_spec) {
      gen_print_group(stdout, reg_spec, 0, &reg, 0,
                      option_color == COLOR_ALWAYS);
   }
}

struct ring_register_mapping {
   unsigned ring_class;
   unsigned ring_instance;
   const char *register_name;
};

enum {
   RCS,
   BCS,
   VCS,
   VECS,
};

static const struct ring_register_mapping acthd_registers[] = {
   { BCS, 0, "BCS_ACTHD_UDW" },
   { VCS, 0, "VCS_ACTHD_UDW" },
   { VCS, 1, "VCS2_ACTHD_UDW" },
   { RCS, 0, "ACTHD_UDW" },
   { VECS, 0, "VECS_ACTHD_UDW" },
};

static const struct ring_register_mapping ctl_registers[] = {
   { BCS, 0, "BCS_RING_BUFFER_CTL" },
   { VCS, 0, "VCS_RING_BUFFER_CTL" },
   { VCS, 1, "VCS2_RING_BUFFER_CTL" },
   { RCS, 0, "RCS_RING_BUFFER_CTL" },
   { VECS, 0,  "VECS_RING_BUFFER_CTL" },
};

static const struct ring_register_mapping fault_registers[] = {
   { BCS, 0, "BCS_FAULT_REG" },
   { VCS, 0, "VCS_FAULT_REG" },
   { RCS, 0, "RCS_FAULT_REG" },
   { VECS, 0, "VECS_FAULT_REG" },
};

static int ring_name_to_class(const char *ring_name,
                              unsigned int *class)
{
   static const char *class_names[] = {
      [RCS] = "rcs",
      [BCS] = "bcs",
      [VCS] = "vcs",
      [VECS] = "vecs",
   };
   for (size_t i = 0; i < ARRAY_SIZE(class_names); i++) {
      if (strncmp(ring_name, class_names[i], strlen(class_names[i])))
         continue;

      *class = i;
      return atoi(ring_name + strlen(class_names[i]));
   }

   static const struct {
      const char *name;
      unsigned int class;
      int instance;
   } legacy_names[] = {
      { "render", RCS, 0 },
      { "blt", BCS, 0 },
      { "bsd", VCS, 0 },
      { "bsd2", VCS, 1 },
      { "vebox", VECS, 0 },
   };
   for (size_t i = 0; i < ARRAY_SIZE(legacy_names); i++) {
      if (strcmp(ring_name, legacy_names[i].name))
         continue;

      *class = legacy_names[i].class;
      return legacy_names[i].instance;
   }

   return -1;
}

static const char *
register_name_from_ring(const struct ring_register_mapping *mapping,
                        unsigned nb_mapping,
                        const char *ring_name)
{
   unsigned int class;
   int instance;

   instance = ring_name_to_class(ring_name, &class);
   if (instance < 0)
      return NULL;

   for (unsigned i = 0; i < nb_mapping; i++) {
      if (mapping[i].ring_class == class &&
          mapping[i].ring_instance == instance)
         return mapping[i].register_name;
   }
   return NULL;
}

static const char *
instdone_register_for_ring(const struct gen_device_info *devinfo,
                           const char *ring_name)
{
   unsigned int class;
   int instance;

   instance = ring_name_to_class(ring_name, &class);
   if (instance < 0)
      return NULL;

   switch (class) {
   case RCS:
      if (devinfo->gen == 6)
         return "INSTDONE_2";
      else
         return "INSTDONE_1";

   case BCS:
      return "BCS_INSTDONE";

   case VCS:
      switch (instance) {
      case 0:
         return "VCS_INSTDONE";
      case 1:
         return "VCS2_INSTDONE";
      default:
         return NULL;
      }

   case VECS:
      return "VECS_INSTDONE";
   }

   return NULL;
}

static void
print_pgtbl_err(unsigned int reg, struct gen_device_info *devinfo)
{
   if (reg & (1 << 26))
      printf("    Invalid Sampler Cache GTT entry\n");
   if (reg & (1 << 24))
      printf("    Invalid Render Cache GTT entry\n");
   if (reg & (1 << 23))
      printf("    Invalid Instruction/State Cache GTT entry\n");
   if (reg & (1 << 22))
      printf("    There is no ROC, this cannot occur!\n");
   if (reg & (1 << 21))
      printf("    Invalid GTT entry during Vertex Fetch\n");
   if (reg & (1 << 20))
      printf("    Invalid GTT entry during Command Fetch\n");
   if (reg & (1 << 19))
      printf("    Invalid GTT entry during CS\n");
   if (reg & (1 << 18))
      printf("    Invalid GTT entry during Cursor Fetch\n");
   if (reg & (1 << 17))
      printf("    Invalid GTT entry during Overlay Fetch\n");
   if (reg & (1 << 8))
      printf("    Invalid GTT entry during Display B Fetch\n");
   if (reg & (1 << 4))
      printf("    Invalid GTT entry during Display A Fetch\n");
   if (reg & (1 << 1))
      printf("    Valid PTE references illegal memory\n");
   if (reg & (1 << 0))
      printf("    Invalid GTT entry during fetch for host\n");
}

static void
print_snb_fence(struct gen_device_info *devinfo, uint64_t fence)
{
   printf("    %svalid, %c-tiled, pitch: %i, start: 0x%08x, size: %u\n",
          fence & 1 ? "" : "in",
          fence & (1<<1) ? 'y' : 'x',
          (int)(((fence>>32)&0xfff)+1)*128,
          (uint32_t)fence & 0xfffff000,
          (uint32_t)(((fence>>32)&0xfffff000) - (fence&0xfffff000) + 4096));
}

static void
print_i965_fence(struct gen_device_info *devinfo, uint64_t fence)
{
   printf("    %svalid, %c-tiled, pitch: %i, start: 0x%08x, size: %u\n",
          fence & 1 ? "" : "in",
          fence & (1<<1) ? 'y' : 'x',
          (int)(((fence>>2)&0x1ff)+1)*128,
          (uint32_t)fence & 0xfffff000,
          (uint32_t)(((fence>>32)&0xfffff000) - (fence&0xfffff000) + 4096));
}

static void
print_fence(struct gen_device_info *devinfo, uint64_t fence)
{
   if (devinfo->gen == 6 || devinfo->gen == 7) {
      return print_snb_fence(devinfo, fence);
   } else if (devinfo->gen == 4 || devinfo->gen == 5) {
      return print_i965_fence(devinfo, fence);
   }
}

static void
print_fault_data(struct gen_device_info *devinfo, uint32_t data1, uint32_t data0)
{
   uint64_t address;

   if (devinfo->gen < 8)
      return;

   address = ((uint64_t)(data0) << 12) | ((uint64_t)data1 & 0xf) << 44;
   printf("    Address 0x%016" PRIx64 " %s\n", address,
          data1 & (1 << 4) ? "GGTT" : "PPGTT");
}

#define CSI "\e["
#define NORMAL       CSI "0m"

struct section {
   uint64_t gtt_offset;
   char *ring_name;
   const char *buffer_name;
   uint32_t *data;
   int count;
};

#define MAX_SECTIONS 256
static unsigned num_sections;
static struct section sections[MAX_SECTIONS];

static int zlib_inflate(uint32_t **ptr, int len)
{
   struct z_stream_s zstream;
   void *out;
   const uint32_t out_size = 128*4096;  /* approximate obj size */

   memset(&zstream, 0, sizeof(zstream));

   zstream.next_in = (unsigned char *)*ptr;
   zstream.avail_in = 4*len;

   if (inflateInit(&zstream) != Z_OK)
      return 0;

   out = malloc(out_size);
   zstream.next_out = out;
   zstream.avail_out = out_size;

   do {
      switch (inflate(&zstream, Z_SYNC_FLUSH)) {
      case Z_STREAM_END:
         goto end;
      case Z_OK:
         break;
      default:
         inflateEnd(&zstream);
         return 0;
      }

      if (zstream.avail_out)
         break;

      out = realloc(out, 2*zstream.total_out);
      if (out == NULL) {
         inflateEnd(&zstream);
         return 0;
      }

      zstream.next_out = (unsigned char *)out + zstream.total_out;
      zstream.avail_out = zstream.total_out;
   } while (1);
 end:
   inflateEnd(&zstream);
   free(*ptr);
   *ptr = out;
   return zstream.total_out / 4;
}

static int ascii85_decode(const char *in, uint32_t **out, bool inflate)
{
   int len = 0, size = 1024;

   *out = realloc(*out, sizeof(uint32_t)*size);
   if (*out == NULL)
      return 0;

   while (*in >= '!' && *in <= 'z') {
      uint32_t v = 0;

      if (len == size) {
         size *= 2;
         *out = realloc(*out, sizeof(uint32_t)*size);
         if (*out == NULL)
            return 0;
      }

      if (*in == 'z') {
         in++;
      } else {
         v += in[0] - 33; v *= 85;
         v += in[1] - 33; v *= 85;
         v += in[2] - 33; v *= 85;
         v += in[3] - 33; v *= 85;
         v += in[4] - 33;
         in += 5;
      }
      (*out)[len++] = v;
   }

   if (!inflate)
      return len;

   return zlib_inflate(out, len);
}

static struct gen_batch_decode_bo
get_gen_batch_bo(void *user_data, uint64_t address)
{
   for (int s = 0; s < num_sections; s++) {
      if (sections[s].gtt_offset <= address &&
          address < sections[s].gtt_offset + sections[s].count * 4) {
         return (struct gen_batch_decode_bo) {
            .addr = sections[s].gtt_offset,
            .map = sections[s].data,
            .size = sections[s].count * 4,
         };
      }
   }

   return (struct gen_batch_decode_bo) { .map = NULL };
}

static void
read_data_file(FILE *file)
{
   struct gen_spec *spec = NULL;
   long long unsigned fence;
   int matched;
   char *line = NULL;
   size_t line_size;
   uint32_t offset, value;
   char *ring_name = NULL;
   struct gen_device_info devinfo;

   while (getline(&line, &line_size, file) > 0) {
      char *new_ring_name = NULL;
      char *dashes;

      if (sscanf(line, "%m[^ ] command stream\n", &new_ring_name) > 0) {
         free(ring_name);
         ring_name = new_ring_name;
      }

      if (line[0] == ':' || line[0] == '~') {
         uint32_t *data = NULL;
         int count = ascii85_decode(line+1, &data, line[0] == ':');
         if (count == 0) {
            fprintf(stderr, "ASCII85 decode failed.\n");
            exit(EXIT_FAILURE);
         }
         assert(num_sections < MAX_SECTIONS);
         sections[num_sections].data = data;
         sections[num_sections].count = count;
         num_sections++;
         continue;
      }

      dashes = strstr(line, "---");
      if (dashes) {
         const struct {
            const char *match;
            const char *name;
         } buffers[] = {
            { "ringbuffer", "ring buffer" },
            { "gtt_offset", "batch buffer" },
            { "hw context", "HW Context" },
            { "hw status", "HW status" },
            { "wa context", "WA context" },
            { "wa batchbuffer", "WA batch" },
            { "NULL context", "Kernel context" },
            { "user", "user" },
            { "semaphores", "semaphores", },
            { "guc log buffer", "GuC log", },
            { NULL, "unknown" },
         }, *b;

         free(ring_name);
         ring_name = malloc(dashes - line);
         strncpy(ring_name, line, dashes - line);
         ring_name[dashes - line - 1] = '\0';

         dashes += 4;
         for (b = buffers; b->match; b++) {
            if (strncasecmp(dashes, b->match, strlen(b->match)) == 0)
               break;
         }

         assert(num_sections < MAX_SECTIONS);
         sections[num_sections].buffer_name = b->name;
         sections[num_sections].ring_name = strdup(ring_name);

         uint32_t hi, lo;
         dashes = strchr(dashes, '=');
         if (dashes && sscanf(dashes, "= 0x%08x %08x\n", &hi, &lo))
            sections[num_sections].gtt_offset = ((uint64_t) hi) << 32 | lo;

         continue;
      }

      matched = sscanf(line, "%08x : %08x", &offset, &value);
      if (matched != 2) {
         uint32_t reg, reg2;

         /* display reg section is after the ringbuffers, don't mix them */
         printf("%s", line);

         matched = sscanf(line, "PCI ID: 0x%04x\n", &reg);
         if (matched == 0)
            matched = sscanf(line, " PCI ID: 0x%04x\n", &reg);
         if (matched == 0) {
            const char *pci_id_start = strstr(line, "PCI ID");
            if (pci_id_start)
               matched = sscanf(pci_id_start, "PCI ID: 0x%04x\n", &reg);
         }
         if (matched == 1) {
            if (!gen_get_device_info(reg, &devinfo)) {
               printf("Unable to identify devid=%x\n", reg);
               exit(EXIT_FAILURE);
            }

            printf("Detected GEN%i chipset\n", devinfo.gen);

            if (xml_path == NULL)
               spec = gen_spec_load(&devinfo);
            else
               spec = gen_spec_load_from_path(&devinfo, xml_path);
         }

         matched = sscanf(line, "  CTL: 0x%08x\n", &reg);
         if (matched == 1) {
            print_register(spec,
                           register_name_from_ring(ctl_registers,
                                                   ARRAY_SIZE(ctl_registers),
                                                   ring_name), reg);
         }

         matched = sscanf(line, "  HEAD: 0x%08x\n", &reg);
         if (matched == 1)
            print_head(reg);

         matched = sscanf(line, "  ACTHD: 0x%08x\n", &reg);
         if (matched == 1) {
            print_register(spec,
                           register_name_from_ring(acthd_registers,
                                                   ARRAY_SIZE(acthd_registers),
                                                   ring_name), reg);
         }

         matched = sscanf(line, "  PGTBL_ER: 0x%08x\n", &reg);
         if (matched == 1 && reg)
            print_pgtbl_err(reg, &devinfo);

         matched = sscanf(line, "  ERROR: 0x%08x\n", &reg);
         if (matched == 1 && reg) {
            print_register(spec, "GFX_ARB_ERROR_RPT", reg);
         }

         matched = sscanf(line, "  INSTDONE: 0x%08x\n", &reg);
         if (matched == 1) {
            const char *reg_name =
               instdone_register_for_ring(&devinfo, ring_name);
            if (reg_name)
               print_register(spec, reg_name, reg);
         }

         matched = sscanf(line, "  SC_INSTDONE: 0x%08x\n", &reg);
         if (matched == 1)
            print_register(spec, "SC_INSTDONE", reg);

         matched = sscanf(line, "  SAMPLER_INSTDONE[%*d][%*d]: 0x%08x\n", &reg);
         if (matched == 1)
            print_register(spec, "SAMPLER_INSTDONE", reg);

         matched = sscanf(line, "  ROW_INSTDONE[%*d][%*d]: 0x%08x\n", &reg);
         if (matched == 1)
            print_register(spec, "ROW_INSTDONE", reg);

         matched = sscanf(line, "  INSTDONE1: 0x%08x\n", &reg);
         if (matched == 1)
            print_register(spec, "INSTDONE_1", reg);

         matched = sscanf(line, "  fence[%i] = %Lx\n", &reg, &fence);
         if (matched == 2)
            print_fence(&devinfo, fence);

         matched = sscanf(line, "  FAULT_REG: 0x%08x\n", &reg);
         if (matched == 1 && reg) {
            const char *reg_name =
               register_name_from_ring(fault_registers,
                                       ARRAY_SIZE(fault_registers),
                                       ring_name);
            if (reg_name == NULL)
               reg_name = "FAULT_REG";
            print_register(spec, reg_name, reg);
         }

         matched = sscanf(line, "  FAULT_TLB_DATA: 0x%08x 0x%08x\n", &reg, &reg2);
         if (matched == 2)
            print_fault_data(&devinfo, reg, reg2);

         continue;
      }
   }

   free(line);
   free(ring_name);

   enum gen_batch_decode_flags batch_flags = 0;
   if (option_color == COLOR_ALWAYS)
      batch_flags |= GEN_BATCH_DECODE_IN_COLOR;
   if (option_full_decode)
      batch_flags |= GEN_BATCH_DECODE_FULL;
   if (option_print_offsets)
      batch_flags |= GEN_BATCH_DECODE_OFFSETS;
   batch_flags |= GEN_BATCH_DECODE_FLOATS;

   struct gen_batch_decode_ctx batch_ctx;
   gen_batch_decode_ctx_init(&batch_ctx, &devinfo, stdout, batch_flags,
                             xml_path, get_gen_batch_bo, NULL, NULL);


   for (int s = 0; s < num_sections; s++) {
      printf("--- %s (%s) at 0x%08x %08x\n",
             sections[s].buffer_name, sections[s].ring_name,
             (unsigned) (sections[s].gtt_offset >> 32),
             (unsigned) sections[s].gtt_offset);

      if (option_print_all_bb ||
          strcmp(sections[s].buffer_name, "batch buffer") == 0 ||
          strcmp(sections[s].buffer_name, "ring buffer") == 0 ||
          strcmp(sections[s].buffer_name, "HW Context") == 0) {
         gen_print_batch(&batch_ctx, sections[s].data, sections[s].count,
                         sections[s].gtt_offset);
      }
   }

   gen_batch_decode_ctx_finish(&batch_ctx);

   for (int s = 0; s < num_sections; s++) {
      free(sections[s].ring_name);
      free(sections[s].data);
   }
}

static void
setup_pager(void)
{
   int fds[2];
   pid_t pid;

   if (!isatty(1))
      return;

   if (pipe(fds) == -1)
      return;

   pid = fork();
   if (pid == -1)
      return;

   if (pid == 0) {
      close(fds[1]);
      dup2(fds[0], 0);
      execlp("less", "less", "-FRSi", NULL);
   }

   close(fds[0]);
   dup2(fds[1], 1);
   close(fds[1]);
}

static void
print_help(const char *progname, FILE *file)
{
   fprintf(file,
           "Usage: %s [OPTION]... [FILE]\n"
           "Parse an Intel GPU i915_error_state.\n"
           "With no FILE, debugfs-dri-directory is probed for in /debug and \n"
           "/sys/kernel/debug.  Otherwise, it may be specified. If a file is given,\n"
           "it is parsed as an GPU dump in the format of /debug/dri/0/i915_error_state.\n\n"
           "      --help          display this help and exit\n"
           "      --headers       decode only command headers\n"
           "      --color[=WHEN]  colorize the output; WHEN can be 'auto' (default\n"
           "                        if omitted), 'always', or 'never'\n"
           "      --no-pager      don't launch pager\n"
           "      --no-offsets    don't print instruction offsets\n"
           "      --xml=DIR       load hardware xml description from directory DIR\n"
           "      --all-bb        print out all batchbuffers\n",
           progname);
}

int
main(int argc, char *argv[])
{
   FILE *file;
   const char *path;
   struct stat st;
   int c, i, error;
   bool help = false, pager = true;
   const struct option aubinator_opts[] = {
      { "help",       no_argument,       (int *) &help,                 true },
      { "no-pager",   no_argument,       (int *) &pager,                false },
      { "no-offsets", no_argument,       (int *) &option_print_offsets, false },
      { "headers",    no_argument,       (int *) &option_full_decode,   false },
      { "color",      required_argument, NULL,                          'c' },
      { "xml",        required_argument, NULL,                          'x' },
      { "all-bb",     no_argument,       (int *) &option_print_all_bb,  true },
      { NULL,         0,                 NULL,                          0 }
   };

   i = 0;
   while ((c = getopt_long(argc, argv, "", aubinator_opts, &i)) != -1) {
      switch (c) {
      case 'c':
         if (optarg == NULL || strcmp(optarg, "always") == 0)
            option_color = COLOR_ALWAYS;
         else if (strcmp(optarg, "never") == 0)
            option_color = COLOR_NEVER;
         else if (strcmp(optarg, "auto") == 0)
            option_color = COLOR_AUTO;
         else {
            fprintf(stderr, "invalid value for --color: %s", optarg);
            exit(EXIT_FAILURE);
         }
         break;
      case 'x':
         xml_path = strdup(optarg);
         break;
      default:
         break;
      }
   }

   if (help || argc == 1) {
      print_help(argv[0], stderr);
      exit(EXIT_SUCCESS);
   }

   if (optind >= argc) {
      if (isatty(0)) {
         path = "/sys/class/drm/card0/error";
         error = stat(path, &st);
         if (error != 0) {
            path = "/debug/dri";
            error = stat(path, &st);
         }
         if (error != 0) {
            path = "/sys/kernel/debug/dri";
            error = stat(path, &st);
         }
         if (error != 0) {
            errx(1,
                 "Couldn't find i915 debugfs directory.\n\n"
                 "Is debugfs mounted? You might try mounting it with a command such as:\n\n"
                 "\tsudo mount -t debugfs debugfs /sys/kernel/debug\n");
         }
      } else {
         read_data_file(stdin);
         exit(EXIT_SUCCESS);
      }
   } else {
      path = argv[optind];
      error = stat(path, &st);
      if (error != 0) {
         fprintf(stderr, "Error opening %s: %s\n",
                 path, strerror(errno));
         exit(EXIT_FAILURE);
      }
   }

   if (option_color == COLOR_AUTO)
      option_color = isatty(1) ? COLOR_ALWAYS : COLOR_NEVER;

   if (isatty(1) && pager)
      setup_pager();

   if (S_ISDIR(st.st_mode)) {
      int ret;
      char *filename;

      ret = asprintf(&filename, "%s/i915_error_state", path);
      assert(ret > 0);
      file = fopen(filename, "r");
      if (!file) {
         int minor;
         free(filename);
         for (minor = 0; minor < 64; minor++) {
            ret = asprintf(&filename, "%s/%d/i915_error_state", path, minor);
            assert(ret > 0);

            file = fopen(filename, "r");
            if (file)
               break;

            free(filename);
         }
      }
      if (!file) {
         fprintf(stderr, "Failed to find i915_error_state beneath %s\n",
                 path);
         return EXIT_FAILURE;
      }
   } else {
      file = fopen(path, "r");
      if (!file) {
         fprintf(stderr, "Failed to open %s: %s\n",
                 path, strerror(errno));
         return EXIT_FAILURE;
      }
   }

   read_data_file(file);
   fclose(file);

   /* close the stdout which is opened to write the output */
   fflush(stdout);
   close(1);
   wait(NULL);

   if (xml_path)
      free(xml_path);

   return EXIT_SUCCESS;
}

/* vim: set ts=8 sw=8 tw=0 cino=:0,(0 noet :*/
