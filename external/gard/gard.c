/* Copyright 2013-2015 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>

#include <ccan/array_size/array_size.h>

#include <mtd/mtd-abi.h>

#include <getopt.h>

#include <libflash/libflash.h>
#include <libflash/libffs.h>
#include <libflash/file.h>
#include <libflash/ecc.h>
#include <libflash/blocklevel.h>

#include "gard.h"

#define FDT_ACTIVE_FLASH_PATH "/proc/device-tree/chosen/ibm,system-flash"
#define SYSFS_MTD_PATH "/sys/class/mtd/"
#define FLASH_GARD_PART "GUARD"

struct gard_ctx {
	bool ecc;
	uint32_t f_size;
	uint32_t f_pos;

	uint32_t gard_part_idx;
	uint32_t gard_data_pos;
	uint32_t gard_data_len;

	struct blocklevel_device *bl;
	struct ffs_handle *ffs;
};

/*
 * Return the size of a struct gard_ctx depending on if the buffer contains
 * ECC bits
 */
static inline size_t sizeof_gard(struct gard_ctx *ctx)
{
	return ctx->ecc ? ecc_buffer_size(sizeof(struct gard_record)) : sizeof(struct gard_record);
}

static void show_flash_err(int rc)
{
	switch (rc) {
		case FFS_ERR_BAD_MAGIC:
			fprintf(stderr, "libffs bad magic\n");
			break;
		case FFS_ERR_BAD_VERSION:
			fprintf(stderr, "libffs bad version\n");
			break;
		case FFS_ERR_BAD_CKSUM:
			fprintf(stderr, "libffs bad check sum\n");
			break;
		case FFS_ERR_PART_NOT_FOUND:
			fprintf(stderr, "libffs flash partition not found\n");
			break;
		/* ------- */
		case FLASH_ERR_MALLOC_FAILED:
			fprintf(stderr, "libflash malloc failed\n");
			break;
		case FLASH_ERR_CHIP_UNKNOWN:
			fprintf(stderr, "libflash unknown flash chip\n");
			break;
		case FLASH_ERR_PARM_ERROR:
			fprintf(stderr, "libflash parameter error\n");
			break;
		case FLASH_ERR_ERASE_BOUNDARY:
			fprintf(stderr, "libflash erase boundary error\n");
			break;
		case FLASH_ERR_WREN_TIMEOUT:
			fprintf(stderr, "libflash WREN timeout\n");
			break;
		case FLASH_ERR_WIP_TIMEOUT:
			fprintf(stderr, "libflash WIP timeout\n");
			break;
		case FLASH_ERR_VERIFY_FAILURE:
			fprintf(stderr, "libflash verification failure\n");
			break;
		case FLASH_ERR_4B_NOT_SUPPORTED:
			fprintf(stderr, "libflash 4byte mode not supported\n");
			break;
		case FLASH_ERR_CTRL_CONFIG_MISMATCH:
			fprintf(stderr, "libflash control config mismatch\n");
			break;
		case FLASH_ERR_CHIP_ER_NOT_SUPPORTED:
			fprintf(stderr, "libflash chip not supported\n");
			break;
		case FLASH_ERR_CTRL_CMD_UNSUPPORTED:
			fprintf(stderr, "libflash unsupported control command\n");
			break;
		case FLASH_ERR_CTRL_TIMEOUT:
			fprintf(stderr, "libflash control timeout\n");
			break;
		case FLASH_ERR_ECC_INVALID:
			fprintf(stderr, "libflash ecc invalid\n");
			break;
		default:
			fprintf(stderr, "A libflash/libffs error has occured %d\n", rc);
	}
}

static const char *target_type_to_str(enum target_type t)
{
	switch (t) {
		case TYPE_NA:
			return "Not applicable";
		case TYPE_SYS:
			return "System";
		case TYPE_NODE:
			return "Node";
		case TYPE_DIMM:
			return "Dimm";
		case TYPE_MEMBUF:
			return "Memory Buffer";
		case TYPE_PROC:
			return "Processor";
		case TYPE_EX:
			return "EX";
		case TYPE_CORE:
			return "Core";
		case TYPE_L2:
			return "L2 cache";
		case TYPE_L3:
			return "L3 cache";
		case TYPE_L4:
			return "L4 cache";
		case TYPE_MCS:
			return "MSC";
		case TYPE_MBA:
			return "MBA";
		case TYPE_XBUS:
			return "XBUS";
		case TYPE_ABUS:
			return "ABUS";
		case TYPE_PCI:
			return "PCI";
		case TYPE_DPSS:
			return "DPSS";
		case TYPE_APSS:
			return "APSS";
		case TYPE_OCC:
			return "OCC";
		case TYPE_PSI:
			return "PSI";
		case TYPE_FSP:
			return "FSP";
		case TYPE_PNOR:
			return "PNOR";
		case TYPE_OSC:
			return "OSC";
		case TYPE_TODCLK:
			return "Time of day clock";
		case TYPE_CONTROL_NODE:
			return "Control Node";
		case TYPE_OSCREFCLK:
			return "OSC Ref Clock";
		case TYPE_OSCPCICLK:
			return "OSC PCI Clock";
		case TYPE_REFCLKENDPT:
			return "Ref Clock";
		case TYPE_PCICLKENDPT:
			return "PCI Clock";
		case TYPE_NX:
			return "NX";
		case TYPE_PORE:
			return "PORE";
		case TYPE_PCIESWITCH:
			return "PCIE Switch";
		case TYPE_CAPP:
			return "CAPP";
		case TYPE_FSI:
			return "FSI";
		case TYPE_TEST_FAIL:
			return "Test Fail";
		case TYPE_LAST_IN_RANGE:
			return "Last";
	}
	return "Unknown";
}

static const char *path_type_to_str(enum path_type t)
{
	switch (t) {
		case PATH_NA:
			return "not applicable";
		case PATH_AFFINITY:
			return "affinity";
		case PATH_PHYSICAL:
			return "physical";
		case PATH_DEVICE:
			return "device";
		case PATH_POWER:
			return "power";
	}
	return "Unknown";
}

static bool get_dev_attr(const char *dev, const char *attr_file, uint32_t *attr)
{
	char dev_path[PATH_MAX] = SYSFS_MTD_PATH;
	/*
	 * Needs to be large enough to hold at most uint32_t represented as a
	 * string in hex with leading 0x
	 */
	char attr_buf[10];
	int fd, rc;

	/*
	 * sizeof(dev_path) - (strlen(dev_path) + 1) is the remaining space in
	 * dev_path, + 1 to account for the '\0'. As strncat could write n+1 bytes
	 * to dev_path the correct calulcation for n is:
	 * (sizeof(dev_path) - (strlen(dev_path) + 1) - 1)
	 */
	strncat(dev_path, dev, (sizeof(dev_path) - (strlen(dev_path) + 1) - 1));
	strncat(dev_path, "/", (sizeof(dev_path) - (strlen(dev_path) + 1) - 1));
	strncat(dev_path, attr_file, (sizeof(dev_path) - (strlen(dev_path) + 1) - 1));
	fd = open(dev_path, O_RDONLY);
	if (fd == -1)
		goto out;

	rc = read(fd, attr_buf, sizeof(attr_buf));
	close(fd);
	if (rc == -1)
		goto out;

	if (attr)
		*attr = strtol(attr_buf, NULL, 0);

	return 0;

out:
	fprintf(stderr, "Couldn't get MTD device attribute '%s' from '%s'\n", dev, attr_file);
	return -1;
}

static int get_dev_mtd(const char *fdt_flash_path, char **r_path)
{
	struct dirent **namelist;
	char fdt_node_path[PATH_MAX];
	int count, i, rc, fd;
	bool done;

	if (!fdt_flash_path)
		return -1;

	fd = open(fdt_flash_path, O_RDONLY);
	if (fd == -1) {
		fprintf(stderr, "Couldn't open '%s' FDT attribute to determine which flash device to use\n",
				fdt_flash_path);
		return -1;
	}

	rc = read(fd, fdt_node_path, sizeof(fdt_node_path));
	close(fd);
	if (rc == -1) {
		fprintf(stderr, "Couldn't read flash FDT node from '%s'\n", fdt_flash_path);
		return -1;
	}

	count = scandir(SYSFS_MTD_PATH, &namelist, NULL, alphasort);
	if (count == -1) {
		fprintf(stderr, "Couldn't scan '%s' for MTD devices\n", SYSFS_MTD_PATH);
		return -1;
	}

	rc = 0;
	done = false;
	for (i = 0; i < count; i++) {
		struct dirent *dirent;
		char dev_path[PATH_MAX] = SYSFS_MTD_PATH;
		char fdt_node_path_tmp[PATH_MAX];

		dirent = namelist[i];
		if (dirent->d_name[0] == '.' || rc || done) {
			free(namelist[i]);
			continue;
		}

		strncat(dev_path, dirent->d_name, sizeof(dev_path) - strlen(dev_path) - 2);
		strncat(dev_path, "/device/of_node", sizeof(dev_path) - strlen(dev_path) - 2);

		rc = readlink(dev_path, fdt_node_path_tmp, sizeof(fdt_node_path_tmp) - 1);
		if (rc == -1) {
			/*
			 * This might fail because it could not exist if the system has flash
			 * devices that present as mtd but don't have corresponding FDT
			 * nodes, just continue silently.
			 */
			free(namelist[i]);
			/* Should still try the next dir so reset rc */
			rc = 0;
			continue;
		}
		fdt_node_path_tmp[rc] = '\0';

		if (strstr(fdt_node_path_tmp, fdt_node_path)) {
			uint32_t flags, size;

			/*
			 * size and flags could perhaps have be gotten another way but this
			 * method is super unlikely to fail so it will do.
			 */

			/* Check to see if device is writeable */
			rc = get_dev_attr(dirent->d_name, "flags", &flags);
			if (rc) {
				free(namelist[i]);
				continue;
			}

			/* Get the size of the mtd device while we're at it */
			rc = get_dev_attr(dirent->d_name, "size", &size);
			if (rc) {
				free(namelist[i]);
				continue;
			}

			strcpy(dev_path, "/dev/");
			strncat(dev_path, dirent->d_name, sizeof(dev_path) - strlen(dev_path) - 2);
			*r_path = strdup(dev_path);
			done = true;
		}
		free(namelist[i]);
	}
	free(namelist);

	if (!done)
		fprintf(stderr, "Couldn't find '%s' corresponding MTD\n", fdt_flash_path);

	/* explicit negative value so as to not return a libflash code */
	return done ? rc : -1;
}

static int do_iterate(struct gard_ctx *ctx,
                      int (*func)(struct gard_ctx *ctx, int pos,
                                  struct gard_record *gard, void *priv),
                      void *priv)
{
	int rc = 0;
	unsigned int i;
	struct gard_record gard, null_gard;

	memset(&null_gard, UINT_MAX, sizeof(gard));
	for (i = 0; i * sizeof_gard(ctx) < ctx->gard_data_len && rc == 0; i++) {
		memset(&gard, 0, sizeof(gard));

		rc = blocklevel_read(ctx->bl, ctx->gard_data_pos +
				(i * sizeof_gard(ctx)), &gard, sizeof(gard));
		/* It isn't super clear what constitutes the end, this should do */
		if (rc || memcmp(&gard, &null_gard, sizeof(gard)) == 0)
			break;

		rc = func(ctx, i, &gard, priv);
	}

	return rc;
}

static int get_largest_pos_i(struct gard_ctx *ctx, int pos, struct gard_record *gard, void *priv)
{
	if (!priv)
		return -1;

	*(int *)priv = pos;

	return 0;
}

static int get_largest_pos(struct gard_ctx *ctx)
{
	int rc, largest = -1;

	rc = do_iterate(ctx, &get_largest_pos_i, &largest);
	if (rc)
		return -1;

	return largest;
}

static int do_list_i(struct gard_ctx *ctx, int pos, struct gard_record *gard, void *priv)
{
	if (!gard)
		return -1;

	printf("| %08x | %08x | %-15s |\n", be32toh(gard->record_id),
	       be32toh(gard->errlog_eid), path_type_to_str(gard->target_id.type_size >> PATH_TYPE_SHIFT));

	return 0;
}

static int do_list(struct gard_ctx *ctx, int argc, char **argv)
{
	int rc;

	/* No entries */
	if (get_largest_pos(ctx) == -1) {
		printf("No GARD entries to display\n");
		rc = 0;
	} else {
		printf("|    ID    |   Error  | Type            |\n");
		printf("+---------------------------------------+\n");
		rc = do_iterate(ctx, &do_list_i, NULL);
		printf("+=======================================+\n");
	}

	return rc;
}

static int do_show_i(struct gard_ctx *ctx, int pos, struct gard_record *gard, void *priv)
{
	uint32_t id;

	if (!priv || !gard)
		return -1;

	id = *(uint32_t *)priv;

	if (be32toh(gard->record_id) == id) {
		unsigned int count, i;

		printf("Record ID:    0x%08x\n", id);
		printf("========================\n");
		printf("Error ID:     0x%08x\n", be32toh(gard->errlog_eid));
		printf("Error Type:         0x%02x\n", gard->error_type);
		printf("Res Recovery:       0x%02x\n", gard->resource_recovery);
		printf("Path Type: %s\n", path_type_to_str(gard->target_id.type_size >> PATH_TYPE_SHIFT));
		count = gard->target_id.type_size & PATH_ELEMENTS_MASK;
		for (i = 0; i < count && i < MAX_PATH_ELEMENTS; i++)
			printf("%*c%s, Instance #%d\n", i + 1, '>', target_type_to_str(gard->target_id.path_elements[i].target_type),
			       gard->target_id.path_elements[i].instance);
	}

	return 0;
}

static int do_show(struct gard_ctx *ctx, int argc, char **argv)
{
	uint32_t id;
	int rc;

	if (argc != 2) {
		fprintf(stderr, "%s option requires a GARD record\n", argv[0]);
		return -1;
	}

	id = strtoul(argv[1], NULL, 16);

	rc = do_iterate(ctx, &do_show_i, &id);

	return rc;
}

static int do_clear_i(struct gard_ctx *ctx, int pos, struct gard_record *gard, void *priv)
{
	int largest = 0, rc = 0;
	char *buf;
	struct gard_record null_gard;

	if (!gard || !ctx)
		return -1;

	memset(&null_gard, INT_MAX, sizeof(null_gard));

	if (!priv) {
		if (pos != 0)
			/* We erased everything on the first iteration, don't bother */
			return 0;

		printf("Erasing the entire gard partition...");
		rc = blocklevel_erase(ctx->bl, ctx->gard_data_pos, ctx->gard_data_len);
		if (rc) {
			fprintf(stderr, "\nCouldn't erase flash partition at 0x%08x for size %u\n",
					ctx->gard_data_pos, ctx->gard_data_len);
			return rc;
		}
		printf("done\n");

		ctx->gard_data_len = sizeof_gard(ctx);
	} else if (be32toh(gard->record_id) == *(uint32_t *)priv) {
		largest = get_largest_pos(ctx);
		if (largest < 0 || pos > largest) {
			/* Something went horribly wrong */
			fprintf(stderr, "largest index out of range %d\n", largest);
			return -1;
		}

		if (pos < largest) {
			/* We're not clearing the last record, shift all the records up */
			int buf_len = ((largest - pos) * sizeof(struct gard_record));
			int buf_pos = ctx->gard_data_pos + ((pos + 1) * sizeof_gard(ctx));
			buf = malloc(buf_len);
			if (!buf)
				return -ENOMEM;

			rc = blocklevel_read(ctx->bl, buf_pos, buf, buf_len);
			if (rc) {
				free(buf);
				fprintf(stderr, "Couldn't read from flash at 0x%08x for len 0x%08x\n", buf_pos, buf_len);
				return rc;
			}

			rc = blocklevel_smart_write(ctx->bl, buf_pos - sizeof_gard(ctx), buf, buf_len);
			free(buf);
			if (rc) {
				fprintf(stderr, "Couldn't write to flash at 0x%08lx for len 0x%08x\n",
				        buf_pos - sizeof_gard(ctx), buf_len);
				return rc;
			}
		}

		ctx->gard_data_len -= sizeof_gard(ctx);
		printf("Cleared gard record with id ID 0x%08x\n", be32toh(gard->record_id));
	}

	/* Now wipe the last record */
	rc = blocklevel_smart_write(ctx->bl, ctx->gard_data_pos + (largest * sizeof_gard(ctx)),
		                            &null_gard, sizeof(null_gard));

	return rc;
}

static int do_clear(struct gard_ctx *ctx, int argc, char **argv)
{
	int rc;
	uint32_t id;

	if (argc != 2) {
		fprintf(stderr, "%s option requires a GARD record or 'all'\n", argv[0]);
		return -1;
	}

	if (strncmp(argv[1], "all", strlen("all")) == 0) {
		rc = do_iterate(ctx, do_clear_i, NULL);
	} else {
		id = strtoul(argv[1], NULL, 16);
		rc = do_iterate(ctx, do_clear_i, &id);
	}

	return rc;
}

int check_gard_partition(struct gard_ctx *ctx)
{
	int rc;
	struct gard_record gard;
	char msg[2];

	if (ctx->gard_data_len == 0 || ctx->gard_data_len % sizeof(struct gard_record) != 0)
		/* Just warn for now */
		fprintf(stderr, "The %s partition doesn't appear to be an exact multiple of"
				"gard records in size: %lu vs %u (or partition is zero in length)\n",
				FLASH_GARD_PART, sizeof(struct gard_record), ctx->gard_data_len);

	/*
	 * Attempt to read the first record, nothing can really operate if the
	 * first record is dead. There (currently) isn't a way to validate more
	 * than ECC correctness.
	 */
	rc = blocklevel_read(ctx->bl, ctx->gard_data_pos, &gard, sizeof(gard));
	if (rc == FLASH_ERR_ECC_INVALID) {
		fprintf(stderr, "The data at the GUARD partition does not appear to be valid gard data\n");
		fprintf(stderr, "Clear the entire GUARD partition? [y/N]\n");
		if (fgets(msg, sizeof(msg), stdin) == NULL) {
			fprintf(stderr, "Couldn't read from standard input\n");
			return -1;
		}
		if (msg[0] == 'y') {
			rc = blocklevel_erase(ctx->bl, ctx->gard_data_pos, ctx->gard_data_len);
			if (rc) {
				fprintf(stderr, "Couldn't erase flash partition. Bailing out\n");
				return rc;
			}

			memset(&gard, INT_MAX, sizeof(gard));

			rc = blocklevel_smart_write(ctx->bl, ctx->gard_data_pos, &gard, sizeof(gard));
			if (rc) {
				fprintf(stderr, "Couldn't create a sane first gard record. Bailing out\n");
				return rc;
			}
		}
		/*
		 * else leave rc as is so that the main bails out, not going to be
		 * able to do sensible anyway
		 */
	}
	return rc;
}



__attribute__ ((unused))
static int do_nop(struct gard_ctx *ctx, int argc, char **argv)
{
	fprintf(stderr, "Unimplemented action '%s'\n", argv[0]);
	return EXIT_SUCCESS;
}

struct {
	const char	*name;
	const char	*desc;
	int		(*fn)(struct gard_ctx *, int, char **);
} actions[] = {
	{ "list", "List current GARD records", do_list },
	{ "show", "Show details of a GARD record", do_show },
	{ "clear", "Clear GARD records", do_clear },
};

static void usage(const char *progname)
{
	unsigned int i;

	fprintf(stderr, "Usage: %s [-a -e -f <file> -p] <command> [<args>]\n\n",
			progname);
	fprintf(stderr, "-e --ecc\n\tForce reading/writing with ECC bytes.\n\n");
	fprintf(stderr, "-f --file <file>\n\tDon't search for MTD device,"
	                " read from <file>.\n\n");
	fprintf(stderr, "-p --part\n\tUsed in conjunction with -f to specify"
	                "that just\n");
	fprintf(stderr, "\tthe GUARD partition is in <file> and libffs\n");
	fprintf(stderr, "\tshouldn't be used.\n\n");


	fprintf(stderr, "Where <command> is one of:\n\n");

	for (i = 0; i < ARRAY_SIZE(actions); i++) {
		fprintf(stderr,  "\t%-7s\t%s\n",
				actions[i].name, actions[i].desc);
	}
}

static struct option global_options[] = {
	{ "file", required_argument, 0, 'f' },
	{ "part", no_argument, 0, 'p' },
	{ "ecc", no_argument, 0, 'e' },
	{ 0 },
};
static const char *global_optstring = "+ef:p";

int main(int argc, char **argv)
{
	const char *action, *progname;
	const char *fdt_flash_path = FDT_ACTIVE_FLASH_PATH;
	char *filename = NULL;
	struct gard_ctx _ctx, *ctx;
	int rc, i = 0;
	bool part = 0;
	bool ecc = 0;

	progname = argv[0];

	ctx = &_ctx;
	memset(ctx, 0, sizeof(*ctx));

	/* process global options */
	for (;;) {
		int c;

		c = getopt_long(argc, argv, global_optstring, global_options,
				NULL);
		if (c == -1)
			break;
		switch (c) {
		case 'e':
			ecc = true;
			break;
		case 'f':
			/* If they specify -f twice */
			free(filename);

			filename = strdup(optarg);
			if (!filename) {
				fprintf(stderr, "Out of memory\n");
				return EXIT_FAILURE;
			}
			break;
		case 'p':
			part = true;
			break;
		case '?':
			usage(progname);
			return EXIT_FAILURE;
		}
	}

	/*
	 * It doesn't make sense to specify that we have the gard partition but
	 * read from flash
	 */
	if (part && !filename) {
		usage(progname);
		return EXIT_FAILURE;
	}

	/* do we have a command? */
	if (optind == argc) {
		usage(progname);
		return EXIT_FAILURE;
	}

	argc -= optind;
	argv += optind;
	action = argv[0];

	if (!filename) {
		rc = get_dev_mtd(fdt_flash_path, &filename);
		if (rc)
			return EXIT_FAILURE;
	}

	rc = file_init_path(filename, NULL, &(ctx->bl));
	if (rc)
		return EXIT_FAILURE;

	rc = blocklevel_get_info(ctx->bl, NULL, &(ctx->f_size), NULL);
	if (rc)
		goto out;

	if (!part) {
		rc = ffs_init(0, ctx->f_size, ctx->bl, &ctx->ffs, 1);
		if (rc)
			goto out;

		rc = ffs_lookup_part(ctx->ffs, FLASH_GARD_PART, &ctx->gard_part_idx);
		if (rc)
			goto out;

		rc = ffs_part_info(ctx->ffs, ctx->gard_part_idx, NULL, &(ctx->gard_data_pos),
				&(ctx->gard_data_len), NULL, &(ctx->ecc));
		if (rc)
			goto out;
	} else {
		if (ecc) {
			rc = blocklevel_ecc_protect(ctx->bl, 0, ctx->f_size);
			if (rc)
				goto out;
		}

		ctx->ecc = ecc;
		ctx->gard_data_pos = 0;
		ctx->gard_data_len = ctx->f_size;
	}

	rc = check_gard_partition(ctx);
	if (rc) {
		fprintf(stderr, "Does not appear to be sane gard data\n");
		goto out;
	}

	for (i = 0; i < ARRAY_SIZE(actions); i++) {
		if (!strcmp(actions[i].name, action)) {
			rc = actions[i].fn(ctx, argc, argv);
			break;
		}
	}

out:
	free(filename);
	if (ctx->ffs)
		ffs_close(ctx->ffs);

	file_exit_close(ctx->bl);

	if (i == ARRAY_SIZE(actions)) {
		fprintf(stderr, "%s: '%s' isn't a valid command\n", progname, action);
		usage(progname);
		return EXIT_FAILURE;
	}

	if (rc > 0) {
		show_flash_err(rc);
		if (filename && rc == FFS_ERR_BAD_MAGIC)
			fprintf(stderr, "Maybe you didn't give a full flash image file?\nDid you mean '--part'?\n");
	}
	return rc;
}