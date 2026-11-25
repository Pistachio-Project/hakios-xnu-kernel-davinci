/*
 * Files of a FAT16 or FAT32 volume, read only: what a Qualcomm phone's `modem` partition is (NON-HLOS: the
 * modem's firmware in image/, which Android mounts as /vendor/firmware_mnt). Enough to find a file by its path
 * and read it whole, for a driver that has the partition's bytes through a function of its own (board/ufs.c)
 * and needs a few files out of it (board/mpss.c: image/modem.mdt and its segments).
 *
 * The volume is not trusted: every count, size and cluster number in it is checked before it is used, a chain of
 * clusters that loops or leaves the volume ends the read with an error, and nothing is read past the buffer the
 * caller gives. Names are matched by their short (8.3) names, without regard to case; long names are skipped,
 * which finds every name that is 8.3 whether the volume has long names for it or not (all of the modem's are).
 * FAT12 (a volume of under 4085 clusters) is refused. Nothing is written.
 */
#ifndef HAKIOS_FAT_CORE_H
#define HAKIOS_FAT_CORE_H

#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/errno.h>

/* bytes [offset, offset + len) of the volume into dst: 0, or an errno */
typedef int (*fat_read_t)(void *context, uint64_t offset, uint8_t *dst, uint64_t len);

#define FAT_TABLE_MAX           (32u << 20)             /* the largest FAT this reads into memory */
#define FAT_PATH_DEPTH          4

struct fat {
	fat_read_t read;
	void *context;
	uint64_t data_offset;           /* where cluster 2 begins */
	uint64_t root_offset;           /* FAT16: the fixed root directory */
	uint32_t root_bytes;
	uint32_t cluster_bytes, clusters;       /* the size of a cluster; how many there are (numbered from 2) */
	uint32_t root_cluster;          /* FAT32: where the root directory begins */
	uint32_t table_bytes;
	bool is32;
	uint8_t *table;                 /* the FAT, read whole */
	uint8_t *scratch;               /* a cluster, for the directories */
};

struct fat_file {
	uint32_t cluster;               /* where it begins; 0: an empty file */
	uint32_t size;
	bool directory;
};

static inline uint32_t
fat_le16(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static inline uint32_t
fat_le32(const uint8_t *p)
{
	return fat_le16(p) | fat_le16(p + 2) << 16;
}

static inline bool
fat_power_of_two(uint32_t value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

static inline void
fat_unmount(struct fat *f)
{
	if (f->table != NULL) {
		kfree_data(f->table, f->table_bytes);
	}
	if (f->scratch != NULL) {
		kfree_data(f->scratch, f->cluster_bytes);
	}
	f->table = f->scratch = NULL;
}

/* The next cluster of a chain: 0 where it ends (or is not one that can be followed). */
static inline uint32_t
fat_next(const struct fat *f, uint32_t cluster)
{
	uint32_t next;

	if (cluster < 2 || cluster >= f->clusters + 2) {
		return 0;
	}
	if (f->is32) {
		next = fat_le32(f->table + 4 * (uint64_t)cluster) & 0x0fffffffu;
	} else {
		next = fat_le16(f->table + 2 * (uint64_t)cluster);
	}
	return next >= 2 && next < f->clusters + 2 ? next : 0;
}

/* The volume's boot sector, and the FAT after it, read: 0, or an errno (EINVAL: not a volume this reads). */
static inline int
fat_mount(struct fat *f, fat_read_t read, void *context, uint64_t volume_bytes)
{
	uint8_t boot[512];
	uint32_t sector, per_cluster, reserved, fats, root_entries, total, fat_sectors, root_sectors, data_sectors;
	uint64_t fat_offset;
	int error;

	bzero(f, sizeof(*f));
	f->read = read;
	f->context = context;
	error = read(context, 0, boot, sizeof(boot));
	if (error != 0) {
		return error;
	}
	if (boot[510] != 0x55 || boot[511] != 0xaa) {
		return EINVAL;
	}
	sector = fat_le16(boot + 11);
	per_cluster = boot[13];
	reserved = fat_le16(boot + 14);
	fats = boot[16];
	root_entries = fat_le16(boot + 17);
	total = fat_le16(boot + 19) != 0 ? fat_le16(boot + 19) : fat_le32(boot + 32);
	fat_sectors = fat_le16(boot + 22) != 0 ? fat_le16(boot + 22) : fat_le32(boot + 36);
	if (sector < 512 || sector > 4096 || !fat_power_of_two(sector) || !fat_power_of_two(per_cluster) || per_cluster > 128 ||
	    reserved == 0 || fats == 0 || fats > 2 || fat_sectors == 0 || total == 0 || (uint64_t)total * sector > volume_bytes) {
		return EINVAL;
	}
	root_sectors = (root_entries * 32 + sector - 1) / sector;
	if ((uint64_t)reserved + (uint64_t)fats * fat_sectors + root_sectors >= total) {
		return EINVAL;
	}
	data_sectors = total - reserved - fats * fat_sectors - root_sectors;
	f->cluster_bytes = sector * per_cluster;
	if (f->cluster_bytes > (1u << 20)) {
		return EINVAL;
	}
	f->clusters = data_sectors / per_cluster;
	/* (a volume says which it is: a FAT32 one has no 16-bit size of its FAT; a small one made by a tool that was told so is FAT32 all the same) */
	f->is32 = fat_le16(boot + 22) == 0;
	if (f->clusters < 4085 || (!f->is32 && f->clusters >= 65525)) {
		return EINVAL;                  /* FAT12, or a FAT16 volume with more clusters than FAT16 has numbers */
	}
	if (f->is32) {
		f->root_cluster = fat_le32(boot + 44);
		if (root_entries != 0 || f->root_cluster < 2 || f->root_cluster >= f->clusters + 2) {
			return EINVAL;
		}
	} else if (root_entries == 0) {
		return EINVAL;
	}
	f->table_bytes = (f->clusters + 2) * (f->is32 ? 4u : 2u);
	if (f->table_bytes > FAT_TABLE_MAX || f->table_bytes > fat_sectors * sector) {
		return EINVAL;
	}
	fat_offset = (uint64_t)reserved * sector;
	f->root_offset = ((uint64_t)reserved + (uint64_t)fats * fat_sectors) * sector;
	f->root_bytes = root_sectors * sector;
	f->data_offset = f->root_offset + f->root_bytes;
	f->table = kalloc_data(f->table_bytes, Z_WAITOK | Z_ZERO);
	f->scratch = kalloc_data(f->cluster_bytes, Z_WAITOK | Z_ZERO);
	if (f->table == NULL || f->scratch == NULL) {
		fat_unmount(f);
		return ENOMEM;
	}
	error = read(context, fat_offset, f->table, f->table_bytes);
	if (error != 0) {
		fat_unmount(f);
	}
	return error;
}

static inline uint64_t
fat_cluster_offset(const struct fat *f, uint32_t cluster)
{
	return f->data_offset + (uint64_t)(cluster - 2) * f->cluster_bytes;
}

/* A path component as the 11 bytes a directory entry names it by: false if it has no such short name. */
static inline bool
fat_short_name(const char *component, size_t length, uint8_t out[11])
{
	size_t dot = length, i;

	for (i = 0; i < length; i++) {
		if (component[i] == '.') {
			dot = i;                /* (the last one is the extension's) */
		}
	}
	if (length == 0 || dot == 0 || dot > 8 || (dot < length && (length - dot - 1 == 0 || length - dot - 1 > 3))) {
		return false;
	}
	memset(out, ' ', 11);
	for (i = 0; i < dot; i++) {
		out[i] = (uint8_t)(component[i] >= 'a' && component[i] <= 'z' ? component[i] - 32 : component[i]);
	}
	for (i = dot + 1; i < length; i++) {
		out[8 + i - dot - 1] = (uint8_t)(component[i] >= 'a' && component[i] <= 'z' ? component[i] - 32 : component[i]);
	}
	return true;
}

/* In the directory that begins at `cluster' (0: the root), the entry with this short name. 0, ENOENT, or an errno. */
static inline int
fat_find(struct fat *f, uint32_t cluster, const uint8_t name[11], struct fat_file *found)
{
	uint32_t hops = 0;

	if (cluster == 0 && !f->is32) {
		/* the fixed root directory of a FAT16 volume: read a cluster's worth at a time */
		for (uint32_t at = 0; at < f->root_bytes; at += f->cluster_bytes) {
			uint32_t bytes = f->root_bytes - at < f->cluster_bytes ? f->root_bytes - at : f->cluster_bytes;
			int error = f->read(f->context, f->root_offset + at, f->scratch, bytes);

			if (error != 0) {
				return error;
			}
			for (uint32_t e = 0; e + 32 <= bytes; e += 32) {
				const uint8_t *entry = f->scratch + e;

				if (entry[0] == 0) {
					return ENOENT;
				}
				if (entry[0] == 0xe5 || (entry[11] & 0x0f) == 0x0f || (entry[11] & 0x08) != 0 || memcmp(entry, name, 11) != 0) {
					continue;
				}
				found->cluster = fat_le16(entry + 26);
				found->size = fat_le32(entry + 28);
				found->directory = (entry[11] & 0x10) != 0;
				return 0;
			}
		}
		return ENOENT;
	}
	if (cluster == 0) {
		cluster = f->root_cluster;
	}
	while (cluster != 0 && hops++ <= f->clusters) {
		int error = f->read(f->context, fat_cluster_offset(f, cluster), f->scratch, f->cluster_bytes);

		if (error != 0) {
			return error;
		}
		for (uint32_t e = 0; e + 32 <= f->cluster_bytes; e += 32) {
			const uint8_t *entry = f->scratch + e;

			if (entry[0] == 0) {
				return ENOENT;
			}
			if (entry[0] == 0xe5 || (entry[11] & 0x0f) == 0x0f || (entry[11] & 0x08) != 0 || memcmp(entry, name, 11) != 0) {
				continue;
			}
			found->cluster = fat_le16(entry + 26) | fat_le16(entry + 20) << 16;
			found->size = fat_le32(entry + 28);
			found->directory = (entry[11] & 0x10) != 0;
			return 0;
		}
		cluster = fat_next(f, cluster);
	}
	return ENOENT;
}

/* A file or directory by its path ("image/modem.b00"): 0, ENOENT (not there, or a name that is no 8.3 name), or an errno. */
static inline int
fat_open(struct fat *f, const char *path, struct fat_file *file)
{
	uint32_t directory = 0;
	unsigned depth = 0;

	bzero(file, sizeof(*file));
	file->directory = true;
	while (*path != 0) {
		const char *end = path;
		uint8_t name[11];
		int error;

		while (*path == '/') {
			path++;
		}
		if (*path == 0) {
			break;
		}
		for (end = path; *end != 0 && *end != '/'; end++) {
		}
		if (!file->directory || ++depth > FAT_PATH_DEPTH || !fat_short_name(path, (size_t)(end - path), name)) {
			return ENOENT;
		}
		error = fat_find(f, directory, name, file);
		if (error != 0) {
			return error;
		}
		if (file->directory && file->cluster == 0 && f->is32) {
			return ENOENT;          /* (a directory that begins nowhere) */
		}
		directory = file->cluster;
		path = end;
	}
	return 0;
}

/*
 * A file's bytes, all of them, into dst (which holds file->size): clusters that follow one another are read in one
 * request. EIO: its chain of clusters ends early, loops, or leaves the volume.
 */
static inline int
fat_read_file(struct fat *f, const struct fat_file *file, uint8_t *dst)
{
	uint64_t left = file->size, hops = 0;
	uint32_t cluster = file->cluster;

	if (file->directory) {
		return EISDIR;
	}
	while (left > 0) {
		uint32_t first = cluster, last = cluster, run = 1;
		uint64_t bytes;
		int error;

		if (cluster == 0) {
			return EIO;
		}
		while (run * (uint64_t)f->cluster_bytes < left && fat_next(f, last) == last + 1) {
			last++;
			run++;
		}
		if ((hops += run) > f->clusters) {
			return EIO;
		}
		bytes = run * (uint64_t)f->cluster_bytes;
		bytes = bytes < left ? bytes : left;
		error = f->read(f->context, fat_cluster_offset(f, first), dst, bytes);
		if (error != 0) {
			return error;
		}
		dst += bytes;
		left -= bytes;
		cluster = fat_next(f, last);
	}
	return 0;
}

/*
 * Bytes [offset, offset + len) of a file into dst: the same, for a file read in pieces (the clusters before offset are
 * walked in the FAT, which is in memory, not read). EINVAL: the range is not inside the file.
 */
static inline int
fat_read_range(struct fat *f, const struct fat_file *file, uint64_t offset, uint8_t *dst, uint64_t len)
{
	uint64_t left = len, hops = 0, skip;
	uint32_t cluster = file->cluster, in_cluster;

	if (file->directory) {
		return EISDIR;
	}
	if (offset > file->size || len > file->size - offset) {
		return EINVAL;
	}
	skip = offset / f->cluster_bytes;
	in_cluster = (uint32_t)(offset % f->cluster_bytes);
	for (uint64_t i = 0; i < skip && len > 0; i++) {
		if (cluster == 0 || ++hops > f->clusters) {
			return EIO;
		}
		cluster = fat_next(f, cluster);
	}
	while (left > 0) {
		uint32_t first = cluster, last = cluster;
		uint64_t room = f->cluster_bytes - in_cluster, bytes;
		int error;

		if (cluster == 0) {
			return EIO;
		}
		while (room < left && fat_next(f, last) == last + 1) {
			last++;
			room += f->cluster_bytes;
		}
		if ((hops += (last - first) + 1) > f->clusters) {
			return EIO;
		}
		bytes = room < left ? room : left;
		error = f->read(f->context, fat_cluster_offset(f, first) + in_cluster, dst, bytes);
		if (error != 0) {
			return error;
		}
		in_cluster = 0;
		dst += bytes;
		left -= bytes;
		cluster = fat_next(f, last);
	}
	return 0;
}

#endif /* HAKIOS_FAT_CORE_H */
