/*
 * The FAT reader the modem's firmware is read with (drivers/board/fat_core.h), on volumes made here: a FAT32 volume
 * with a directory image/ of files whose clusters are not in order, a FAT16 volume with its fixed root directory, and
 * volumes made wrong -- a chain that loops, a chain that leaves the volume, a FAT too small for its clusters, a
 * boot sector that is none. What must hold: a file is found by its path in any case, long-name entries and deleted
 * ones are passed over, a file is read whole from clusters wherever they are, and nothing wrong ends in a read
 * past a buffer or in a loop.
 *
 *   fat_test SCENARIO
 */
#include "fakekernel.h"
#include "board/fat_core.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

#define SECTOR          512u
static uint8_t *volume;
static uint64_t volume_bytes;
static uint32_t reads;

static int
volume_read(void *context __unused, uint64_t offset, uint8_t *dst, uint64_t len)
{
	if (offset > volume_bytes || len > volume_bytes - offset) {
		return EIO;
	}
	reads++;
	memcpy(dst, volume + offset, len);
	return 0;
}

static void
put16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void
put32(uint8_t *p, uint32_t v)
{
	put16(p, v);
	put16(p + 2, v >> 16);
}

static struct {
	bool is32;
	uint32_t per_cluster, reserved, fat_sectors, root_entries, clusters;
	uint64_t fat, data, root;
} g;

/* a volume of `clusters' clusters of `per' sectors, empty: FAT32 or FAT16 */
static void
make(bool is32, uint32_t per, uint32_t clusters)
{
	uint32_t root_sectors;
	uint8_t *boot;

	g.is32 = is32;
	g.per_cluster = per;
	g.clusters = clusters;
	g.reserved = is32 ? 32 : 1;
	g.root_entries = is32 ? 0 : 512;
	root_sectors = g.root_entries * 32 / SECTOR;
	g.fat_sectors = ((clusters + 2) * (is32 ? 4 : 2) + SECTOR - 1) / SECTOR;
	g.fat = (uint64_t)g.reserved * SECTOR;
	g.root = g.fat + (uint64_t)2 * g.fat_sectors * SECTOR;
	g.data = g.root + (uint64_t)root_sectors * SECTOR;
	volume_bytes = g.data + (uint64_t)clusters * per * SECTOR;
	free(volume);
	volume = calloc(1, volume_bytes);
	boot = volume;
	put16(boot + 11, SECTOR);
	boot[13] = (uint8_t)per;
	put16(boot + 14, g.reserved);
	boot[16] = 2;
	put16(boot + 17, g.root_entries);
	if (volume_bytes / SECTOR < 65536 && !is32) {
		put16(boot + 19, (uint32_t)(volume_bytes / SECTOR));
	} else {
		put32(boot + 32, (uint32_t)(volume_bytes / SECTOR));
	}
	if (is32) {
		put32(boot + 36, g.fat_sectors);
		put32(boot + 44, 2);
	} else {
		put16(boot + 22, g.fat_sectors);
	}
	boot[510] = 0x55;
	boot[511] = 0xaa;
}

static void
fat_set(uint32_t cluster, uint32_t next)
{
	if (g.is32) {
		put32(volume + g.fat + 4 * (uint64_t)cluster, next);
	} else {
		put16(volume + g.fat + 2 * (uint64_t)cluster, next);
	}
}

static uint32_t eoc(void) { return g.is32 ? 0x0fffffffu : 0xffffu; }

static uint8_t *
cluster_at(uint32_t cluster)
{
	return volume + g.data + (uint64_t)(cluster - 2) * g.per_cluster * SECTOR;
}

/* a directory entry at `entry' */
static void
dirent(uint8_t *entry, const char *name83, uint8_t attr, uint32_t cluster, uint32_t size)
{
	memset(entry, 0, 32);
	memcpy(entry, name83, 11);
	entry[11] = attr;
	put16(entry + 20, cluster >> 16);
	put16(entry + 26, cluster);
	put32(entry + 28, size);
}

/* a file's bytes: a pattern of its own, so that clusters read from the wrong place are seen */
static uint8_t
pattern(unsigned which, uint32_t at)
{
	return (uint8_t)(which * 37 + at * 7 + (at >> 8) * 13 + (at >> 16));
}

/* a file of `size' bytes in the clusters of `list' (in that order), the last one's chain ended */
static void
store(unsigned which, const uint32_t *list, unsigned count, uint32_t size)
{
	uint32_t cluster_bytes = g.per_cluster * SECTOR;

	for (unsigned i = 0; i < count; i++) {
		for (uint32_t b = 0; b < cluster_bytes && (uint64_t)i * cluster_bytes + b < size; b++) {
			cluster_at(list[i])[b] = pattern(which, i * cluster_bytes + b);
		}
		fat_set(list[i], i + 1 < count ? list[i + 1] : eoc());
	}
}

static bool
mounted(struct fat *f)
{
	return fat_mount(f, volume_read, NULL, volume_bytes) == 0;
}

static bool
file_is(struct fat *f, const char *path, unsigned which, uint32_t size)
{
	struct fat_file file;
	uint8_t *data;
	bool good = true;

	if (fat_open(f, path, &file) != 0 || file.size != size || file.directory) {
		return false;
	}
	data = calloc(1, size + 16);
	memset(data + size, 0xcc, 16);
	good = fat_read_file(f, &file, data) == 0;
	for (uint32_t i = 0; i < size && good; i++) {
		good = data[i] == pattern(which, i);
	}
	for (unsigned i = 0; i < 16 && good; i++) {
		good = data[size + i] == 0xcc;          /* (nothing written past the file) */
	}
	uint32_t counted = reads;

	/* pieces: from any offset, across cluster and run boundaries, in sizes that do not line up */
	for (uint32_t at = 0; at < size && good; at += 1 + size / 7) {
		for (uint32_t n = 1; n <= 3000 && good; n += 997) {
			uint8_t *piece;

			if (at + n > size) {
				break;
			}
			piece = calloc(1, n + 16);
			memset(piece + n, 0xcc, 16);
			good = fat_read_range(f, &file, at, piece, n) == 0;
			for (uint32_t i = 0; i < n && good; i++) {
				good = piece[i] == pattern(which, at + i);
			}
			for (unsigned i = 0; i < 16 && good; i++) {
				good = piece[n + i] == 0xcc;
			}
			free(piece);
		}
	}
	good = good && fat_read_range(f, &file, size, data, 0) == 0 && fat_read_range(f, &file, size, data, 1) == EINVAL &&
	    fat_read_range(f, &file, 0, data, (uint64_t)size + 1) == EINVAL;
	reads = counted;
	free(data);
	return good;
}

/* the clusters of a file of `size' bytes from `base': pairs that follow one another, a gap between the pairs */
static unsigned
chain(uint32_t base, uint32_t size, uint32_t *list)
{
	unsigned count = (size + g.per_cluster * SECTOR - 1) / (g.per_cluster * SECTOR);

	for (unsigned i = 0; i < count; i++) {
		list[i] = base + i + (i / 2) * 3;
	}
	return count;
}

/* root: a volume label, a deleted entry, a long-name entry, image/; image/: four files */
static void
tree(void)
{
	uint32_t list[64];
	uint8_t *root = g.is32 ? cluster_at(2) : volume + g.root;

	if (g.is32) {
		fat_set(2, eoc());
	}
	dirent(root + 0, "MYPHONE    ", 0x08, 0, 0);
	dirent(root + 32, "\xe5" "OLD     TXT", 0x20, 0, 0);
	dirent(root + 64, "A       TXT", 0x0f, 0, 0);                    /* (a long-name entry's bytes: skipped) */
	dirent(root + 96, "IMAGE      ", 0x10, 3, 0);
	fat_set(3, eoc());
	dirent(cluster_at(3) + 0, "MODEM   MDT", 0x20, 0, 0);
	dirent(cluster_at(3) + 32, "MODEM   B00", 0x20, 20, 5000);
	dirent(cluster_at(3) + 64, "MODEM   B01", 0x20, 7, 100);
	dirent(cluster_at(3) + 96, "WLANMDSPMBN", 0x20, 100, 1124);
	unsigned count = chain(20, 5000, list);

	store(1, list, count, 5000);
	list[0] = 7;
	store(2, list, 1, 100);
	count = chain(100, 1124, list);
	store(3, list, count, 1124);
}

static void
fat32(void)
{
	struct fat f;
	struct fat_file file;

	make(true, 2, 70000);                   /* 70000 clusters: FAT32 by the count */
	tree();
	CHECK(mounted(&f), "a FAT32 volume");
	CHECK(f.is32 && f.clusters == 70000 && f.cluster_bytes == 1024, "%u clusters of %u", f.clusters, f.cluster_bytes);
	CHECK(file_is(&f, "image/modem.b00", 1, 5000), "a fragmented file");
	CHECK(file_is(&f, "IMAGE/MODEM.B00", 1, 5000), "in capitals");
	CHECK(file_is(&f, "/image//Modem.b01", 2, 100), "slashes, mixed case, a file of one cluster");
	CHECK(file_is(&f, "image/wlanmdsp.mbn", 3, 1124), "a file that ends in a part cluster");
	CHECK(file_is(&f, "image/modem.mdt", 0, 0), "an empty file");
	CHECK(fat_open(&f, "image/nothing.b00", &file) == ENOENT, "a file that is not there");
	CHECK(fat_open(&f, "old.txt", &file) == ENOENT, "a deleted one is not there");
	CHECK(fat_open(&f, "a.txt", &file) == ENOENT, "a long-name entry is not a file");
	CHECK(fat_open(&f, "myphone", &file) == ENOENT, "nor is the volume's label");
	CHECK(fat_open(&f, "image/modem.b00/x", &file) == ENOENT, "a file is not a directory");
	CHECK(fat_open(&f, "image/a-name-too-long.mdt", &file) == ENOENT, "a name that is no 8.3 name");
	CHECK(fat_open(&f, "image/modem.abcd", &file) == ENOENT, "an extension of four");
	CHECK(fat_open(&f, "image", &file) == 0 && file.directory, "a directory");
	CHECK(fat_read_file(&f, &file, (uint8_t *)&file) == EISDIR, "which is not read");
	reads = 0;
	CHECK(file_is(&f, "image/modem.b00", 1, 5000), "again");
	CHECK(reads <= 5, "a file of three runs, and the directories: %u requests", reads);
	fat_unmount(&f);
}

static void
fat16(void)
{
	struct fat f;

	make(false, 1, 5000);                   /* 5000 clusters: FAT16 by the count */
	tree();
	CHECK(mounted(&f), "a FAT16 volume");
	CHECK(!f.is32 && f.clusters == 5000 && f.cluster_bytes == 512, "%u clusters of %u", f.clusters, f.cluster_bytes);
	CHECK(file_is(&f, "image/modem.b00", 1, 5000), "a fragmented file");
	CHECK(file_is(&f, "image/wlanmdsp.mbn", 3, 1124), "a file that ends in a part cluster");
	fat_unmount(&f);
}

static void
broken(void)
{
	struct fat f;
	struct fat_file file;
	uint8_t *data;

	make(true, 2, 70000);
	tree();
	fat_set(26, 20);                        /* the first file's chain loops back to its beginning, and the file says it is long */
	put32(cluster_at(3) + 32 + 28, 90u << 20);
	CHECK(mounted(&f), "mounted");
	CHECK(fat_open(&f, "image/modem.b00", &file) == 0, "found");
	data = calloc(1, file.size);
	CHECK(fat_read_file(&f, &file, data) == EIO, "a chain that loops ends in an error");
	free(data);
	fat_unmount(&f);

	make(true, 2, 70000);
	tree();
	fat_set(26, 70002 + 5);                 /* ... leaves the volume (the next is not a cluster: the chain ends early) */
	CHECK(mounted(&f), "mounted");
	CHECK(fat_open(&f, "image/modem.b00", &file) == 0, "found");
	data = calloc(1, file.size);
	CHECK(fat_read_file(&f, &file, data) == EIO, "a chain that leaves the volume ends in an error");
	free(data);
	fat_unmount(&f);

	make(true, 2, 70000);
	tree();
	put32(volume + 36, 10);                 /* a FAT too small for the clusters */
	CHECK(!mounted(&f), "a FAT of 10 sectors for 70000 clusters");
	make(true, 2, 70000);
	volume[510] = 0;
	CHECK(!mounted(&f), "no boot sector signature");
	make(true, 2, 70000);
	volume[13] = 3;
	CHECK(!mounted(&f), "clusters of three sectors");
	make(true, 2, 70000);
	put32(volume + 32, 0xffffffffu);
	CHECK(!mounted(&f), "more sectors than the volume has");
	make(true, 2, 70000);
	put32(volume + 44, 0);
	CHECK(!mounted(&f), "a root directory that begins nowhere");
	make(false, 1, 2000);
	CHECK(!mounted(&f), "FAT12 (2000 clusters)");
	make(true, 2, 70000);
	put16(volume + 11, 100);
	CHECK(!mounted(&f), "a sector of 100 bytes");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "fat32", fat32 }, { "fat16", fat16 }, { "broken", broken },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			fprintf(stderr, "%-10s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
