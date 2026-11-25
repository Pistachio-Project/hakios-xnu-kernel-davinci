/*
 * The data volume of Darwin between two boots: /private/var packed into the phone's flash and
 * unpacked out of it again.
 *
 * /private/var is a memory filesystem (bsd/miscfs/ramfs), mounted empty by init at every boot and
 * given the directories of the template Darwin ships with (darwin165/mkvarskel.py), so nothing Darwin keeps
 * there outlives the boot that wrote it: no home screen layout, no application data, no baked
 * wallpaper. The kernel's APFS driver reads and does not write, and writing APFS is not the point
 * here -- what is needed is that the bytes come back. So the tree is packed into a plain archive
 * and the archive is written into the one place on the phone's flash this port owns and nothing of
 * it uses: the partition darwin, past the Darwin volume and both cryptexes. The kernel takes those
 * writes on /dev/rufs_darwin alone and refuses anything else (bsd/dev/board/ufs.c, ufs_write=1).
 *
 *   var_store save          pack /private/var into the store
 *   var_store load          unpack the newest good store into /private/var (init, at boot)
 *   var_store watch [SECS]  stay and save by itself (var_store=1, or var_store=new: a new data volume
 *                           this boot, not unpacked from the store): when the kernel says the run is ending, and
 *                           every SECS seconds besides (a launchd job of ours does this)
 *   var_store show          what the store holds, both slots
 *   var_store erase         forget both slots (the next boot starts empty again)
 *   var_store stamp PATH    write a line into PATH saying when: a marker for a run
 *   var_store bootok        tell the loader's boot guard that this generation came up
 *
 * Nobody has to remember to save. `watch` registers with the kernel (kern.hakios.var_save), and
 * on the orderly way back to the loader -- the run's time up, or "xl stop" -- the kernel asks it
 * for the volume and waits, bounded, before it goes (bsd/dev/board/ufs.c,
 * hakios_var_save_on_return). A save every SECS seconds is there as well but is not the default:
 * one costs seconds of the run and a hundred-odd megabytes of flash, and while it runs the
 * storage is read through the loader rather than through this kernel's own driver.
 *
 * One saver at a time: a write to the store is refused unless the writer holds the claim
 * (kern.hakios.var_busy), so two of them cannot interleave into one slot. Even if they did, the
 * slot being written is never the one holding the newest good archive, and a header is written
 * last and over a zeroed one -- so the worst an interrupted or doubled save can do is lose itself.
 *
 * Two slots, written in turn: the one not holding the newest good archive is the one written, its
 * header first zeroed, the archive next, the header last. So a save that does not finish -- the
 * phone reset in the middle of it -- loses nothing: the other slot still holds the archive from
 * before, and `load` takes the newest slot whose header and contents both check out.
 *
 * `load` reads the archive twice: once to see that its checksum is the one in the header, and only
 * then to make files. A store that was torn or is half a different archive therefore leaves
 * /private/var as init made it rather than half filled.
 *
 * The boot guard (xnuloader/flashboot.c in hakios-xnu-kernel): the loader counts every start of a boot
 * bundle in a block of darwin's boot area and gives up on a generation that was started three times
 * without coming up. `bootok` is the "came up": the watcher says it once the run has lasted
 * BOOTOK_AFTER seconds, so a kernel or a userland that dies early is rolled back by itself.
 *
 * What is left out: sockets and devices, which the daemons of a new boot make for themselves, and
 * the directories in SKIP below, which hold what only this boot can mean (its runtime sockets, its
 * swap, its logs) or what is grafted from a read-only image at every boot anyway. And the keychain,
 * unless asked for (keychain_kept below): nothing here encrypts it.
 */
#include "mini.h"

#define VS_O_NOFOLLOW   0x100           /* Darwin's O_NOFOLLOW (mini.h has not got it) */
static unsigned changed_under;          /* files that were another by the time they were opened: left out */

#define BLOCK           4096
#define BUFFER          (4u << 20)              /* the loader's own buffer: one request a write */
#define STORE_AT        8589934592LL            /* darwin's block 2097152: past the cryptexes, with room to spare */
#define SLOT_BYTES      0x80000000LL            /* 2 GiB a slot, two slots */
#define SLOTS           2
#define DEVICE          "/dev/rufs_darwin"
#define STATE_AT        8566865920LL            /* darwin's block 2091520: the boot state (common/flash/bundlefmt.py) */
#define BOOTOK_AFTER    60                      /* seconds of a run that make it a good start */
#define ROOT            "/private/var"
#define MAX_PATH        1024
#define MAX_DEPTH       32
#define MAX_XATTR       (256u << 10)            /* one attribute's value; Darwin keeps small ones */
#define XPACK           (512u << 10)            /* all the attributes of one file, packed */
#define XATTRS          4096                    /* attributes of one file at most, the access control list among them:
                                                 * `load` takes an entry with more for a broken archive, so no more are packed */
#define LINKS           8192                    /* names remembered for hard links */
#define LINK_PATH       256
/*
 * The file flags put back: the ones a tree of files can mean. UF_COMPRESSED is not among them
 * (a ramfs file is not a compressed HFS one, and the flag would make it read as empty), nor are
 * the flags only a single-user boot can clear.
 */
#define FLAGS_KEPT      (0x1u | 0x2u | 0x4u | 0x8u | 0x8000u)   /* nodump, immutable, append, opaque, hidden */

/* the directories whose contents are not kept (the directory itself is) */
static const char *const SKIP[] = {
	"tmp",                  /* its name says it */
	"mobile/tmp",           /* the same, the mobile user's: 21 MB in a store (audiomxd 17, assetsd 4), wired in RAM from the restore on */
	"run",                  /* runtime sockets and pid files: this boot's, not the last one's */
	"vm",                   /* the compressor's swap */
	"log", "logs",          /* what the last boot said */
	"db/diagnostics", "db/uuidtext",        /* logd's archive: tens of MB a boot, and remade */
	"jb",                   /* the package prefix, grafted from the read-only image at every boot */
	/* dirs_cleaner's per-boot temporary roots (DARWIN_USER_TEMP/CACHE): a boot makes one under a new random name and
	 * never looks into an earlier one, which dirs_cleaner would have emptied; kept, they piled up ~21 MB a boot
	 * (23 of them, 485 MB of an 817 MB archive, run 213754) */
	"dirs_cleaner", "mobile/dirs_cleaner",
	/* chronod's state (ChronoKit, the widgets' timelines): restored into a new boot under iOS 27 it had SpringBoard
	 * stand at the boot logo, no frame drawn (found by halves with var_store_skip: everything else of the volume
	 * restored, the boot came up); the widgets are made again instead */
	"mobile/Library/chronod",
	/* MobileGestalt's answers, kept in a cache it makes when it is missing: kept across boots they are the first boot's,
	 * whatever the device tree says now -- a boot with torch=3 had "camera-flash" still false, AVFlashlight no
	 * flashlight. Made again at every boot, as before /private/var was kept */
	"containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches",
	NULL,
};

/*
 * Diagnostic and regenerable data, left out as well unless boot-arg var_store_diag=1 asks for it (and, with it, the
 * log files over LOG_CAP below). Everything here is made again by the system or only says what a run did; none of it is
 * settings, preferences, keychain, or the shader and icon caches, which stay. What it saves is the growth of the store
 * from run to run (322 MB at generation 4, 627 MB at 13), and with it the time of the save, which grows with the
 * bytes. Paths are relative to /private/var, a directory's contents are left out, the directory is kept.
 */
/* (kept, not in this list: db/dyld and Caches/com.apple.dyld -- the apps' launch closures, slow launches without
 * them -- and SplashBoard/Snapshots -- the launch screens: without them an app opened on a white screen, run 210530) */
static const char *const SKIP_DIAG[] = {
	"Logs", "root/Library/Logs",
	"mobile/Library/Logs",                  /* CrashReporter, analytics, DiagnosticMessages, MobileInstallation, ... */
	"db/analyticsd", "db/powerlog", "db/PowerLog", "db/ReportCrash",
	"db/Spotlight", "mobile/Library/Spotlight", "mobile/Library/Metadata/CoreSpotlight",
	"db/biome", "mobile/Library/Biome",     /* the event streams: personalisation only */
	"mobile/Library/CoreDuet", "mobile/Library/DuetExpertCenter", "mobile/Library/IntelligencePlatform",
	"mobile/Library/PowerLog", "mobile/Library/com.apple.aggregated",
	"mobile/Media/MediaAnalysis",           /* mediaanalysisd's scene and face results */
	"mobile/Library/Caches/com.apple.nsurlsessiond/Downloads",
	"mobile/Library/Caches/com.apple.CacheDelete",
	"MobileAsset/Downloads", "mobile/Library/Caches/com.apple.mobileassetd",
	"db/CoreDuet", "mobile/Library/Caches/com.apple.coreduetd",
	NULL,
};

/*
 * Boot-arg var_store_lean=1: left out as well, to take memory back (/private/var is wired RAM, the whole of it, from the
 * restore on): what the system makes again when it is missing, though a first start of the program after it may be
 * slower. About 60 MB of a 400 MB store (run 231808's sizes: Shortcuts 49 MB, StatusKit 5, Finance 5).
 */
static const char *const SKIP_LEAN[] = {
	"mobile/Library/Shortcuts", "mobile/Library/StatusKit", "mobile/Library/Finance",
	NULL,
};

/* a log file bigger than this is left out (the diagnostic list's second half) */
#define LOG_CAP         (1u << 20)

/*
 * And by name, wherever it is: LaunchServices' database (com.apple.LaunchServices-*.csstore, in a data container
 * whose name is a UUID). 16.5 made it at every boot by walking /Applications, which is how an application put into
 * the system image appears; kept across boots it is the first boot's, and an application added since is never
 * registered ("Unable to register /Applications/Sileo.app", uicache included). lsd makes it again when it is missing.
 */
static int
is_launchservices_store(const char *relative)
{
	static const char name[] = "com.apple.LaunchServices";

	for (const char *at = relative; *at != 0; at++) {
		if ((at == relative || at[-1] == '/') && strncmp(at, name, sizeof(name) - 1) == 0) {
			return 1;       /* (the name of the path or of a directory on it) */
		}
	}
	return 0;
}

enum { ENT_DIR = 1, ENT_FILE = 2, ENT_SYMLINK = 3, ENT_HARDLINK = 4, ENT_DIRTIME = 5,
       ENT_PAD = 6 /* size bytes of nothing follow the head: puts the next file's data on a boundary (no path, no pad after) */ };

/* a file this big has its data begin on, and its entry end on, an ALIGN boundary of the archive (pack_one) */
#define ALIGN           (32u << 10)
#define ALIGN_MIN       (128u << 10)

#define HEAD_MAGIC      "RDWVAR\0"
#define HEAD_VERSION    1

struct head {
	char     magic[8];
	uint32_t version;
	uint32_t slot;
	uint64_t generation;            /* the newest good slot is the one with the highest */
	uint64_t bytes;                 /* the archive, without the padding to a whole block */
	uint64_t entries;
	uint64_t seconds;               /* when it was written */
	uint32_t crc;                   /* of those bytes */
	uint32_t head_crc;              /* of this structure with head_crc zero */
};

struct entry {
	uint32_t kind;
	uint32_t mode;                  /* permission bits */
	uint32_t uid, gid;
	uint32_t flags;                 /* st_flags */
	uint32_t pathlen;
	uint32_t nxattr;
	uint32_t mtime_nsec;
	int64_t  mtime_sec;
	uint64_t size;                  /* a file's contents, a link's target, a hard link's first name */
};

/* ---- the kernel's side of it: the boot-arg, the handshake, the claim ---- */

#define VAR_SAVE_NONE   0
#define VAR_SAVE_IDLE   1
#define VAR_SAVE_WANTED 2
#define VAR_SAVE_DONE   3

/* Is NAME=VALUE among the kernel's arguments? (as init.c reads them) */
static int
boot_arg_is(const char *name, const char *value)
{
	static char args[1024];
	size_t len = sizeof(args) - 1, n = strlen(name), v = strlen(value);

	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) {
		return 0;
	}
	args[len < sizeof(args) ? len : sizeof(args) - 1] = 0;
	for (char *p = args; *p;) {
		while (*p == ' ') {
			p++;
		}
		if (strncmp(p, name, n) == 0 && p[n] == '=' && strncmp(p + n + 1, value, v) == 0 &&
		    (p[n + 1 + v] == 0 || p[n + 1 + v] == ' ')) {
			return 1;
		}
		while (*p && *p != ' ') {
			p++;
		}
	}
	return 0;
}

/* The number NAME=<number> in the kernel's arguments is, or 0 */
static long
boot_arg_number(const char *name)
{
	static char args[1024];
	size_t len = sizeof(args) - 1, n = strlen(name);
	long value = 0;

	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) {
		return 0;
	}
	args[len < sizeof(args) ? len : sizeof(args) - 1] = 0;
	for (char *p = args; *p;) {
		while (*p == ' ') {
			p++;
		}
		if (strncmp(p, name, n) == 0 && p[n] == '=') {
			for (p += n + 1; *p >= '0' && *p <= '9'; p++) {
				value = value * 10 + (*p - '0');
			}
			return value;
		}
		while (*p && *p != ' ') {
			p++;
		}
	}
	return 0;
}

/*
 * The keychain (Keychains: securityd's database, every item with its key) is left out as well, saved and unpacked
 * alike, unless boot-arg var_store_keychain=1 asks for it. The kernel's AppleKeyStore stand-in has no secret to wrap
 * the items' keys with (iokit/Kernel/MiniAppleKeyStore.cpp), so in the flash the keychain is as good as plaintext to
 * whoever reads the flash; left out, it is gone at the next start, as it always was before the volume was kept.
 */
#define KEYCHAIN        "Keychains"
static int keychain_wanted = -1;        /* the boot-arg, read once */

static int
keychain_kept(void)
{
	if (keychain_wanted < 0) {
		keychain_wanted = boot_arg_is("var_store_keychain", "1");
	}
	return keychain_wanted;
}

/*
 * boot-arg var_store_skip=PATH,PATH...: these (relative to /private/var, each with what is under it) left out when the
 * store is unpacked, though the archive keeps them: for finding what of a restored data volume a boot cannot live with,
 * by halves, without saving again
 */
static unsigned skipped_load;
static int skip_list_set;

/* PATTERN (N bytes of it) against TEXT: '*' any run of characters, '/' too; "[A-M]" one of a range; anything else itself */
static int
glob_n(const char *pattern, size_t n, const char *text)
{
	if (n == 0) {
		return *text == 0;
	}
	if (*pattern == '*') {
		for (const char *t = text;; t++) {
			if (glob_n(pattern + 1, n - 1, t)) {
				return 1;
			}
			if (*t == 0) {
				return 0;
			}
		}
	}
	if (*pattern == '[' && n >= 5 && pattern[2] == '-' && pattern[4] == ']') {
		return *text >= pattern[1] && *text <= pattern[3] && glob_n(pattern + 5, n - 5, text + 1);
	}
	return *text == *pattern && *text != 0 && glob_n(pattern + 1, n - 1, text + 1);
}

static int
load_skipped(const char *relative)
{
	static char list[1024];
	static int read_once;

	if (!read_once) {
		static char args[1024];
		size_t len = sizeof(args) - 1;
		const char *at;

		read_once = 1;
		if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) == 0) {
			args[len < sizeof(args) ? len : sizeof(args) - 1] = 0;
			for (at = args; *at;) {
				while (*at == ' ') {
					at++;
				}
				if (strncmp(at, "var_store_skip=", 15) == 0) {
					size_t n = 0;

					at += 15;
					while (at[n] != 0 && at[n] != ' ' && n < sizeof(list) - 1) {
						list[n] = at[n];
						n++;
					}
					list[n] = 0;
					skip_list_set = n != 0;
					break;
				}
				while (*at && *at != ' ') {
					at++;
				}
			}
		}
	}
	for (const char *p = list; *p;) {
		size_t n = 0;

		while (p[n] != 0 && p[n] != ',') {
			n++;
		}
		if (n != 0 && strncmp(relative, p, n) == 0 && (relative[n] == 0 || relative[n] == '/')) {
			return 1;
		}
		if (n != 0 && glob_n(p, n, relative)) {
			return 1;   /* (a pattern: mobile/Library/[A-M]* -- what it names and, '*' taking '/', what is under it) */
		}
		p += n;
		if (*p == ',') {
			p++;
		}
	}
	return 0;
}

/* whether var_store_skip names anything at all */
static int
load_skipped_any(void)
{
	(void)load_skipped("\001");    /* (reads the list) */
	return skip_list_set;
}

static long
parse_number(const char *text)
{
	long value = 0;

	for (; *text >= '0' && *text <= '9'; text++) {
		value = value * 10 + (*text - '0');
	}
	return value;
}

static int
sysctl_int(const char *name, int *value)
{
	size_t len = sizeof(*value);

	return sysctlbyname(name, value, &len, NULL, 0);
}

static int
sysctl_set_int(const char *name, int value)
{
	return sysctlbyname(name, NULL, NULL, &value, sizeof(value));
}

/* The claim on the store: nothing may be written without it, and only one program may hold it. */
static int
claim(void)
{
	if (sysctl_set_int("kern.hakios.var_busy", 1) != 0) {
		printf("var_store: the store is claimed by another saver (%s)\n", strerror(errno));
		return 0;
	}
	return 1;
}

static void
release(void)
{
	(void)sysctl_set_int("kern.hakios.var_busy", 0);
}

/* ---- checksum (the same one the loader reads a write back with) ---- */

static uint32_t crc_table[256];

static void
crc_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;

		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
		crc_table[i] = c;
	}
}

static uint32_t
crc_add(uint32_t c, const void *p, uint64_t n)
{
	const uint8_t *b = p;

	for (uint64_t i = 0; i < n; i++) {
		c = crc_table[(c ^ b[i]) & 0xff] ^ (c >> 8);
	}
	return c;
}

static uint32_t
head_checksum(const struct head *h)
{
	struct head copy = *h;

	copy.head_crc = 0;
	return crc_add(0xffffffffu, &copy, sizeof(copy)) ^ 0xffffffffu;
}

/* ---- the store, as a stream of bytes in one slot ---- */

static int fd = -1;
static uint8_t *buffer;
static uint8_t *already;                /* what the slot holds there now, to not write it again */
static uint64_t skipped_blocks, written_blocks;
static uint64_t buffered;               /* bytes in the buffer: to be written, or still to be read */
static uint64_t taken;                  /* of those, already read out (load) */
static off_t at;                        /* where in the device the buffer begins */
static uint64_t stream_bytes;           /* the archive's own bytes so far */
static uint64_t stream_left;            /* bytes of the archive still to come (load) */
static off_t stream_limit;              /* the end of the slot: nothing is read or written past it */
static uint32_t stream_crc;
static uint64_t entries;

static off_t
slot_at(int slot)
{
	return STORE_AT + (off_t)slot * SLOT_BYTES;
}

static int
device_write(off_t where, const void *data, uint64_t n)
{
	const uint8_t *p = data;

	while (n > 0) {
		ssize_t wrote = pwrite(fd, p, (size_t)n, where);

		if (wrote <= 0) {
			printf("var_store: writing %llu bytes at %lld: %s\n", (unsigned long long)n,
			    (long long)where, strerror(errno));
			return 0;
		}
		p += wrote;
		where += wrote;
		n -= (uint64_t)wrote;
	}
	return 1;
}

static int
device_read(off_t where, void *data, uint64_t n)
{
	uint8_t *p = data;

	while (n > 0) {
		ssize_t got = pread(fd, p, (size_t)n, where);

		if (got <= 0) {
			printf("var_store: reading %llu bytes at %lld: %s\n", (unsigned long long)n,
			    (long long)where, strerror(errno));
			return 0;
		}
		p += got;
		where += got;
		n -= (uint64_t)got;
	}
	return 1;
}

/* ---- what each block of a slot held when it was last saved: so a save need not read it back to know ---- */

/*
 * A slot's archive is followed by a table of one 64-bit hash a block of it, and the header block (past the header
 * itself) says where, how long and with what checksum. The save into a slot compares the blocks it is about to write
 * with the table of the archive that is in that slot (the one before the newest), read at the start while its header
 * is still whole, instead of reading the whole slot back: only changed blocks cost anything. The table is only ever
 * trusted while the slot's header checks out and says of the table what the table says of itself; a save that is torn
 * wrote a zero header first, so the next one into that slot reads the blocks back, as before. Old archives have no
 * table and are read back once.
 */
#define TABLE_AT        64                      /* in the header block, after struct head */
#define TABLE_MAGIC     "RDWTBL1"
#define TABLE_MAX       ((uint64_t)SLOT_BYTES / BLOCK)

struct tblhead {
	char     magic[8];
	uint64_t generation, bytes, blocks;
	uint32_t crc;                           /* of the table's bytes */
	uint32_t self_crc;                      /* of this structure with self_crc zero */
};

static uint64_t *new_hash;                      /* the hashes of the blocks being written (NULL: no table is kept) */
static uint64_t *old_hash;                      /* the table of what the slot held */
static uint64_t old_hash_n;                     /* its blocks; 0: none, read the slot back */
static off_t stream_base;                       /* where the archive's first byte is */
static uint64_t hashed_blocks;
static int read_head(int slot, struct head *into);

static uint64_t
hash_block(const uint8_t *p)
{
	uint64_t h = 0x9E3779B97F4A7C15ull;

	for (unsigned i = 0; i < BLOCK; i += 8) {
		uint64_t w;

		memcpy(&w, p + i, 8);
		h = (h ^ w) * 0xff51afd7ed558ccdull;
		h ^= h >> 29;
	}
	return h ^ (h >> 32);
}

static uint32_t
tbl_checksum(const struct tblhead *t)
{
	struct tblhead copy = *t;

	copy.self_crc = 0;
	return crc_add(0xffffffffu, &copy, sizeof(copy)) ^ 0xffffffffu;
}

static uint64_t
table_bytes(uint64_t blocks)
{
	return (blocks * 8 + BLOCK - 1) / BLOCK * BLOCK;
}

/* The table of SLOT's archive into old_hash, if the slot has a good header and a table that agrees with it. */
static void
load_table(int slot)
{
	static uint8_t block[BLOCK];
	struct head h;
	struct tblhead t;
	uint64_t n;

	old_hash_n = 0;
	if (old_hash == NULL || !read_head(slot, &h) || !device_read(slot_at(slot), block, BLOCK)) {
		return;
	}
	memcpy(&t, block + TABLE_AT, sizeof(t));
	if (memcmp(t.magic, TABLE_MAGIC, sizeof(TABLE_MAGIC)) != 0 || t.self_crc != tbl_checksum(&t) ||
	    t.generation != h.generation || t.bytes != h.bytes || t.blocks != (h.bytes + BLOCK - 1) / BLOCK ||
	    t.blocks == 0 || t.blocks > TABLE_MAX) {
		return;
	}
	n = table_bytes(t.blocks);
	if (slot_at(slot) + BLOCK + (off_t)(t.blocks * BLOCK) + (off_t)n > slot_at(slot) + SLOT_BYTES ||
	    !device_read(slot_at(slot) + BLOCK + (off_t)(t.blocks * BLOCK), old_hash, n)) {
		return;
	}
	if ((crc_add(0xffffffffu, old_hash, t.blocks * 8) ^ 0xffffffffu) != t.crc) {
		return;
	}
	old_hash_n = t.blocks;
}

/* ---- writing the archive out ---- */

static int
out_flush(int last)
{
	uint64_t n = buffered;

	if (n == 0) {
		return 1;
	}
	if (last) {
		/* whole blocks only: the tail is padded, and the header says where the archive ends */
		while (n % BLOCK != 0) {
			buffer[n++] = 0;
		}
	}
	if (at + (off_t)n > stream_limit) {
		printf("var_store: the archive does not fit in its slot: nothing more is written\n");
		return 0;
	}
	{
		/*
		 * Each block against what the slot held there: by the old table if there is one, else by reading the slot back
		 * (a volume Darwin has stopped touching packs to the very same bytes, and a save that changes nothing then
		 * costs no writes; flash wears by writing and writes are several times dearer than reads). The blocks that
		 * differ are written in runs, one request a run.
		 */
		uint64_t nb = n / BLOCK, first = (uint64_t)(at - stream_base) / BLOCK, run = 0;
		int have_old = 0;

		if (old_hash_n == 0 && already != NULL && device_read(at, already, n)) {
			have_old = 1;
		}
		for (uint64_t i = 0; i <= nb; i++) {
			int same = 0;

			if (i < nb) {
				uint64_t h = 0;

				if (new_hash != NULL || old_hash_n != 0) {
					h = hash_block(buffer + i * BLOCK);
				}
				if (new_hash != NULL && first + i < TABLE_MAX) {
					new_hash[first + i] = h;
					hashed_blocks = first + i + 1;
				}
				if (old_hash_n != 0) {
					same = first + i < old_hash_n && old_hash[first + i] == h;
				} else if (have_old) {
					same = memcmp(already + i * BLOCK, buffer + i * BLOCK, BLOCK) == 0;
				}
			}
			if (i < nb && !same) {
				run++;
				continue;
			}
			if (run != 0) {
				uint64_t from = i - run;

				if (!device_write(at + (off_t)(from * BLOCK), buffer + from * BLOCK, run * BLOCK)) {
					return 0;
				}
				written_blocks += run;
				run = 0;
			}
			if (i < nb) {
				skipped_blocks++;
			}
		}
	}
	at += (off_t)n;
	buffered = 0;
	return 1;
}

static int
out(const void *data, uint64_t n)
{
	const uint8_t *p = data;

	stream_crc = crc_add(stream_crc, data, n);
	stream_bytes += n;
	while (n > 0) {
		uint64_t room = BUFFER - buffered;

		if (room == 0) {
			if (!out_flush(0)) {
				return 0;
			}
			continue;
		}
		if (room > n) {
			room = n;
		}
		memcpy(buffer + buffered, p, (size_t)room);
		buffered += room;
		p += room;
		n -= room;
	}
	return 1;
}

static int
out_pad(void)
{
	static const uint8_t zero[8] = { 0 };
	uint64_t over = stream_bytes % 8;

	return over == 0 ? 1 : out(zero, 8 - over);
}

/* ---- reading the archive in ---- */

static int
in(void *data, uint64_t n)
{
	uint8_t *p = data;

	if (n > stream_left) {
		printf("var_store: the archive ends in the middle of an entry\n");
		return 0;
	}
	stream_left -= n;
	stream_bytes += n;
	while (n > 0) {
		uint64_t have = buffered - taken;

		if (have == 0) {
			uint64_t want = BUFFER;

			if (at + (off_t)want > stream_limit) {
				want = (uint64_t)(stream_limit - at);
			}
			if (want == 0 || !device_read(at, buffer, want)) {
				return 0;
			}
			at += (off_t)want;
			buffered = want;
			taken = 0;
			continue;
		}
		if (have > n) {
			have = n;
		}
		if (p != NULL) {
			memcpy(p, buffer + taken, (size_t)have);
			p += have;
		}
		stream_crc = crc_add(stream_crc, buffer + taken, have);
		taken += have;
		n -= have;
	}
	return 1;
}

/* n bytes of the archive passed over, their checksum counted in */
static int
in_skip(uint64_t n)
{
	uint8_t waste[512];

	while (n > 0) {
		uint64_t piece = n > sizeof(waste) ? sizeof(waste) : n;

		if (!in(waste, piece)) {
			return 0;
		}
		n -= piece;
	}
	return 1;
}

static int
in_pad(void)
{
	uint64_t over = stream_bytes % 8;

	return over == 0 ? 1 : in_skip(8 - over);
}

static void
stream_start(int slot, uint64_t bytes)
{
	at = stream_base = slot_at(slot) + BLOCK;
	hashed_blocks = 0;
	stream_limit = slot_at(slot) + SLOT_BYTES;
	buffered = taken = 0;
	stream_bytes = 0;
	stream_left = bytes;
	stream_crc = 0xffffffffu;
}

/* ---- the slots ---- */

/*
 * Kept across boots all the same, when the system image is the one the store was saved under (unless boot-arg
 * var_store_ls=0, which drops it at every boot as above): the icon caches (IconServices', in which the Settings list's
 * icons are held) are keyed to the database's entries, and with the database made anew every boot they were rebuilt, late,
 * every boot. The image's identity is written into the archive's header block at save, and compared at load with the
 * image of this boot: the file /System/Library/HakiOS/image-id (mkimage26.py writes one, new at every build), or, for an
 * image built without it, a hash of what an added application changes: SystemVersion.plist's size and time and
 * /Applications' names, sizes and times. A different image, an archive without an identity, or no identity to be had:
 * the database and the icon caches are both dropped, as before, so that they are made again from the image.
 */
#ifndef IMAGE_ID_FILE
#define IMAGE_ID_FILE   "/System/Library/HakiOS/image-id"
#define IMAGE_SYSVER    "/System/Library/CoreServices/SystemVersion.plist"
#define IMAGE_APPS      "/Applications"
#endif
#define ID_AT           256             /* in the header block, after the structure */
#define ID_MAX          96
#define ID_MAGIC        "IMGID1\0"

static int ls_wanted = -1;

static int
ls_on(void)
{
	if (ls_wanted < 0) {
		ls_wanted = !boot_arg_is("var_store_ls", "0");
	}
	return ls_wanted;
}

static int ls_keep;                     /* load: the saved identity is this boot's image's */

/* IconServices' caches: the system group's, and the mobile user's */
static int
is_icon_cache(const char *relative)
{
	static const char *const names[] = {
		"containers/Shared/SystemGroup/systemgroup.com.apple.lsd.iconscache",
		"mobile/Library/Caches/com.apple.IconsCache",
		NULL,
	};

	for (int i = 0; names[i] != NULL; i++) {
		const size_t n = strlen(names[i]);

		if (strncmp(relative, names[i], n) == 0 && relative[n] == '/') {
			return 1;
		}
	}
	return 0;
}

/* The identity of the image this boot runs from, in ID (ID_MAX bytes); 0 when there is none to be had. */
static int
image_identity(char *id)
{
	char text[ID_MAX];
	struct stat st;
	int f = open(IMAGE_ID_FILE, O_RDONLY, 0);

	if (f >= 0) {
		ssize_t got = read(f, text, sizeof(text) - 8);

		close(f);
		while (got > 0 && (text[got - 1] == '\n' || text[got - 1] == ' ' || text[got - 1] == '\r')) {
			got--;
		}
		if (got > 0) {
			memcpy(id, "id:", 3);
			memcpy(id + 3, text, (size_t)got);
			id[3 + got] = 0;
			for (char *c = id; *c != 0; c++) {
				if ((unsigned char)*c < 0x20) {
					*c = '?';
				}
			}
			return 1;
		}
	}
	/* no file: what the image's applications and version say */
	if (lstat(IMAGE_SYSVER, &st) != 0) {
		return 0;
	}
	{
		static char buf[8192];
		off_t position = 0;
		ssize_t got;
		uint32_t sum = 0, count = 0;
		uint32_t base = crc_add(0xffffffffu, &st.st_size, sizeof(st.st_size));
		int dir = open(IMAGE_APPS, O_RDONLY | O_DIRECTORY, 0);

		base = crc_add(base, &st.st_mtimespec.tv_sec, sizeof(st.st_mtimespec.tv_sec));
		if (dir < 0) {
			return 0;
		}
		while ((got = getdirentries(dir, buf, sizeof(buf), &position)) > 0) {
			for (ssize_t at = 0; at < got; at += ((struct dirent *)(buf + at))->d_reclen) {
				const char *name = ((struct dirent *)(buf + at))->d_name;
				char full[MAX_PATH];
				uint32_t c;

				if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
				    sizeof(IMAGE_APPS) + strlen(name) + 1 >= sizeof(full)) {
					continue;
				}
				memcpy(full, IMAGE_APPS "/", sizeof(IMAGE_APPS));
				strcpy(full + sizeof(IMAGE_APPS), name);
				if (lstat(full, &st) != 0) {
					continue;
				}
				c = crc_add(0xffffffffu, name, strlen(name));
				c = crc_add(c, &st.st_size, sizeof(st.st_size));
				c = crc_add(c, &st.st_mtimespec.tv_sec, sizeof(st.st_mtimespec.tv_sec));
				sum += c;       /* (the order of the names does not matter) */
				count++;
			}
		}
		close(dir);
		{
			static const char hex[] = "0123456789abcdef";
			const uint32_t part[3] = { base ^ 0xffffffffu, sum, count };
			char *o = id;

			*o++ = 'f';
			*o++ = 'b';
			for (int k = 0; k < 3; k++) {
				*o++ = ':';
				for (int sh = 28; sh >= 0; sh -= 4) {
					*o++ = hex[(part[k] >> sh) & 15];
				}
			}
			*o = 0;
		}
	}
	return 1;
}

/* the identity a slot's header block holds, if any */
static int
slot_identity(int slot, char *id)
{
	static uint8_t block[BLOCK];

	if (!device_read(slot_at(slot), block, BLOCK) || memcmp(block + ID_AT, ID_MAGIC, 8) != 0 ||
	    block[ID_AT + 8 + ID_MAX - 1] != 0 || block[ID_AT + 8] == 0) {
		return 0;
	}
	memcpy(id, block + ID_AT + 8, ID_MAX);
	return 1;
}


/* The slot's header, if it is one of ours and says of itself what it should. */
static int
read_head(int slot, struct head *into)
{
	static uint8_t block[BLOCK];
	struct head *h = (struct head *)block;

	if (!device_read(slot_at(slot), block, BLOCK)) {
		return 0;
	}
	if (memcmp(h->magic, HEAD_MAGIC, sizeof(h->magic)) != 0 || h->version != HEAD_VERSION ||
	    h->slot != (uint32_t)slot || h->bytes > (uint64_t)SLOT_BYTES - BLOCK ||
	    h->head_crc != head_checksum(h)) {
		return 0;
	}
	*into = *h;
	return 1;
}

/* The newest slot whose header checks out, or -1. */
static int
newest(struct head *out_head)
{
	int best = -1;

	for (int slot = 0; slot < SLOTS; slot++) {
		struct head h;

		if (!read_head(slot, &h)) {
			continue;
		}
		if (best < 0 || h.generation > out_head->generation) {
			best = slot;
			*out_head = h;
		}
	}
	return best;
}

/* ---- packing ---- */

/*
 * How much of this save is new since the last one: an entry whose time of writing is later than
 * the second the previous generation was saved at. It costs nothing to count -- the walk has every
 * entry's time in hand anyway -- and it is what says whether saving only the difference would be
 * worth building, or whether Darwin rewrites the volume faster than any such thing could help.
 */
static uint64_t last_saved_at, changed_entries, changed_bytes;

static char path[MAX_PATH];               /* the whole path, under ROOT */
static struct { uint64_t ino; char path[LINK_PATH]; } *links;
static uint64_t skipped_kinds, skipped_big;

static int diag_wanted = -1;

static int
diag_kept(void)
{
	if (diag_wanted < 0) {
		diag_wanted = boot_arg_is("var_store_diag", "1");
	}
	return diag_wanted;
}

static int lean_wanted = -1;

static int
lean_on(void)
{
	if (lean_wanted < 0) {
		lean_wanted = boot_arg_is("var_store_lean", "1");
	}
	return lean_wanted;
}

static int
is_skipped(const char *relative)
{
	for (int i = 0; SKIP[i] != NULL; i++) {
		if (strcmp(relative, SKIP[i]) == 0) {
			return 1;
		}
	}
	for (int i = 0; !diag_kept() && SKIP_DIAG[i] != NULL; i++) {
		if (strcmp(relative, SKIP_DIAG[i]) == 0) {
			return 1;
		}
	}
	for (int i = 0; lean_on() && SKIP_LEAN[i] != NULL; i++) {
		if (strcmp(relative, SKIP_LEAN[i]) == 0) {
			return 1;
		}
	}
	return strcmp(relative, KEYCHAIN) == 0 && !keychain_kept();
}

/*
 * The name this inode was first met under, if it has one; otherwise, with remember, this one is remembered. Only a name
 * whose contents went into the archive is: the inode's other names are packed as links to it, and a link to a name
 * the archive does not hold is restored as nothing at all.
 */
static const char *
link_name(uint64_t ino, const char *relative, int remember)
{
	uint64_t slot = (ino * 2654435761u) % LINKS;

	for (uint64_t tries = 0; tries < LINKS; tries++, slot = (slot + 1) % LINKS) {
		if (links[slot].ino == ino) {
			return links[slot].path;
		}
		if (links[slot].ino == 0) {
			if (remember && strlen(relative) < LINK_PATH) {
				links[slot].ino = ino;
				strcpy(links[slot].path, relative);
			}
			return NULL;
		}
	}
	return NULL;                    /* the table is full: this one is written out whole */
}

/*
 * The extended attributes of path -- Darwin keeps container metadata and much else in them -- packed
 * into a buffer of their own, because the entry's head says how many there are and has to go out
 * before them. One that does not fit is left out and counted.
 */
static uint8_t xpack[XPACK];
#define ACL_NAME        "com.hakios.acl"        /* the access control list in the archive (pack_xattrs) */
static uint8_t acl[MAX_XATTR];                  /* the entry being made's, put on after its owner and mode */
static size_t acl_size;

static uint64_t
pack_xattrs(uint32_t *count)
{
	static char names[16384];
	static uint8_t value[MAX_XATTR];
	ssize_t n = listxattr(path, names, sizeof(names), XATTR_NOFOLLOW);
	uint64_t used = 0;

	*count = 0;
	for (ssize_t i = 0; n > 0 && i < n && i < (ssize_t)sizeof(names); i += (ssize_t)strlen(names + i) + 1) {
		uint32_t namelen = (uint32_t)strlen(names + i), vlen;
		ssize_t size;

		/* (com.apple.system.*, the kernel's own -- the access control list among them -- are not read or written as
		 * attributes: the list is taken below) */
		if (strncmp(names + i, "com.apple.system.", 17) == 0) {
			continue;
		}
		/*
		 * (an attribute of that name set by whoever can write the file is not taken: the restore, as root, would install it
		 * as the file's access control list. The real list is packed below, from lstat_extended)
		 */
		if (strcmp(names + i, ACL_NAME) == 0) {
			continue;
		}
		/*
		 * (no more than load reads, one kept for the list below: a file of anyone's with thousands of tiny attributes
		 * would otherwise make the whole archive one that load refuses -- every file of the volume lost to one)
		 */
		if (*count >= XATTRS - 1) {
			skipped_big++;
			continue;
		}
		size = getxattr(path, names + i, value, sizeof(value), 0, XATTR_NOFOLLOW);
		if (size < 0) {
			continue;
		}
		vlen = (uint32_t)size;
		if (used + 8 + namelen + vlen > XPACK) {
			skipped_big++;
			continue;
		}
		memcpy(xpack + used, &namelen, sizeof(namelen));
		memcpy(xpack + used + 4, &vlen, sizeof(vlen));
		memcpy(xpack + used + 8, names + i, namelen);
		memcpy(xpack + used + 8 + namelen, value, vlen);
		used += 8 + namelen + vlen;
		(*count)++;
	}
	/*
	 * The access control list, as an attribute of ours (ACL_NAME): the kernel's struct kauth_filesec as lstat_extended
	 * gives it, put back by chmod_extended. iOS 27's container manager makes its containers root's, 755, and gives the
	 * program they are for its access by a list (systemgroup.com.apple.installcoordinationd and its Library, mode 0,
	 * nobody's): without the list a restored volume had installcoordinationd abort at every start ("Failed to create
	 * data directory", EACCES) and SpringBoard never answer a touch.
	 */
	{
		struct stat st;
		size_t size = sizeof(value);
		const uint32_t namelen = (uint32_t)strlen(ACL_NAME);

		if (lstat_extended(path, &st, value, &size) == 0 && size > 0 && size <= sizeof(value) &&
		    used + 8 + namelen + size <= XPACK) {
			const uint32_t vlen = (uint32_t)size;

			memcpy(xpack + used, &namelen, sizeof(namelen));
			memcpy(xpack + used + 4, &vlen, sizeof(vlen));
			memcpy(xpack + used + 8, ACL_NAME, namelen);
			memcpy(xpack + used + 8 + namelen, value, vlen);
			used += 8 + namelen + vlen;
			(*count)++;
		}
	}
	return used;
}

/* One entry: its head, its name, its extended attributes, its contents. */
/* ---- where the bytes are: a summary at the end of every save, for finding what grows ---- */

#define BUCKETS 4096                    /* a power of two: a hash table, probed linearly */
static struct bucket { char name[80]; uint64_t bytes, entries; } buckets[BUCKETS];
static uint64_t skipped_logs, skipped_log_bytes, dropped_buckets;

static void
account_to(const char *name, size_t n, uint64_t bytes)
{
	uint32_t h = 2166136261u;

	if (n >= sizeof(buckets[0].name)) {
		return;
	}
	for (size_t i = 0; i < n; i++) {
		h = (h ^ (uint8_t)name[i]) * 16777619u;
	}
	for (uint32_t tries = 0; tries < BUCKETS; tries++) {
		struct bucket *b = &buckets[(h + tries) % BUCKETS];

		if (b->name[0] == 0) {
			memcpy(b->name, name, n);
			b->name[n] = 0;
			b->bytes = bytes;
			b->entries = 1;
			return;
		}
		if (strlen(b->name) == n && memcmp(b->name, name, n) == 0) {
			b->bytes += bytes;
			b->entries++;
			return;
		}
	}
	dropped_buckets++;
}

/*
 * a file or whatever of SIZE bytes under RELATIVE: counted for each of its leading directories, three levels deep
 * (mobile/Library/Caches...), five under the per-service temp and cache directories and the shared containers, where
 * the bytes of this volume have been (dirs_cleaner/1O/com.apple.audiomxd/...)
 */
static void
account(const char *relative, uint64_t size)
{
	size_t n = 0;
	int level = 0, deepest = strncmp(relative, "dirs_cleaner", 12) == 0 || strncmp(relative, "containers", 10) == 0 ? 5 : 3;

	for (; relative[n] != 0; n++) {
		if (relative[n] == '/') {
			account_to(relative, n, size);
			if (++level == deepest) {
				return;
			}
		}
	}
	account_to(relative, n, size);
}

static void
print_summary(void)
{
	/* the biggest forty (a megabyte at least; half of one deep down), by selection */
	for (int rank = 0; rank < 40; rank++) {
		struct bucket *best = NULL;

		for (int i = 0; i < BUCKETS; i++) {
			if (buckets[i].name[0] != 0 && (best == NULL || buckets[i].bytes > best->bytes)) {
				best = &buckets[i];
			}
		}
		if (best == NULL || best->bytes < (512u << 10)) {
			break;
		}
		printf("var_store: size: %llu bytes %llu entries %s\n", (unsigned long long)best->bytes,
		    (unsigned long long)best->entries, best->name);
		best->name[0] = 0;     /* (open addressing: the table is not probed again this save) */
	}
	if (dropped_buckets != 0) {
		printf("var_store: size: %llu names did not fit the summary\n", (unsigned long long)dropped_buckets);
	}
	if (skipped_logs != 0) {
		printf("var_store: size: %llu log files over %u bytes, %llu bytes, left out (var_store_diag=1 keeps them)\n",
		    (unsigned long long)skipped_logs, LOG_CAP, (unsigned long long)skipped_log_bytes);
	}
}

/* a name that is a log by its ending */
static int
is_log_name(const char *relative)
{
	static const char *const endings[] = { ".log", ".crash", ".ips", ".asl", ".core", ".trace", ".tracev3", ".log.gz", ".ips.ca.synced", NULL };
	size_t n = strlen(relative);

	for (int i = 0; endings[i] != NULL; i++) {
		size_t m = strlen(endings[i]);

		if (n > m && strcmp(relative + n - m, endings[i]) == 0) {
			return 1;
		}
	}
	return 0;
}

/* LENGTH bytes of nothing, as an entry that puts the stream on a boundary after it: used on both sides of a big file */
static int
pack_pad_to(uint64_t extra_before_boundary)
{
	struct entry e;
	static const uint8_t zero[512];
	uint64_t t = (ALIGN - (stream_bytes + extra_before_boundary) % ALIGN) % ALIGN;

	if (t == 0) {
		return 1;
	}
	if (t < sizeof(e)) {
		t += ALIGN;
	}
	memset(&e, 0, sizeof(e));
	e.kind = ENT_PAD;
	e.size = t - sizeof(e);
	if (!out(&e, sizeof(e))) {
		return 0;
	}
	for (uint64_t left = e.size; left > 0;) {
		uint64_t piece = left > sizeof(zero) ? sizeof(zero) : left;

		if (!out(zero, piece)) {
			return 0;
		}
		left -= piece;
	}
	entries++;
	return 1;
}

static int
pack_one(const char *relative, const struct stat *st)
{
	struct entry e;
	static char target[MAX_PATH];
	const char *first;
	uint32_t kind = 0;
	uint64_t size = 0;
	const void *tail = NULL;
	int in_fd = -1;

	switch (st->st_mode & S_IFMT) {
	case S_IFDIR:
		kind = ENT_DIR;
		break;
	case S_IFLNK: {
		ssize_t n = readlink(path, target, sizeof(target));

		if (n <= 0) {
			return 1;
		}
		kind = ENT_SYMLINK;
		size = (uint64_t)n;
		tail = target;
		break;
	}
	case S_IFREG:
		kind = ENT_FILE;
		size = (uint64_t)st->st_size;
		if (st->st_nlink > 1) {
			first = link_name(st->st_ino, relative, 0);
			if (first != NULL) {
				kind = ENT_HARDLINK;
				size = strlen(first);
				tail = first;
				break;
			}
		}
		/*
		 * (not through a symbolic link, and the very file that was looked at: one swapped for a link to a file of
		 * root's between the lstat and here would go into the archive with this one's owner and mode)
		 */
		if (size > LOG_CAP && !diag_kept() && is_log_name(relative)) {
			skipped_logs++;
			skipped_log_bytes += size;
			return 1;       /* a big log: what a run said, not what it keeps */
		}
		in_fd = open(path, O_RDONLY | VS_O_NOFOLLOW, 0);
		if (in_fd < 0) {
			return 1;       /* gone, or not ours to read: it is not in the archive */
		}
		{
			struct stat now;

			if (fstat(in_fd, &now) != 0 || now.st_dev != st->st_dev || now.st_ino != st->st_ino ||
			    (now.st_mode & S_IFMT) != S_IFREG) {
				close(in_fd);
				changed_under++;
				return 1;
			}
			size = (uint64_t)now.st_size;
			/* (the name the inode's other names will be links to: this one, now that it is certain to be packed) */
			if (st->st_nlink > 1) {
				(void)link_name(st->st_ino, relative, 1);
			}
		}
		break;
	default:
		skipped_kinds++;        /* a socket, a fifo, a device: a new boot makes its own */
		return 1;
	}

	memset(&e, 0, sizeof(e));
	e.kind = kind;
	e.mode = st->st_mode & 07777;
	e.uid = st->st_uid;
	e.gid = st->st_gid;
	e.flags = st->st_flags & FLAGS_KEPT;
	e.pathlen = (uint32_t)strlen(relative);
	e.mtime_sec = st->st_mtimespec.tv_sec;
	e.mtime_nsec = (uint32_t)st->st_mtimespec.tv_nsec;
	e.size = size;
	if (last_saved_at != 0 && (uint64_t)st->st_mtimespec.tv_sec > last_saved_at) {
		changed_entries++;
		changed_bytes += size;
	}
	{
		uint64_t xlen = pack_xattrs(&e.nxattr);

		/* a big file's data on a boundary, so the blocks it fills are the same blocks next time whatever came before it */
		if (in_fd >= 0 && size >= ALIGN_MIN && !pack_pad_to(sizeof(e) + e.pathlen + xlen)) {
			close(in_fd);
			return 0;
		}
		account(relative, size);
		if (!out(&e, sizeof(e)) || !out(relative, e.pathlen) || !out(xpack, xlen)) {
			if (in_fd >= 0) {
				close(in_fd);
			}
			return 0;
		}
	}
	if (tail != NULL && !out(tail, size)) {
		if (in_fd >= 0) {
			close(in_fd);
		}
		return 0;
	}
	if (in_fd >= 0) {
		static uint8_t piece[256 * 1024];
		uint64_t left = size;

		while (left > 0) {
			uint64_t want = left > sizeof(piece) ? sizeof(piece) : left;
			ssize_t got = read(in_fd, piece, (size_t)want);

			if (got <= 0) {
				/* the file shrank under us: the rest is zeros, so the archive stays whole */
				memset(piece, 0, (size_t)want);
				got = (ssize_t)want;
			}
			if (!out(piece, (uint64_t)got)) {
				close(in_fd);
				return 0;
			}
			left -= (uint64_t)got;
		}
		close(in_fd);
	}
	if (!out_pad()) {
		return 0;
	}
	entries++;
	if (size >= ALIGN_MIN && kind == ENT_FILE && !pack_pad_to(0)) {
		return 0;       /* (and the next entry on a boundary too: the file growing a little moves nothing after it) */
	}
	return 1;
}

/* The directory's own times once more, after its contents: making them moved them. */
static int
pack_dirtime(const char *relative, const struct stat *st)
{
	struct entry e;

	memset(&e, 0, sizeof(e));
	e.kind = ENT_DIRTIME;
	e.flags = st->st_flags & FLAGS_KEPT;
	e.pathlen = (uint32_t)strlen(relative);
	e.mtime_sec = st->st_mtimespec.tv_sec;
	e.mtime_nsec = (uint32_t)st->st_mtimespec.tv_nsec;
	if (!out(&e, sizeof(e)) || !out(relative, e.pathlen) || !out_pad()) {
		return 0;
	}
	entries++;
	return 1;
}

/*
 * The names of one directory, gathered and sorted. The archive is meant to come out the same
 * bytes for the same tree: the order getdirentries gives shifts as a directory is written to, and
 * then every byte after it in the archive moves, which is the difference between a save that
 * writes nothing into the flash and one that writes all of itself. Both arenas are used as stacks
 * -- a level marks them on the way in and gives them back on the way out.
 */
#define NAME_ARENA      (2u << 20)
#define NAME_MAX_NAMES  65536

static char name_arena[NAME_ARENA];
static uint32_t name_at[NAME_MAX_NAMES];
static uint64_t arena_used, names_used, unsorted_dirs;

static void
sort_names(uint32_t *at, uint64_t n)
{
	/* insertion sort: a directory holds tens of names, and this keeps the program small */
	for (uint64_t i = 1; i < n; i++) {
		uint32_t v = at[i];
		uint64_t j = i;

		while (j > 0 && strcmp(name_arena + at[j - 1], name_arena + v) > 0) {
			at[j] = at[j - 1];
			j--;
		}
		at[j] = v;
	}
}

/* Every name in the open directory into the arena, sorted. Returns the count, or -1 if it did not fit. */
static long
gather_names(int dir_fd, char *buf, size_t bufsize, uint64_t mark_a, uint64_t mark_n)
{
	off_t position = 0;
	ssize_t n;

	while ((n = getdirentries(dir_fd, buf, bufsize, &position)) > 0) {
		for (ssize_t i = 0; i < n;) {
			struct dirent *d = (struct dirent *)(buf + i);
			const char *name = d->d_name;
			size_t len = strlen(name);

			i += d->d_reclen;
			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
				continue;
			}
			if (arena_used + len + 1 > NAME_ARENA || names_used >= NAME_MAX_NAMES) {
				arena_used = mark_a;
				names_used = mark_n;
				return -1;
			}
			name_at[names_used++] = (uint32_t)arena_used;
			strcpy(name_arena + arena_used, name);
			arena_used += len + 1;
		}
	}
	sort_names(name_at + mark_n, names_used - mark_n);
	return (long)(names_used - mark_n);
}

/* relative: the path under ROOT, "" for ROOT itself. path holds the whole of it. */
static int
pack_tree(const char *relative)
{
	static char dirbuf[MAX_DEPTH][4096];    /* one read buffer a level */
	static int depth;
	struct stat st;
	size_t base = strlen(path), rbase = strlen(relative);
	int dir_fd, ok = 1;

	if (lstat(path, &st) != 0) {
		return 1;
	}
	if (relative[0] != 0 && !pack_one(relative, &st)) {
		return 0;
	}
	if ((st.st_mode & S_IFMT) != S_IFDIR || (relative[0] != 0 && is_skipped(relative))) {
		return 1;
	}
	if (depth >= MAX_DEPTH) {
		printf("var_store: %s: deeper than this program walks\n", path);
		return 1;
	}
	if ((dir_fd = open(path, O_RDONLY | O_DIRECTORY | VS_O_NOFOLLOW, 0)) < 0) {
		return 1;       /* (gone, or swapped for a link meanwhile: not followed out of the tree) */
	}
	depth++;
	{
		uint64_t mark_a = arena_used, mark_n = names_used;
		long count = gather_names(dir_fd, dirbuf[depth - 1], sizeof(dirbuf[0]), mark_a, mark_n);

		if (count < 0) {
			/*
			 * (more names than the arena holds: none of them was kept, and the walk below would pack nothing of this
			 * directory -- the save would go on and write over the older slot without it. The save fails instead,
			 * loudly, and the slot that holds the directory stays)
			 */
			unsorted_dirs++;
			printf("var_store: %s: more names than this program holds at once: NOT SAVED (the last save stays)\n", path);
			ok = 0;
		}
		for (long k = 0; ok && k < count; k++) {
			const char *name = name_arena + name_at[mark_n + k];
			char child[MAX_PATH];           /* the caller's while this one recurses: not static */

			if (base + 1 + strlen(name) >= MAX_PATH || rbase + 1 + strlen(name) >= MAX_PATH) {
				printf("var_store: %s/%s: the path is too long\n", path, name);
				continue;
			}
			path[base] = '/';
			strcpy(path + base + 1, name);
			if (rbase > 0) {
				memcpy(child, relative, rbase);
				child[rbase] = '/';
				strcpy(child + rbase + 1, name);
			} else {
				strcpy(child, name);
			}
			if (ls_on() || !is_launchservices_store(child)) {
				ok = pack_tree(child);
			}
			path[base] = 0;
		}
		arena_used = mark_a;
		names_used = mark_n;
	}
	depth--;
	close(dir_fd);
	if (ok && relative[0] != 0 && lstat(path, &st) == 0) {
		ok = pack_dirtime(relative, &st);
	}
	return ok;
}

/*
 * The slot and generation whose archive this process has read through and found whole, or written itself (each block
 * of a write is read back before it is answered): what save() need not read again.
 */
static int good_slot = -1;
static uint64_t good_generation;
static int pass(const struct head *h, int make);

/* Whether the archive SLOT's header H describes reads back whole: read through once, then remembered. */
static int
archive_whole(int slot, const struct head *h)
{
	if (slot == good_slot && h->generation == good_generation) {
		return 1;
	}
	if (!pass(h, 0)) {
		return 0;
	}
	good_slot = slot;
	good_generation = h->generation;
	return 1;
}

/*
 * The slot a save writes, OLD the newest header and *BEST its slot (-1: none). It is the one NOT holding the newest good
 * archive. newest() goes by the headers; a header that checks out over an archive that does not (a save torn after its
 * header -- or its last blocks -- reached the flash) is one load passes over for the slot before it. Writing that slot then
 * would leave nothing good at all: the torn one is written instead, and the one load took is kept.
 */
static int
save_slot(struct head *old, int *best_out)
{
	int best = newest(old), slot = best < 0 ? 0 : (best + 1) % SLOTS;

	if (best >= 0 && !archive_whole(best, old)) {
		printf("var_store: slot %d (generation %llu) does not read back whole: it is the one written, and slot %d is kept\n",
		    best, (unsigned long long)old->generation, (best + 1) % SLOTS);
		slot = best;
	}
	*best_out = best;
	return slot;
}

static int
save(void)
{
	struct head old, h;
	int slot, best;
	static uint8_t block[BLOCK];

	memset(&old, 0, sizeof(old));
	slot = save_slot(&old, &best);
	printf("var_store: saving into slot %d (slot %d holds generation %llu)\n", slot, best,
	    best < 0 ? 0ULL : (unsigned long long)old.generation);
	printf("var_store: the keychain is %s\n", keychain_kept() ? "kept too (var_store_keychain=1), and not encrypted" :
	    "left out (var_store_keychain=1 keeps it)");

	/* what the slot holds, by its table, while its header is still whole */
	if (boot_arg_is("var_store_full", "1")) {
		old_hash_n = 0;
	} else {
		load_table(slot);
	}
	printf("var_store: the slot's old archive is %s\n", old_hash_n != 0 ? "compared by its table of block hashes" :
	    "read back to compare (no table)");
	/* the slot's header first, so that a save cut short cannot leave an old header over a new archive */
	memset(block, 0, sizeof(block));
	if (!device_write(slot_at(slot), block, BLOCK)) {
		return 0;
	}
	stream_start(slot, 0);
	entries = 0;
	last_saved_at = best < 0 ? 0 : old.seconds;
	changed_entries = changed_bytes = 0;
	skipped_blocks = written_blocks = 0;
	arena_used = names_used = unsorted_dirs = 0;
	/* a watcher saves more than once: nothing of the save before is carried into this one */
	memset(links, 0, LINKS * sizeof(*links));
	skipped_kinds = skipped_big = 0;
	skipped_logs = skipped_log_bytes = dropped_buckets = 0;
	memset(buckets, 0, sizeof(buckets));
	changed_under = 0;
	strcpy(path, ROOT);
	if (!pack_tree("") || !out_flush(1)) {
		return 0;
	}
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, HEAD_MAGIC, sizeof(h.magic));
	h.version = HEAD_VERSION;
	h.slot = (uint32_t)slot;
	h.generation = (best < 0 ? 0 : old.generation) + 1;
	h.bytes = stream_bytes;
	h.entries = entries;
	{
		struct timeval tv = { 0, 0 };

		gettimeofday(&tv, NULL);
		h.seconds = (uint64_t)tv.tv_sec;
	}
	h.crc = stream_crc ^ 0xffffffffu;
	h.head_crc = head_checksum(&h);
	if (h.bytes > (uint64_t)SLOT_BYTES - BLOCK) {
		printf("var_store: the archive is %llu bytes, more than a slot holds (%llu): nothing saved\n",
		    (unsigned long long)h.bytes, (unsigned long long)(SLOT_BYTES - BLOCK));
		return 0;
	}
	memset(block, 0, sizeof(block));
	memcpy(block, &h, sizeof(h));
	{
		char id[ID_MAX];

		if (ls_on() && image_identity(id)) {
			memcpy(block + ID_AT, ID_MAGIC, 8);
			memcpy(block + ID_AT + 8, id, ID_MAX);
			printf("var_store: the image is %s\n", id);
		} else {
			printf("var_store: no image identity kept (LaunchServices store and icon caches are dropped at the next boot)\n");
		}
	}
	/* the table of the blocks, after the archive and before the header that vouches for both */
	if (new_hash != NULL && hashed_blocks == (h.bytes + BLOCK - 1) / BLOCK && hashed_blocks != 0) {
		struct tblhead t;
		uint64_t tb = table_bytes(hashed_blocks);
		off_t tat = slot_at(slot) + BLOCK + (off_t)(hashed_blocks * BLOCK);

		memset((uint8_t *)new_hash + hashed_blocks * 8, 0, tb - hashed_blocks * 8);
		if (tat + (off_t)tb <= slot_at(slot) + SLOT_BYTES && device_write(tat, new_hash, tb)) {
			memset(&t, 0, sizeof(t));
			memcpy(t.magic, TABLE_MAGIC, sizeof(TABLE_MAGIC));
			t.generation = h.generation;
			t.bytes = h.bytes;
			t.blocks = hashed_blocks;
			t.crc = crc_add(0xffffffffu, new_hash, hashed_blocks * 8) ^ 0xffffffffu;
			t.self_crc = tbl_checksum(&t);
			memcpy(block + TABLE_AT, &t, sizeof(t));
		}
	}
	if (!device_write(slot_at(slot), block, BLOCK)) {
		return 0;
	}
	print_summary();
	printf("var_store: slot %d: generation %llu, %llu entries, %llu bytes, crc %08x%s%s\n", slot,
	    (unsigned long long)h.generation, (unsigned long long)h.entries, (unsigned long long)h.bytes,
	    h.crc, skipped_kinds ? " (sockets and the like left out)" : "",
	    skipped_big ? " (something too big left out)" : "");
	if (last_saved_at != 0) {
		printf("var_store: %llu entries and %llu bytes of it were written since the last save "
		    "(%llu seconds ago)\n", (unsigned long long)changed_entries,
		    (unsigned long long)changed_bytes, (unsigned long long)(h.seconds - last_saved_at));
	}
	printf("var_store: %llu blocks into the flash, %llu left alone (they held this already)%s\n",
	    (unsigned long long)written_blocks, (unsigned long long)skipped_blocks,
	    unsorted_dirs ? " (a directory was too big to put in order)" : "");
	if (changed_under != 0) {
		printf("var_store: %u files were another by the time they were opened: left out\n", changed_under);
	}
	good_slot = slot;
	good_generation = h.generation;
	return 1;
}

/* ---- unpacking ---- */

static uint64_t made_dirs, made_files, made_links, made_bytes, short_files, lost_bytes;
/*
 * A belt, after the braces: the memory filesystem no longer answers ENOSPC for want of a page
 * (bsd/miscfs/ramfs, ramfs_grow_slots now takes an array found without the mount lock), so a short
 * write here means the volume really is full. Waiting a little and trying again still earns its
 * place -- the pageout path cannot wait for an array either, and it may be what answered -- but
 * the whole restore spends at most RETRY_MS on it, so a volume that is truly full does not turn
 * the boot into a crawl.
 */
#define RETRY_MS        2000
static uint64_t retry_ms_left = RETRY_MS;

static int
whole_path(const char *relative, uint32_t len)
{
	if (len == 0 || len >= MAX_PATH - sizeof(ROOT) - 1 || relative[0] == '/') {
		return 0;
	}
	for (uint32_t i = 0; i + 1 < len; i++) {
		if (relative[i] == '.' && relative[i + 1] == '.' &&
		    (i == 0 || relative[i - 1] == '/') && (i + 2 == len || relative[i + 2] == '/')) {
			return 0;       /* nothing of this archive is unpacked outside ROOT */
		}
	}
	strcpy(path, ROOT);
	path[sizeof(ROOT) - 1] = '/';
	memcpy(path + sizeof(ROOT), relative, len);
	path[sizeof(ROOT) + len] = 0;
	return 1;
}

static void
set_times(const struct entry *e)
{
	struct timeval times[2];

	times[0].tv_sec = (long)e->mtime_sec;
	times[0].tv_usec = (int)(e->mtime_nsec / 1000);
	times[1] = times[0];
	utimes(path, times);
}

/*
 * A directory of the archive that init had already made something else of -- a symbolic link (/private/var/jb, with the
 * root on the flash, leads into /private/preboot) or a file: it is left as init made it, and nothing of the archive goes
 * in under it. Unpacked as root, the directory's owner, mode and flags would otherwise land on whatever the link leads
 * to, outside /private/var, and its files would be written there.
 */
static char not_made_dir[MAX_PATH];

/*
 * One entry out of the archive and into /private/var. make: with 0, the entry is only read past
 * (the first pass, which proves the archive's checksum before anything is made).
 */
static int
unpack_one(int make)
{
	struct entry e;
	static char relative[MAX_PATH];
	static uint8_t value[MAX_XATTR];
	static char name[1024];
	int out_fd = -1, ok = 1, made_file = 0;
	struct stat there;

	if (!in(&e, sizeof(e))) {
		return 0;
	}
	if (e.kind == ENT_PAD) {
		return in_skip(e.size);
	}
	if (e.pathlen >= MAX_PATH || e.nxattr > XATTRS) {
		printf("var_store: an entry this program cannot read (kind %u, path %u bytes)\n", e.kind,
		    e.pathlen);
		return 0;
	}
	if (!in(relative, e.pathlen)) {
		return 0;
	}
	relative[e.pathlen] = 0;
	if (make && !whole_path(relative, e.pathlen)) {
		make = 0;               /* read past it: an archive is not to make names of its own choosing */
	}
	if (make && load_skipped(relative)) {
		make = 0;               /* (var_store_skip) */
		skipped_load++;
	}
	/* (what is under a directory of SKIP is not put back either: an archive saved before it was listed holds it) */
	for (int i = 0; make && SKIP[i] != NULL; i++) {
		const size_t n = strlen(SKIP[i]);

		if (strncmp(relative, SKIP[i], n) == 0 && relative[n] == '/') {
			make = 0;
			skipped_load++;
		}
	}
	/* (the diagnostic list too: an archive from before it holds hundreds of MB of it, and the boot would read it back for nothing) */
	for (int i = 0; make && !diag_kept() && SKIP_DIAG[i] != NULL; i++) {
		const size_t n = strlen(SKIP_DIAG[i]);

		if (strncmp(relative, SKIP_DIAG[i], n) == 0 && relative[n] == '/') {
			make = 0;
			skipped_load++;
		}
	}
	for (int i = 0; make && lean_on() && SKIP_LEAN[i] != NULL; i++) {
		const size_t n = strlen(SKIP_LEAN[i]);

		if (strncmp(relative, SKIP_LEAN[i], n) == 0 && relative[n] == '/') {
			make = 0;
			skipped_load++;
		}
	}
	/* (nor the keychain an archive saved with var_store_keychain=1 holds, unless this boot asks for it too) */
	if (make && strncmp(relative, KEYCHAIN "/", sizeof(KEYCHAIN)) == 0 && !keychain_kept()) {
		make = 0;
		skipped_load++;
	}
	if (make && !ls_keep && (is_launchservices_store(relative) || (ls_on() && is_icon_cache(relative)))) {
		make = 0;
		skipped_load++;
	}
	if (make && not_made_dir[0] != 0) {
		const size_t n = strlen(not_made_dir);

		/* (what is under it, and its own times and flags at the end of it) */
		if (strncmp(relative, not_made_dir, n) == 0 && relative[n] == '/') {
			make = 0;
			skipped_load++;
		}
	}
	if (make) {
		switch (e.kind) {
		case ENT_DIR:
			if (mkdir(path, (int)e.mode) == 0) {
				made_dirs++;
			} else if (lstat(path, &there) != 0 || (there.st_mode & S_IFMT) != S_IFDIR) {
				printf("var_store: %s is not a directory here: left as it is, and what the store has under it too\n",
				    path);
				strcpy(not_made_dir, relative);
				make = 0;
			}
			break;
		case ENT_FILE:
			/* (a symbolic link the skeleton made in its place is replaced, not written through: the store has a file) */
			if (lstat(path, &there) == 0 && (there.st_mode & S_IFMT) == S_IFLNK) {
				unlink(path);
			}
			out_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | VS_O_NOFOLLOW, (int)e.mode);
			if (out_fd < 0) {
				printf("var_store: %s cannot be made: %s\n", path, strerror(errno));
			} else {
				made_files++;
				made_file = 1;
			}
			break;
		default:
			break;
		}
	}
	/* the attributes, before the contents, as they were packed */
	acl_size = 0;   /* (this entry's list, if it has one) */
	for (uint32_t i = 0; i < e.nxattr; i++) {
		uint32_t namelen, vlen;

		if (!in(&namelen, sizeof(namelen)) || !in(&vlen, sizeof(vlen))) {
			ok = 0;
			break;
		}
		if (namelen >= sizeof(name) || vlen > sizeof(value)) {
			ok = in_skip(namelen) && in_skip(vlen);
			skipped_big++;
			continue;
		}
		if (!in(name, namelen) || !in(value, vlen)) {
			ok = 0;
			break;
		}
		name[namelen] = 0;
		if (make && e.kind != ENT_HARDLINK && strcmp(name, ACL_NAME) == 0) {
			memcpy(acl, value, vlen);
			acl_size = vlen;
		} else if (make && e.kind != ENT_HARDLINK) {
			setxattr(path, name, value, vlen, 0, XATTR_NOFOLLOW);
		}
	}
	if (!ok) {
		if (out_fd >= 0) {
			close(out_fd);
		}
		return 0;
	}
	/* the contents */
	if (e.kind == ENT_FILE) {
		static uint8_t piece[256 * 1024];
		uint64_t left = e.size, in_file = 0;

		while (left > 0) {
			uint64_t want = left > sizeof(piece) ? sizeof(piece) : left, done = 0;

			if (!in(piece, want)) {
				ok = 0;
				break;
			}
			/*
			 * A whole data volume goes in at once, faster than the memory filesystem can find
			 * pages for it, and it then says ENOSPC for want of one page rather than for want
			 * of room (bsd/miscfs/ramfs, ramfs_grow_slots and the page limit). So a short write
			 * is waited out and tried again, and only a file that will not go in at all after a
			 * second of trying is given up on -- with its name, so it is known which.
			 */
			while (out_fd >= 0 && done < want) {
				ssize_t wrote = write(out_fd, piece + done, (size_t)(want - done));

				if (wrote > 0) {
					done += (uint64_t)wrote;
					continue;
				}
				if (retry_ms_left < 5) {
					printf("var_store: %s: %llu of %llu bytes, no more would go in: %s\n",
					    path, (unsigned long long)(in_file + done),
					    (unsigned long long)e.size, strerror(errno));
					short_files++;
					lost_bytes += e.size - in_file - done;
					close(out_fd);
					out_fd = -1;
					break;
				}
				usleep(5000);
				retry_ms_left -= 5;
			}
			in_file += done;
			left -= want;
		}
		if (make) {
			made_bytes += in_file;
		}
	} else if (e.kind == ENT_SYMLINK || e.kind == ENT_HARDLINK) {
		static char target[MAX_PATH];

		if (e.size >= MAX_PATH) {
			ok = in_skip(e.size);
		} else if (!in(target, e.size)) {
			ok = 0;
		} else {
			target[e.size] = 0;
			if (make && e.kind == ENT_SYMLINK) {
				/* a name the skeleton made too comes out as the store has it */
				unlink(path);
				if (symlink(target, path) == 0) {
					made_links++;
				}
			} else if (make) {
				static char from[MAX_PATH];
				char keep[MAX_PATH];

				strcpy(keep, path);
				if (whole_path(target, (uint32_t)e.size)) {
					strcpy(from, path);
					strcpy(path, keep);
					unlink(path);
					if (link(from, path) == 0) {
						made_links++;
					}
				} else {
					strcpy(path, keep);
				}
			}
		}
	}
	if (out_fd >= 0) {
		close(out_fd);
	}
	if (!ok || !in_pad()) {
		return 0;
	}
	if (!make) {
		return 1;
	}
	/*
	 * Owner, then permissions, then the access control list, then the times (writing moved them), and the flags last of
	 * all: one of them may be what keeps the file from being changed again.
	 */
	switch (e.kind) {
	case ENT_DIR:
		/* its flags wait for ENT_DIRTIME: one of them may be what stops anything being made in it */
		chown(path, (int)e.uid, (int)e.gid);
		chmod(path, (int)e.mode);
		if (acl_size != 0) chmod_extended(path, KAUTH_UID_NONE, KAUTH_GID_NONE, -1, acl);
		break;
	case ENT_DIRTIME:
		set_times(&e);
		if (e.flags != 0) {
			chflags(path, (int)e.flags);
		}
		break;
	case ENT_SYMLINK:
		lchown(path, (int)e.uid, (int)e.gid);
		break;
	case ENT_HARDLINK:
		break;                  /* the same inode: it has them already */
	default:
		if (!made_file) {
			break;          /* (not made: what is there instead is not given this one's owner and mode) */
		}
		chown(path, (int)e.uid, (int)e.gid);
		chmod(path, (int)e.mode);
		if (acl_size != 0) chmod_extended(path, KAUTH_UID_NONE, KAUTH_GID_NONE, -1, acl);
		set_times(&e);
		if (e.flags != 0) {
			chflags(path, (int)e.flags);
		}
		break;
	}
	return 1;
}

static int
pass(const struct head *h, int make)
{
	stream_start((int)h->slot, h->bytes);
	not_made_dir[0] = 0;
	for (uint64_t i = 0; i < h->entries; i++) {
		if (!unpack_one(make)) {
			return 0;
		}
	}
	if (stream_left != 0) {
		printf("var_store: %llu bytes left over after %llu entries\n",
		    (unsigned long long)stream_left, (unsigned long long)h->entries);
		return 0;
	}
	if ((stream_crc ^ 0xffffffffu) != h->crc) {
		printf("var_store: slot %u: the contents are not the %08x the header says (%08x)\n", h->slot,
		    h->crc, stream_crc ^ 0xffffffffu);
		return 0;
	}
	return 1;
}

static int
load(void)
{
	struct head h;
	int best;

	memset(&h, 0, sizeof(h));
	best = newest(&h);
	if (best < 0) {
		printf("var_store: no store yet: /private/var stays as init made it\n");
		return 1;
	}
	printf("var_store: slot %d: generation %llu of %llu seconds, %llu entries, %llu bytes\n", best,
	    (unsigned long long)h.generation, (unsigned long long)h.seconds,
	    (unsigned long long)h.entries, (unsigned long long)h.bytes);
	/* read once to prove the checksum, and only then make anything */
	if (!pass(&h, 0)) {
		struct head other;
		int left = -1;

		/* the newest is torn: the one before it, which a save never touches, may still be whole */
		for (int slot = 0; slot < SLOTS; slot++) {
			if (slot != best && read_head(slot, &other) && pass(&other, 0)) {
				left = slot;
				break;
			}
		}
		if (left < 0) {
			printf("var_store: nothing good in the store: /private/var stays as init made it\n");
			return 1;
		}
		h = other;
		best = left;
		printf("var_store: slot %d is good after all: generation %llu\n", left,
		    (unsigned long long)h.generation);
	}
	{
		char saved[ID_MAX], now[ID_MAX];

		ls_keep = 0;
		if (!ls_on()) {
			printf("var_store: LaunchServices store dropped (var_store_ls=0)\n");
		} else if (!slot_identity(best, saved)) {
			printf("var_store: LaunchServices store dropped (the archive knows no image)\n");
		} else if (!image_identity(now)) {
			printf("var_store: LaunchServices store dropped (this image has no identity)\n");
		} else if (strcmp(saved, now) != 0) {
			printf("var_store: LaunchServices store dropped (image changed: %s -> %s)\n", saved, now);
		} else {
			ls_keep = 1;
			printf("var_store: LaunchServices store kept (image unchanged: %s)\n", now);
		}
	}
	if (!pass(&h, 1)) {
		printf("var_store: the archive read differently the second time: what was made stays\n");
		return 0;
	}
	/*
	 * When the archive was saved, as the time of /private/var itself (whose own time the archive does not hold): init
	 * raises a clock that is behind it to it (init/init.c, clock_floor), and the clock of a boot without one goes on from
	 * where the last boot left it.
	 */
	if (h.seconds != 0) {
		struct timeval saved[2] = { { (long)h.seconds, 0 }, { (long)h.seconds, 0 } };

		utimes(ROOT, saved);
	}
	printf("var_store: %llu directories, %llu files (%llu bytes), %llu links back in /private/var\n",
	    (unsigned long long)made_dirs, (unsigned long long)made_files, (unsigned long long)made_bytes,
	    (unsigned long long)made_links);
	if (skipped_load != 0) {
		printf("var_store: %u entries left out (the skipped directories, var_store_skip)\n", skipped_load);
	}
	if (short_files != 0) {
		printf("var_store: %llu files did not fit: %llu bytes of them are not there\n",
		    (unsigned long long)short_files, (unsigned long long)lost_bytes);
	}
	return 1;
}

static void
show(void)
{
	for (int slot = 0; slot < SLOTS; slot++) {
		static uint8_t block[BLOCK];
		struct head *h = (struct head *)block;

		if (!device_read(slot_at(slot), block, BLOCK)) {
			continue;
		}
		if (memcmp(h->magic, HEAD_MAGIC, sizeof(h->magic)) != 0) {
			printf("var_store: slot %d: empty\n", slot);
			continue;
		}
		printf("var_store: slot %d: version %u, generation %llu, %llu entries, %llu bytes, crc %08x, "
		    "written at %llu, header %s\n", slot, h->version, (unsigned long long)h->generation,
		    (unsigned long long)h->entries, (unsigned long long)h->bytes, h->crc,
		    (unsigned long long)h->seconds, h->head_crc == head_checksum(h) ? "right" : "WRONG");
	}
}

/*
 * Both slots forgotten, and what they held with them: a header zeroed alone would leave the archive behind it
 * readable to anyone with the flash in hand (the keychain too, if it was kept). So the archive of a slot whose header
 * checks out is zeroed first, as far as the header says it goes (whole blocks, never past the slot), and the header
 * last -- an erase cut short leaves a slot whose contents no longer check out, which `load` refuses, not one that
 * loads half. (What a save cut short left behind a zeroed header is not found this way and stays.)
 */
static int
erase(void)
{
	static uint8_t block[BLOCK];
	uint64_t zeroed = 0;

	memset(block, 0, sizeof(block));
	memset(buffer, 0, BUFFER);
	for (int slot = 0; slot < SLOTS; slot++) {
		struct head h;

		if (read_head(slot, &h)) {
			uint64_t bytes = (h.bytes + BLOCK - 1) / BLOCK * BLOCK;

			if (bytes > (uint64_t)SLOT_BYTES - BLOCK) {
				bytes = (uint64_t)SLOT_BYTES - BLOCK;
			}
			for (uint64_t done = 0; done < bytes;) {
				uint64_t n = bytes - done > BUFFER ? BUFFER : bytes - done;

				if (!device_write(slot_at(slot) + BLOCK + (off_t)done, buffer, n)) {
					return 0;
				}
				done += n;
			}
			zeroed += bytes;
		}
		if (!device_write(slot_at(slot), block, BLOCK)) {
			return 0;
		}
	}
	printf("var_store: both slots forgotten (%llu bytes of archive zeroed)\n", (unsigned long long)zeroed);
	return 1;
}

/*
 * Stay and save by itself. The kernel is told there is a saver, and then this waits: for the
 * kernel to ask (the run is ending, and it waits for the answer before it goes), and, if seconds
 * is given, for that long to pass between saves of its own accord. A save that fails is said so
 * and the waiting goes on -- the one at the end of the run still gets its chance.
 */
static int
save_once(void)
{
	int ok;

	/*
	 * The device is opened for the save and closed again, not held: while a write is in flight
	 * this kernel's own transfer list is out of the controller, and it goes back in when the
	 * device is closed (bsd/dev/board/ufs.c, ufs_cclose). A watcher that kept the device open
	 * would leave Darwin reading the flash through the loader for the rest of the run.
	 */
	fd = open(DEVICE, O_RDWR, 0);
	if (fd < 0) {
		printf("var_store: %s: %s\n", DEVICE, strerror(errno));
		return 0;
	}
	ok = claim() && save();
	release();
	close(fd);
	fd = -1;
	return ok;
}

/* The boot state's block, as the loader keeps it (the layout: common/flash/bundlefmt.py). */
struct boot_state {
	char magic[8];
	uint64_t trying, tries_flags;    /* tries_flags: u32 tries, u32 flags */
	uint64_t good, boots, bad, sum;
};

static uint64_t
fnv64(const void *data, uint64_t n)
{
	const uint8_t *p = data;
	uint64_t h = 0xcbf29ce484222325ULL;

	for (uint64_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

/* This start came up: the generation the loader is trying is a good one. */
static int
bootok(void)
{
	static uint8_t block[BLOCK] __attribute__((aligned(BLOCK)));
	struct boot_state st;
	int ok;

	fd = open(DEVICE, O_RDWR, 0);
	if (fd < 0) {
		printf("var_store: bootok: %s: %s\n", DEVICE, strerror(errno));
		return 0;
	}
	ok = device_read(STATE_AT, block, BLOCK);
	memcpy(&st, block, sizeof(st));
	if (!ok || memcmp(st.magic, "HKSTATE1", 8) != 0 || fnv64(&st, 48) != st.sum) {
		printf("var_store: bootok: no boot state (not started from darwin): nothing to say\n");
		close(fd);
		fd = -1;
		return 1;
	}
	if (st.good == st.trying && (uint32_t)st.tries_flags == 0) {
		printf("var_store: bootok: generation %llu is already known good\n", (unsigned long long)st.good);
		close(fd);
		fd = -1;
		return 1;
	}
	st.good = st.trying;
	st.tries_flags &= ~0xffffffffULL;        /* tries = 0, the flags kept */
	if (st.bad == st.trying) {
		st.bad = 0;
	}
	st.sum = fnv64(&st, 48);
	memcpy(block, &st, sizeof(st));
	ok = claim() && device_write(STATE_AT, block, BLOCK);
	release();
	close(fd);
	fd = -1;
	if (ok) {
		printf("var_store: bootok: generation %llu came up (start %llu)\n", (unsigned long long)st.good,
		    (unsigned long long)st.boots);
	}
	return ok;
}

static int
watch(int seconds)
{
	uint64_t last = 0;
	int asked = 0;

	fd = open(DEVICE, O_RDWR, 0);
	if (fd < 0) {
		printf("var_store: %s cannot be written (%s): nothing would be saved\n", DEVICE,
		    strerror(errno));
		return 0;
	}
	{
		/* (the newest archive read through now, at the start, and not within the 20 s the end of the run gives a save) */
		struct head h;
		int best;

		memset(&h, 0, sizeof(h));
		best = newest(&h);
		if (best >= 0 && !archive_whole(best, &h)) {
			printf("var_store: slot %d (generation %llu) does not read back whole: a save writes it, not the other\n", best,
			    (unsigned long long)h.generation);
		}
	}
	close(fd);
	fd = -1;
	if (sysctl_set_int("kern.hakios.var_save", VAR_SAVE_IDLE) != 0) {
		printf("var_store: this kernel does not keep the data volume (%s)\n", strerror(errno));
		return 0;
	}
	printf("var_store: watching: the volume is saved when the run ends%s\n",
	    seconds > 0 ? ", and in between" : "");
	{
		struct timeval tv = { 0, 0 };

		gettimeofday(&tv, NULL);
		last = (uint64_t)tv.tv_sec;
	}
	for (uint64_t started = last, said_ok = 0;;) {
		struct timeval tv = { 0, 0 };
		int state = VAR_SAVE_NONE;

		gettimeofday(&tv, NULL);
		if (!said_ok && (uint64_t)tv.tv_sec - started >= BOOTOK_AFTER) {
			said_ok = 1;
			(void)bootok();
		}

		if (sysctl_int("kern.hakios.var_save", &state) == 0 && state == VAR_SAVE_WANTED) {
			asked = 1;
		}
		gettimeofday(&tv, NULL);
		if (asked || (seconds > 0 && (uint64_t)tv.tv_sec - last >= (uint64_t)seconds)) {
			int ok = save_once();

			if (!ok) {
				printf("var_store: this save came to nothing; the store keeps what it had\n");
			}
			last = (uint64_t)tv.tv_sec;
			if (asked) {
				/* answered either way: the kernel is waiting on this to go back to the loader */
				(void)sysctl_set_int("kern.hakios.var_save", VAR_SAVE_DONE);
				return ok;
			}
		}
		usleep(100000);
	}
}

/* A line saying when, for a run to leave behind and the next one to find. */
static int
stamp(const char *where)
{
	struct timeval tv = { 0, 0 };
	char line[256];
	int out_fd = open(where, O_WRONLY | O_CREAT | O_APPEND, 0644);
	int n;

	if (out_fd < 0) {
		printf("var_store: %s: %s\n", where, strerror(errno));
		return 0;
	}
	gettimeofday(&tv, NULL);
	n = 0;
	{
		static const char head[] = "written by run of ";
		unsigned long long seconds = (unsigned long long)tv.tv_sec;
		char digits[24];
		int k = 0;

		memcpy(line, head, sizeof(head) - 1);
		n = (int)sizeof(head) - 1;
		do {
			digits[k++] = (char)('0' + seconds % 10);
			seconds /= 10;
		} while (seconds > 0);
		while (k-- > 0) {
			line[n++] = digits[k];
		}
		line[n++] = '\n';
	}
	if (write(out_fd, line, (size_t)n) != n) {
		printf("var_store: %s: %s\n", where, strerror(errno));
		close(out_fd);
		return 0;
	}
	close(out_fd);
	printf("var_store: %s: %d bytes added\n", where, n);
	return 1;
}

int
main(int argc, char **argv, char **envp)
{
	const char *what = argc > 1 ? argv[1] : "show";
	int ok;

	(void)envp;
	crc_init();
	if (strcmp(what, "bootok") == 0) {
		return bootok() ? 0 : 1;
	}
	if (strcmp(what, "stamp") == 0) {
		return stamp(argc > 2 ? argv[2] : "/private/var/hakios-stamp") ? 0 : 1;
	}
	/* ls DIR...: the names in each (there is no ls here) */
	if (strcmp(what, "ls") == 0) {
		static char buf[8192];

		for (int i = 2; i < argc; i++) {
			off_t position = 0;
			ssize_t got;
			int dir = open(argv[i], O_RDONLY | O_DIRECTORY, 0);

			if (dir < 0) {
				printf("var_store: ls: %s: %s\n", argv[i], strerror(errno));
				continue;
			}
			while ((got = getdirentries(dir, buf, sizeof(buf), &position)) > 0) {
				for (ssize_t at = 0; at < got; at += ((struct dirent *)(buf + at))->d_reclen) {
					const char *name = ((struct dirent *)(buf + at))->d_name;

					if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) printf("var_store: ls: %s/%s\n", argv[i], name);
				}
			}
			close(dir);
		}
		return 0;
	}
	/* stat PATH...: each directory on the way to PATH and PATH itself -- mode, owner, flags and the names of its extended
	 * attributes -- for telling what a restored volume differs in from one a boot made (there is no ls here) */
	if (strcmp(what, "stat") == 0) {
		for (int i = 2; i < argc; i++) {
			char path[1024];
			size_t n = strlen(argv[i]);

			if (n >= sizeof(path)) continue;
			memcpy(path, argv[i], n + 1);
			for (size_t at = 1; at <= n; at++) {
				struct stat st;
				char names[512];
				ssize_t got;

				if (at < n && path[at] != '/') continue;
				path[at] = 0;
				if (lstat(path, &st) != 0) {
					printf("var_store: stat: %s: %s\n", path, strerror(errno));
				} else {
					got = listxattr(path, names, sizeof(names) - 1, 1);
					for (ssize_t k = 0; got > 0 && k < got - 1; k++) {
						if (names[k] == 0) names[k] = ',';
					}
					if (got > 0) names[got - 1] = 0;
					printf("var_store: stat: mode %x %u:%u flags %x xattrs %s %s\n", st.st_mode, st.st_uid, st.st_gid, st.st_flags,
					    got > 0 ? names : "-", path);
				}
				if (at < n) path[at] = '/';
			}
		}
		return 0;
	}
	buffer = mmap(NULL, BUFFER, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	links = mmap(NULL, LINKS * sizeof(*links), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (buffer == MAP_FAILED || links == MAP_FAILED) {
		printf("var_store: no memory for the buffers\n");
		return 1;
	}
	/* for comparing with what the slot holds before writing over it; doing without is only slower */
	already = mmap(NULL, BUFFER, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (already == MAP_FAILED) {
		already = NULL;
	}
	new_hash = mmap(NULL, TABLE_MAX * 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	old_hash = mmap(NULL, TABLE_MAX * 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (new_hash == MAP_FAILED || old_hash == MAP_FAILED) {
		new_hash = old_hash = NULL;     /* (no table: the slot is read back, as it was) */
	}
	if (strcmp(what, "watch") == 0) {
		/* (var_store=new as well: a data volume begun anew this boot, kept from now on -- init did not unpack the store) */
		if (!boot_arg_is("var_store", "1") && !boot_arg_is("var_store", "new")) {
			printf("var_store: not asked to keep the data volume (var_store=1): nothing to watch\n");
			return 0;
		}
		/* (a boot that left parts of the store out is a look at it, not one to keep: the store stays as it was) */
		if (load_skipped("") || load_skipped_any()) {
			printf("var_store: var_store_skip: the store is only looked at this boot, not saved\n");
			return 0;
		}
		/*
		 * var_store_every=MINUTES: a save in between as well, so a crash costs that much and not the whole run. Off by
		 * default; at least 2 minutes. A save into the slot that does not hold the newest archive writes the blocks that
		 * changed since the one before the newest (see the table of hashes), so it is as cheap as what the volume changed.
		 */
		long every = argc > 2 ? parse_number(argv[2]) : boot_arg_number("var_store_every") * 60;

		if (argc <= 2 && every > 0 && every < 120) {
			every = 120;
		}
		return watch((int)every) ? 0 : 1;
	}
	fd = open(DEVICE, strcmp(what, "load") == 0 || strcmp(what, "show") == 0 ? O_RDONLY : O_RDWR, 0);
	if (fd < 0) {
		printf("var_store: %s: %s (the kernel takes writes with ufs_write=1)\n", DEVICE,
		    strerror(errno));
		return 1;
	}
	if (strcmp(what, "save") == 0) {
		ok = claim() && save();
		release();
	} else if (strcmp(what, "load") == 0) {
		ok = load();
	} else if (strcmp(what, "erase") == 0) {
		ok = claim() && erase();        /* a write like any other: the claim first */
		release();
	} else {
		show();
		ok = 1;
	}
	close(fd);
	return ok ? 0 : 1;
}
