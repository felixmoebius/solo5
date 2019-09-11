/*
 * Copyright (c) 2015-2019 Contributors as noted in the AUTHORS file
 *
 * This file is part of Solo5, a sandboxed execution environment.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * elftool.c: Solo5 application manifest generator.
 *
 * This tool produces a C source file defining the binary manifest from its
 * JSON source. The produced C source file should be compiled with the Solo5
 * toolchain and linked into the unikernel binary.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json.h"
#include "elf_abi.h"
#include "mft_abi.h"
#include "solo5_version.h"

/*
 * For "dump" functionality, we pull in the ELF loader and manifest validation
 * code directly to simplify the build.
 */
#include "../tenders/common/mft.c"
#include "../tenders/common/elf.c"

static const char *jtypestr(enum jtypes t)
{
    switch (t) {
    case jnull:     return "NULL";
    case jtrue:     return "BOOLEAN";
    case jfalse:    return "BOOLEAN";
    case jstring:   return "STRING";
    case jarray:    return "ARRAY";
    case jobject:   return "OBJECT";
    case jint:      return "INTEGER";
    case jreal:     return "REAL";
    default:        return "UNKNOWN";
    }
}

static void jexpect(enum jtypes t, jvalue *v, const char *loc)
{
    if (v->d != t)
        errx(1, "%s: expected %s, got %s", loc, jtypestr(t), jtypestr(v->d));
}

static const char out_header[] = \
    "/* Generated by solo5-elftool version %s, do not edit */\n\n"
    "#define MFT_ENTRIES %d\n"
    "#include \"mft_abi.h\"\n"
    "\n"
    "MFT1_NOTE_DECLARE_BEGIN\n"
    "{\n"
    "  .version = MFT_VERSION, .entries = %d,\n"
    "  .e = {\n"
    "    { .name = \"\", .type = MFT_RESERVED_FIRST },\n";

static const char out_entry[] = \
    "    { .name = \"%s\", .type = MFT_DEV_%s },\n";

static const char out_footer[] = \
    "  }\n"
    "}\n"
    "MFT1_NOTE_DECLARE_END\n";

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s COMMAND ...\n", prog);
    fprintf(stderr, "%s version %s\n\n", prog, SOLO5_VERSION);
    fprintf(stderr, "COMMAND is:\n");
    fprintf(stderr, "    abi BINARY:\n");
    fprintf(stderr, "        Dump the ABI target and version from BINARY.\n");
    fprintf(stderr, "    dump BINARY:\n");
    fprintf(stderr, "        Dump the application manifest from BINARY.\n");
    fprintf(stderr, "    gen SOURCE OUTPUT:\n");
    fprintf(stderr, "        Generate application manifest from SOURCE, "
            "writing to OUTPUT.\n");
    exit(EXIT_FAILURE);
}

static int elftool_generate(const char *source, const char *output)
{
    FILE *sfp = fopen(source, "r");
    if (sfp == NULL)
        err(1, "Could not open %s", source);
    FILE *ofp = fopen(output, "w");
    if (ofp == NULL)
        err(1, "Could not open %s", output);

    jvalue *root = jparse(sfp);
    if (root == NULL)
        errx(1, "%s: JSON parse error", source);
    jupdate(root);
    fclose(sfp);
    jexpect(jobject, root, "(root)");

    jvalue *jversion = NULL, *jdevices = NULL;
    /*
     * The manifest always has at least 1 entry of type MFT_RESERVED_FIRST
     * which is added implicitly by us.
     */
    int entries = 1;

    for(jvalue **i = root->u.v; *i; ++i) {
        if (strcmp((*i)->n, "version") == 0) {
            jexpect(jint, *i, ".version");
            jversion = *i;
        }
        else if (strcmp((*i)->n, "devices") == 0) {
            jexpect(jarray, *i, ".devices");
            for (jvalue **j = (*i)->u.v; *j; ++j) {
                jexpect(jobject, *j, ".devices[]");
                entries++;
            }
            jdevices = *i;
        }
        else
            errx(1, "(root): unknown key: %s", (*i)->n);
    }

    if (jversion == NULL)
        errx(1, "missing .version");
    if (jdevices == NULL)
        errx(1, "missing .devices[]");

    if (jversion->u.i != MFT_VERSION)
        errx(1, ".version: invalid version %lld, expected %d", jversion->u.i,
                MFT_VERSION);
    if (entries > MFT_MAX_ENTRIES)
        errx(1, ".devices[]: too many entries, maximum %d", MFT_MAX_ENTRIES);

    fprintf(ofp, out_header, SOLO5_VERSION, entries, entries);
    for (jvalue **i = jdevices->u.v; *i; ++i) {
        jexpect(jobject, *i, ".devices[]");
        char *r_name = NULL, *r_type = NULL;
        for (jvalue **j = (*i)->u.v; *j; ++j) {
            if (strcmp((*j)->n, "name") == 0) {
                jexpect(jstring, *j, ".devices[...]");
                r_name = (*j)->u.s;
            }
            else if (strcmp((*j)->n, "type") == 0) {
                jexpect(jstring, *j, ".devices[...]");
                r_type = (*j)->u.s;
            }
            else
                errx(1, ".devices[...]: unknown key: %s", (*j)->n);
        }
        if (r_name == NULL)
            errx(1, ".devices[...]: missing .name");
        if (r_name[0] == 0)
            errx(1, ".devices[...]: .name may not be empty");
        if (strlen(r_name) > MFT_NAME_MAX)
            errx(1, ".devices[...]: name too long");
        for (char *p = r_name; *p; p++)
            if (!isalnum((unsigned char)*p))
                errx(1, ".devices[...]: name is not alphanumeric");
        if (r_type == NULL)
            errx(1, ".devices[...]: missing .type");
        fprintf(ofp, out_entry, r_name, r_type);
    }
    fprintf(ofp, out_footer);

    fclose(ofp);
    jdel(root);
    return EXIT_SUCCESS;
}

static int elftool_dump(const char *binary)
{
    int bin_fd = open(binary, O_RDONLY);
    if (bin_fd == -1)
    if (bin_fd == -1)
        err(1, "%s: Could not open", binary);

    struct mft *mft;
    size_t mft_size;
    if (elf_load_note(bin_fd, binary, MFT1_NOTE_TYPE, MFT1_NOTE_ALIGN,
                MFT1_NOTE_MAX_SIZE, (void **)&mft, &mft_size) == -1) {
        warnx("%s: No Solo5 manifest found in executable", binary);
        close(bin_fd);
        return EXIT_FAILURE;
    }
    if (mft_validate(mft, mft_size) == -1) {
        free(mft);
        close(bin_fd);
        warnx("%s: Manifest validation failed", binary);
        return EXIT_FAILURE;
    }

    printf("{\n"
	   "    \"version\": %u,\n"
	   "    \"devices\": [\n", MFT_VERSION);

    for (unsigned i = 0; i != mft->entries; i++) {
	if (mft->e[i].type >= MFT_RESERVED_FIRST)
	    continue;

	printf("        { \"name\": \"%s\", \"type\": \"%s\" }%s\n",
		mft->e[i].name, mft_type_to_string(mft->e[i].type),
		(i == (mft->entries -1) ? "" : ","));
    }

    printf("    ]\n"
	   "}\n");

    free(mft);
    close(bin_fd);
    return EXIT_SUCCESS;
}

static const char *abi_target_to_string(int abi_target)
{
    switch(abi_target) {
	case HVT_ABI_TARGET:
	    return "hvt";
	case SPT_ABI_TARGET:
	    return "spt";
	case VIRTIO_ABI_TARGET:
	    return "virtio";
	case MUEN_ABI_TARGET:
	    return "muen";
	case GENODE_ABI_TARGET:
	    return "genode";
	default:
	    return "unknown";
    }
}

static int elftool_abi(const char *binary)
{
    int bin_fd = open(binary, O_RDONLY);
    if (bin_fd == -1)
    if (bin_fd == -1)
        err(1, "%s: Could not open", binary);

    struct abi1_info *abi1;
    size_t abi1_size;
    if (elf_load_note(bin_fd, binary, ABI1_NOTE_TYPE, ABI1_NOTE_ALIGN,
		ABI1_NOTE_MAX_SIZE, (void **)&abi1, &abi1_size) == -1)
        errx(1, "%s: No Solo5 ABI information found in executable",
                binary);
    printf("ABI target: %s\n", abi_target_to_string(abi1->abi_target));
    printf("ABI version: %u\n", abi1->abi_version);
    
    free(abi1);
    close(bin_fd);
    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    const char *prog;

    prog = basename(argv[0]);

    if (argc < 2)
        usage(prog);
    if (strcmp(argv[1], "gen") == 0) {
        if (argc != 4)
            usage(prog);
        return elftool_generate(argv[2], argv[3]);
    }
    else if (strcmp(argv[1], "dump") == 0) {
        if (argc != 3)
            usage(prog);
        return elftool_dump(argv[2]);
    }
    else if (strcmp(argv[1], "abi") == 0) {
        if (argc != 3)
            usage(prog);
        return elftool_abi(argv[2]);
    }
    else
        usage(prog);
}
