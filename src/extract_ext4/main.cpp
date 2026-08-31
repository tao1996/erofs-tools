// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 skkk
 *
 * extract.ext4 - extract files/dirs/symlinks/hardlinks/special nodes from an
 * ext2/ext3/ext4 image using the ext2fs library (e2fsprogs).
 *
 * The command line interface and overall workflow are modeled after the
 * project's extract.erofs tool.
 */

#include <getopt.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <utime.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <ext2fs/ext2fs.h>
#include <ext2fs/ext2_io.h>
#include <et/com_err.h>

#include <endian.h>
#include <fstream>
#include <linux/capability.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* result codes, mirroring extract.erofs's ExtractResult */
enum {
	RET_EXTRACT_DONE = 0,
	RET_EXTRACT_CONFIG_DONE = 1,
	RET_EXTRACT_CONFIG_FAIL,
	RET_EXTRACT_INIT_FAIL,
	RET_EXTRACT_INIT_NODE_FAIL,
	RET_EXTRACT_OUTDIR_ROOT,
	RET_EXTRACT_OPEN_FILE,
	RET_EXTRACT_CREATE_DIR_FAIL,
	RET_EXTRACT_CREATE_FILE_FAIL,
	RET_EXTRACT_THREAD_NUM_ERROR,
	RET_EXTRACT_FAIL_SKIP,
	RET_EXTRACT_FAIL_EXIT
};

namespace {

struct Config {
	std::string imagePath;
	std::string imageBaseName;
	uint64_t offset = 0;
	std::string outDir;
	std::vector<std::string> targets;
	std::string targetPath;

	bool isPrintAll = false;
	bool isPrintTarget = false;
	bool isExtractAll = false;
	bool isExtractTarget = false;
	bool targetRecursive = false;
	bool overwrite = false;
	bool isSilent = false;
	uint32_t threadNum = 0;

	mode_t umaskValue;
	bool superuser;
	bool preserve_owner;
	bool preserve_perms;

	Config()
		: umaskValue([] {
			  mode_t m = ::umask(0);
			  ::umask(m);
			  return m;
		  }()),
		  superuser(geteuid() == 0),
		  preserve_owner(superuser),
		  preserve_perms(superuser) {}
};

struct Node {
	ext2_ino_t ino = 0;
	std::string path; /* absolute path inside the image, e.g. "/system/bin/sh" */
	struct ext2_inode_large inode{};
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static void split(const std::string &s, char delim, std::vector<std::string> &out) {
	out.clear();
	size_t start = 0;
	for (;;) {
		size_t pos = s.find(delim, start);
		std::string part = s.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
		out.push_back(part);
		if (pos == std::string::npos)
			break;
		start = pos + 1;
	}
}

static std::string trim(const std::string &s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return "";
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

static int write_all(int fd, const char *buf, size_t n) {
	while (n > 0) {
		ssize_t w = write(fd, buf, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		buf += w;
		n -= w;
	}
	return 0;
}

static int mkdirs_p(const std::string &path, mode_t mode) {
	if (path.empty())
		return 0;
	struct stat st{};
	if (stat(path.c_str(), &st) == 0)
		return S_ISDIR(st.st_mode) ? 0 : -1;
	size_t pos = path.rfind('/');
	if (pos != std::string::npos) {
		if (pos != 0) {
			if (mkdirs_p(path.substr(0, pos), mode) != 0)
				return -1;
		}
	}
	if (mkdir(path.c_str(), mode) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

static size_t path_depth(const std::string &p) {
	size_t d = 0;
	for (char c : p)
		if (c == '/')
			++d;
	return d;
}

static const char *type_str(const Node &n) {
	if (S_ISDIR(n.inode.i_mode)) return "dir";
	if (S_ISREG(n.inode.i_mode)) return "reg";
	if (S_ISLNK(n.inode.i_mode)) return "symlink";
	if (S_ISCHR(n.inode.i_mode)) return "char";
	if (S_ISBLK(n.inode.i_mode)) return "block";
	if (S_ISFIFO(n.inode.i_mode)) return "fifo";
	if (S_ISSOCK(n.inode.i_mode)) return "sock";
	return "unknown";
}

static dev_t rdev_from_inode(const struct ext2_inode_large &inode) {
	/* Both the old and new Linux encodings are supported; the old one
	 * stores the packed device number in i_block[0], the new one in
	 * i_block[1]. */
	__u32 raw = inode.i_block[0] ? inode.i_block[0] : inode.i_block[1];
	unsigned int major = ((raw >> 8) & 0xfff) | ((inode.osd2.linux2.l_i_file_acl_high & 0xfff) << 12);
	unsigned int minor = (raw & 0xff) | ((raw >> 12) & 0xfff00);
	return makedev(major, minor);
}

static std::string make_dest(const Config &cfg, const std::string &nodepath) {
	std::string rel = nodepath;
	if (!rel.empty() && rel.front() == '/')
		rel.erase(0, 1);
	if (rel.empty())
		return cfg.outDir;
	return cfg.outDir + "/" + rel;
}

static std::string dirname_of(const std::string &p) {
	size_t pos = p.rfind('/');
	if (pos == std::string::npos)
		return ".";
	if (pos == 0)
		return "/";
	return p.substr(0, pos);
}

/* make sure the parent directory of dest exists (needed when extracting a
 * single file/symlink/special node whose parents were not part of the walk) */
static int ensure_parent(const std::string &dest) {
	std::string parent = dirname_of(dest);
	if (parent == "." || parent == "/")
		return 0;
	if (mkdirs_p(parent, 0700) != 0)
		return -errno;
	return 0;
}

/* ------------------------------------------------------------------ */
/* directory traversal                                                 */
/* ------------------------------------------------------------------ */

struct Walker {
	ext2_filsys fs = nullptr;
	std::vector<Node> nodes;
	errcode_t err = 0;
};

struct DirIterCtx {
	ext2_filsys fs;
	const std::string *path;
	Walker *w;
};

static void walk_dir(ext2_filsys fs, ext2_ino_t ino, const std::string &path, Walker &w);

static int dir_iter_cb(struct ext2_dir_entry *dirent, int offset, int blocksize, char *buf, void *priv) {
	(void)offset;
	(void)blocksize;
	(void)buf;
	auto *ctx = static_cast<DirIterCtx *>(priv);
	int namelen = ext2fs_dirent_name_len(dirent);
	std::string name(dirent->name, namelen);
	if (name == "." || name == "..")
		return 0;
	if (name == "lost+found" && *ctx->path == "/")
		return 0;

	struct ext2_inode_large inode{};
	errcode_t ret = ext2fs_read_inode_full(ctx->fs, dirent->inode, (struct ext2_inode *)&inode, sizeof(inode));
	if (ret) {
		if (!ctx->w->err)
			ctx->w->err = ret;
		return 0;
	}

	std::string child = (*ctx->path == "/" ? "" : *ctx->path) + "/" + name;
	ctx->w->nodes.push_back(Node{dirent->inode, child, inode});
	if (S_ISDIR(inode.i_mode))
		walk_dir(ctx->fs, dirent->inode, child, *ctx->w);
	return 0;
}

static void walk_dir(ext2_filsys fs, ext2_ino_t ino, const std::string &path, Walker &w) {
	DirIterCtx ctx{fs, &path, &w};
	errcode_t ret = ext2fs_dir_iterate(fs, ino, 0, nullptr, dir_iter_cb, &ctx);
	if (ret && !w.err)
		w.err = ret;
}

static errcode_t collect_tree(ext2_filsys fs, Walker &w) {
	w.fs = fs;
	struct ext2_inode_large root{};
	errcode_t ret = ext2fs_read_inode_full(fs, EXT2_ROOT_INO, (struct ext2_inode *)&root, sizeof(root));
	if (ret)
		return ret;
	w.nodes.push_back(Node{EXT2_ROOT_INO, "/", root});
	walk_dir(fs, EXT2_ROOT_INO, "/", w);
	return w.err;
}

/* ------------------------------------------------------------------ */
/* target filtering                                                    */
/* ------------------------------------------------------------------ */

static std::string normalize_target(const std::string &t) {
	std::string s = trim(t);
	if (!s.empty() && s.front() != '/')
		s = "/" + s;
	if (s.size() > 1 && s.back() == '/')
		s.pop_back();
	return s;
}

static bool path_matches(const std::string &nodepath, const std::string &target, bool recursive) {
	std::string t = normalize_target(target);
	if (nodepath == t)
		return true;
	if (recursive) {
		std::string prefix = (t == "/" ? "/" : t + "/");
		return nodepath.rfind(prefix, 0) == 0;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* metadata                                                            */
/* ------------------------------------------------------------------ */

static void set_attributes(const Config &cfg, const Node &node, const std::string &dest) {
	timespec times[2] = {
		{static_cast<time_t>(node.inode.i_atime), 0},
		{static_cast<time_t>(node.inode.i_mtime), 0},
	};
	if (utimensat(AT_FDCWD, dest.c_str(), times, AT_SYMLINK_NOFOLLOW) < 0)
		fprintf(stderr, "warning: failed to set times: %s\n", dest.c_str());

	if (cfg.preserve_owner) {
		if (lchown(dest.c_str(), inode_uid(node.inode), inode_gid(node.inode)) < 0)
			fprintf(stderr, "warning: failed to change ownership: %s\n", dest.c_str());
	}

	if (!S_ISLNK(node.inode.i_mode)) {
		mode_t m = cfg.preserve_perms ? node.inode.i_mode : (node.inode.i_mode & ~cfg.umaskValue);
		if (chmod(dest.c_str(), m) < 0)
			fprintf(stderr, "warning: failed to set permissions: %s\n", dest.c_str());
	}
}

/* ------------------------------------------------------------------ */
/* extraction primitives                                               */
/* ------------------------------------------------------------------ */

static int create_hardlink(const Config &cfg, const std::string &src, const std::string &dest) {
	int rc = ensure_parent(dest);
	if (rc)
		return rc;
	if (link(src.c_str(), dest.c_str()) < 0) {
		if (errno == EEXIST && cfg.overwrite) {
			if (unlink(dest.c_str()) < 0)
				return -errno;
			if (link(src.c_str(), dest.c_str()) == 0)
				return 0;
		}
		if (errno == EEXIST && !cfg.overwrite)
			return RET_EXTRACT_FAIL_SKIP;
		return -errno;
	}
	return 0;
}

static int extract_regular(ext2_filsys fs, const Config &cfg, const Node &node, const std::string &dest) {
	bool tryagain = true;
	int rc = ensure_parent(dest);
	if (rc)
		return rc;
again:
	int fd = open(dest.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | (cfg.overwrite ? O_TRUNC : O_EXCL), 0600);
	if (fd < 0) {
		if (cfg.overwrite && tryagain) {
			if (errno == EISDIR) {
				if (rmdir(dest.c_str()) < 0)
					return -errno;
			} else if (errno == EACCES && chmod(dest.c_str(), 0700) < 0) {
				return -errno;
			}
			tryagain = false;
			goto again;
		}
		if (errno == EEXIST && !cfg.overwrite)
			return RET_EXTRACT_FAIL_SKIP;
		return -errno;
	}

	ext2_file_t file = nullptr;
	errcode_t ret = ext2fs_file_open2(fs, node.ino, (struct ext2_inode *)&node.inode, 0, &file);
	if (!ret) {
		char buf[131072];
		unsigned int got = 0;
		while ((ret = ext2fs_file_read(file, buf, sizeof(buf), &got)) == 0 && got > 0) {
			if (write_all(fd, buf, got) != 0) {
				ret = errno ? errno : 1;
				break;
			}
		}
		ext2fs_file_close(file);
	}
	close(fd);
	return ret == 0 ? 0 : -static_cast<int>(ret);
}

static int extract_symlink(ext2_filsys fs, const Config &cfg, const Node &node, const std::string &dest) {
	int rc = ensure_parent(dest);
	if (rc)
		return rc;
	__u64 size = EXT2_I_SIZE(&node.inode);
	std::string target;

	if (ext2fs_is_fast_symlink((struct ext2_inode *)&node.inode)) {
		target.assign(reinterpret_cast<const char *>(node.inode.i_block), size);
	} else {
		target.resize(size);
		ext2_file_t file = nullptr;
		errcode_t ret = ext2fs_file_open2(fs, node.ino, (struct ext2_inode *)&node.inode, 0, &file);
		if (ret)
			return -static_cast<int>(ret);
		size_t off = 0;
		while (off < size) {
			unsigned int got = 0;
			unsigned int want = static_cast<unsigned int>(size - off);
			ret = ext2fs_file_read(file, target.data() + off, want, &got);
			if (ret || got == 0)
				break;
			off += got;
		}
		ext2fs_file_close(file);
		if (ret)
			return -static_cast<int>(ret);
		target.resize(off);
	}

	if (symlink(target.c_str(), dest.c_str()) < 0) {
		if (errno == EEXIST && cfg.overwrite) {
			if (unlink(dest.c_str()) < 0)
				return -errno;
			if (symlink(target.c_str(), dest.c_str()) == 0)
				return 0;
		}
		if (errno == EEXIST && !cfg.overwrite)
			return RET_EXTRACT_FAIL_SKIP;
		return -errno;
	}
	return 0;
}

static int extract_special(const Config &cfg, const Node &node, const std::string &dest) {
	int rc = ensure_parent(dest);
	if (rc)
		return rc;
	dev_t rdev = rdev_from_inode(node.inode);
	if (mknod(dest.c_str(), node.inode.i_mode, rdev) < 0) {
		if (errno == EEXIST && cfg.overwrite) {
			if (unlink(dest.c_str()) < 0)
				return -errno;
			if (mknod(dest.c_str(), node.inode.i_mode, rdev) == 0)
				return 0;
		}
		if (errno == EEXIST && !cfg.overwrite)
			return RET_EXTRACT_FAIL_SKIP;
		if (errno == EEXIST || cfg.superuser)
			return -errno;
		return RET_EXTRACT_FAIL_SKIP;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* node planning + orchestration                                       */
/* ------------------------------------------------------------------ */

struct Plan {
	std::vector<const Node *> dirs;
	std::vector<const Node *> primaries; /* regular files whose content is extracted */
	std::vector<const Node *> hardlinks; /* regular files linked to a primary */
	std::vector<const Node *> others;    /* symlinks + special nodes */
	std::unordered_map<ext2_ino_t, std::string> linkMap;
};

static void build_plan(const Config &cfg, const std::vector<Node> &nodes, Plan &plan) {
	std::unordered_map<ext2_ino_t, std::string> firstSeen;
	for (const auto &n : nodes) {
		if (n.path == "/")
			continue;
		if (S_ISDIR(n.inode.i_mode)) {
			plan.dirs.push_back(&n);
		} else if (S_ISREG(n.inode.i_mode)) {
			if (n.inode.i_links_count > 1) {
				auto it = firstSeen.find(n.ino);
				if (it == firstSeen.end()) {
					firstSeen[n.ino] = make_dest(cfg, n.path);
					plan.primaries.push_back(&n);
				} else {
					plan.linkMap[n.ino] = it->second;
					plan.hardlinks.push_back(&n);
				}
			} else {
				plan.primaries.push_back(&n);
			}
		} else {
			plan.others.push_back(&n);
		}
	}
}

static void extract_primaries(ext2_filsys fs, const Config &cfg, const std::vector<const Node *> &primaries,
                              uint32_t threads, int &failures) {
	std::atomic<size_t> idx{0};
	std::mutex errLock;
	uint32_t n = threads ? threads : 1;
	std::vector<std::thread> pool;
	for (uint32_t t = 0; t < n; ++t) {
		pool.emplace_back([&]() {
			for (;;) {
				size_t i = idx.fetch_add(1);
				if (i >= primaries.size())
					break;
				const Node &node = *primaries[i];
				std::string dest = make_dest(cfg, node.path);
				int rc = extract_regular(fs, cfg, node, dest);
				if (rc == 0) {
					set_attributes(cfg, node, dest);
				} else if (rc != RET_EXTRACT_FAIL_SKIP) {
					fprintf(stderr, "error: extract '%s' failed: %s\n", node.path.c_str(), strerror(-rc));
					std::lock_guard<std::mutex> g(errLock);
					++failures;
				}
			}
		});
	}
	for (auto &th : pool)
		th.join();
}

static int do_extract(ext2_filsys fs, const Config &cfg, const std::vector<Node> &nodes) {
	Plan plan;
	build_plan(cfg, nodes, plan);
	int failures = 0;

	/* 1. base + all directories (parents first) */
	if (mkdirs_p(cfg.outDir, 0755) != 0) {
		fprintf(stderr, "error: create out dir '%s' failed\n", cfg.outDir.c_str());
		return RET_EXTRACT_CREATE_DIR_FAIL;
	}
	std::vector<const Node *> dirs = plan.dirs;
	std::sort(dirs.begin(), dirs.end(), [](const Node *a, const Node *b) {
		return path_depth(a->path) < path_depth(b->path);
	});
	for (const Node *d : dirs) {
		std::string dest = make_dest(cfg, d->path);
		if (mkdirs_p(dest, 0700) != 0) {
			fprintf(stderr, "error: create dir '%s' failed\n", d->path.c_str());
			return RET_EXTRACT_CREATE_DIR_FAIL;
		}
	}

	/* 2. regular files */
	extract_primaries(fs, cfg, plan.primaries, cfg.threadNum, failures);

	/* 3. hardlinks (after their source has been written) */
	for (const Node *h : plan.hardlinks) {
		std::string dest = make_dest(cfg, h->path);
		int rc = create_hardlink(cfg, plan.linkMap[h->ino], dest);
		if (rc == 0) {
			set_attributes(cfg, *h, dest);
		} else if (rc != RET_EXTRACT_FAIL_SKIP) {
			fprintf(stderr, "error: create hardlink '%s' failed\n", h->path.c_str());
			++failures;
		}
	}

	/* 4. symlinks + special nodes */
	for (const Node *o : plan.others) {
		std::string dest = make_dest(cfg, o->path);
		int rc;
		if (S_ISLNK(o->inode.i_mode))
			rc = extract_symlink(fs, cfg, *o, dest);
		else
			rc = extract_special(cfg, *o, dest);
		if (rc == 0) {
			set_attributes(cfg, *o, dest);
		} else if (rc != RET_EXTRACT_FAIL_SKIP) {
			fprintf(stderr, "error: extract '%s' failed\n", o->path.c_str());
			++failures;
		}
	}

	/* 5. directory attributes, deepest first */
	for (auto it = dirs.rbegin(); it != dirs.rend(); ++it)
		set_attributes(cfg, **it, make_dest(cfg, (*it)->path));

	if (!cfg.isSilent) {
		fprintf(stderr, "extracted: %zu files, %zu dirs, %zu hardlinks, %zu other\n",
		        plan.primaries.size() + plan.hardlinks.size(), dirs.size(),
		        plan.hardlinks.size(), plan.others.size());
	}
	return failures ? RET_EXTRACT_FAIL_EXIT : RET_EXTRACT_DONE;
}

/* ------------------------------------------------------------------ */
/* printing                                                            */
/* ------------------------------------------------------------------ */

static void print_node(const Node &n) {
	__u64 size = EXT2_I_SIZE(&n.inode);
	printf("%-8s %04o %5u %5u %12llu  %s\n",
	       type_str(n),
	       n.inode.i_mode & 07777,
	       inode_uid(n.inode),
	       inode_gid(n.inode),
	       static_cast<unsigned long long>(size),
	       n.path.c_str());
}

/* ------------------------------------------------------------------ */
/* config + cli                                                        */
/* ------------------------------------------------------------------ */

static void compute_base_name(Config &cfg) {
	std::string b = cfg.imagePath;
	size_t ps = b.rfind('/');
	if (ps != std::string::npos)
		b = b.substr(ps + 1);
	ps = b.find('.');
	if (ps != std::string::npos)
		b.erase(ps);
	cfg.imageBaseName = b;
}

static int init_out_dir(Config &cfg) {
	if (cfg.outDir.empty()) {
		cfg.outDir = "./" + cfg.imageBaseName;
	} else {
		if (cfg.outDir.size() > 1 &&
		    (cfg.outDir.back() == '/' || cfg.outDir.back() == '\\'))
			cfg.outDir.pop_back();
		if (cfg.outDir.size() >= PATH_MAX) {
			fprintf(stderr, "error: out dir name too long\n");
			return RET_EXTRACT_OUTDIR_ROOT;
		}
		bool isRoot = !cfg.outDir.empty();
		for (char c : cfg.outDir)
			if (c != '/')
				isRoot = false;
		if (isRoot) {
			fprintf(stderr, "error: not allow extracting to root: '%s'\n", cfg.outDir.c_str());
			return RET_EXTRACT_OUTDIR_ROOT;
		}
		cfg.outDir = cfg.outDir + "/" + cfg.imageBaseName;
	}
	return RET_EXTRACT_DONE;
}

static void usage(const Config &cfg) {
	static const char *text =
		"usage: [options]\n"
		"  -h, --help             Display this help and exit\n"
		"  -i, --image=[FILE]     Image file\n"
		"  --offset=#             skip # bytes at the beginning of IMAGE\n"
		"  -p                     Print all entries\n"
		"  -P, --print=X          Print the target of path X\n"
		"  -x                     Extract all items\n"
		"  -X, --extract=X        Extract the target of path X\n"
		"  -r                     When using target, recurse directories\n"
		"  -s                     Silent mode, don't show progress\n"
		"  -f, --overwrite        overwrite files that already exist (default: skip)\n"
		"  -T#                    Use # threads (default: -T0 uses all cores)\n"
		"  -o, --outdir=X         Output dir\n"
		"  -V, --version          Print the version info\n";
	(void)cfg;
	printf("%s\n", text);
}

static void print_version() {
	printf("extract.ext4 v0.0.1 (e2fsprogs)\n");
}

static option arg_options[] = {
	{"help", no_argument, nullptr, 'h'},
	{"version", no_argument, nullptr, 'V'},
	{"image", required_argument, nullptr, 'i'},
	{"offset", required_argument, nullptr, 2},
	{"outdir", required_argument, nullptr, 'o'},
	{"print", required_argument, nullptr, 'P'},
	{"overwrite", no_argument, nullptr, 'f'},
	{"extract", required_argument, nullptr, 'X'},
	{"recursive", no_argument, nullptr, 'r'},
	{"silent", no_argument, nullptr, 's'},
	{"threads", required_argument, nullptr, 'T'},
	{nullptr, no_argument, nullptr, 0},
};

static int parse_config(int argc, char **argv, Config &cfg) {
	int opt, ret = RET_EXTRACT_CONFIG_FAIL;
	bool entered = false;
	while ((opt = getopt_long(argc, argv, "hi:psxfrc:P:T:o:X:V", arg_options, nullptr)) != -1) {
		entered = true;
		switch (opt) {
			case 'h': usage(cfg); return RET_EXTRACT_CONFIG_DONE;
			case 'V': print_version(); return RET_EXTRACT_CONFIG_DONE;
			case 'i':
				if (optarg) cfg.imagePath = optarg;
				break;
			case 'o':
				if (optarg) cfg.outDir = optarg;
				break;
			case 'p': cfg.isPrintAll = true; break;
			case 'P':
				cfg.isPrintTarget = true;
				if (optarg) { cfg.targetPath = optarg; split(trim(optarg), ',', cfg.targets); }
				break;
			case 'f': cfg.overwrite = true; break;
			case 'x': cfg.isExtractAll = true; break;
			case 'X':
				cfg.isExtractTarget = true;
				if (optarg) { cfg.targetPath = optarg; split(trim(optarg), ',', cfg.targets); }
				break;
			case 'r': cfg.targetRecursive = true; break;
			case 's': cfg.isSilent = true; break;
			case 'T':
				if (optarg) {
					char *end = nullptr;
					unsigned long n = strtoul(optarg, &end, 0);
					if (end && *end == '\0') cfg.threadNum = static_cast<uint32_t>(n);
				}
				break;
			case 2:
				if (optarg) {
					char *end = nullptr;
					unsigned long long n = strtoull(optarg, &end, 0);
					if (end && *end == '\0') cfg.offset = n;
				}
				break;
			default:
				usage(cfg);
				return RET_EXTRACT_CONFIG_FAIL;
		}
	}

	if (!entered) {
		usage(cfg);
		return RET_EXTRACT_CONFIG_FAIL;
	}

	if (cfg.imagePath.empty()) {
		fprintf(stderr, "error: image file is required\n");
		return RET_EXTRACT_OPEN_FILE;
	}

	compute_base_name(cfg);
	ret = init_out_dir(cfg);
	if (ret)
		return ret;

	if (cfg.threadNum == 0) {
		unsigned int hc = std::thread::hardware_concurrency();
		cfg.threadNum = hc ? hc : 1;
	}

	return RET_EXTRACT_CONFIG_DONE;
}

static void print_operation_time(const timeval *start, const timeval *end) {
	double sec = (end->tv_sec - start->tv_sec) + (end->tv_usec - start->tv_usec) / 1000000.0;
	printf("The operation took: %.3f second(s).\n", sec);
}

} // namespace

int main(const int argc, char *argv[]) {
	int ret = RET_EXTRACT_DONE;
	timeval start{}, end{};

	setbuf(stdout, nullptr);
	setbuf(stderr, nullptr);
	gettimeofday(&start, nullptr);

	Config cfg;
	int configRet = parse_config(argc, argv, cfg);
	if (configRet != RET_EXTRACT_CONFIG_DONE) {
		ret = RET_EXTRACT_INIT_FAIL;
		goto exit;
	}

	initialize_ext2_error_table();

	{
		ext2_filsys fs = nullptr;
		char io_options[64];
		snprintf(io_options, sizeof(io_options), "offset=%llu", static_cast<unsigned long long>(cfg.offset));

		errcode_t e = ext2fs_open2(cfg.imagePath.c_str(), io_options,
		                           EXT2_FLAG_64BITS | EXT2_FLAG_SOFTSUPP_FEATURES,
		                           0, 0, unix_io_manager, &fs);
		if (e) {
			fprintf(stderr, "error: open image failed: %s\n", error_message(e));
			ret = RET_EXTRACT_OPEN_FILE;
			goto exit;
		}

		Walker walker;
		if (collect_tree(fs, walker) != 0) {
			fprintf(stderr, "error: failed to read inode tree\n");
			ext2fs_close_free(&fs);
			ret = RET_EXTRACT_INIT_NODE_FAIL;
			goto exit;
		}

		/* select nodes */
		std::vector<Node> selected;
		if (cfg.isPrintAll || cfg.isExtractAll) {
			selected = walker.nodes;
		} else if (cfg.isPrintTarget || cfg.isExtractTarget) {
			for (const auto &n : walker.nodes) {
				for (const auto &t : cfg.targets) {
					if (path_matches(n.path, t, cfg.targetRecursive)) {
						selected.push_back(n);
						break;
					}
				}
			}
			if (selected.empty()) {
				fprintf(stderr, "warning: target path not found: '%s'\n", cfg.targetPath.c_str());
			}
		} else {
			fprintf(stderr, "error: nothing to do (-p / -P / -x / -X)\n");
			ext2fs_close_free(&fs);
			ret = RET_EXTRACT_CONFIG_FAIL;
			goto exit;
		}

		if (cfg.isPrintTarget || cfg.isPrintAll) {
			for (const auto &n : selected)
				print_node(n);
		} else if (cfg.isExtractTarget || cfg.isExtractAll) {
			if (!cfg.isSilent)
				fprintf(stderr, "Starting...\n");
			ret = do_extract(fs, cfg, selected);
		}

		ext2fs_close_free(&fs);
	}

exit:
	gettimeofday(&end, nullptr);
	print_operation_time(&start, &end);
	return ret;
}