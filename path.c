/*
 * I'm tired of doing "vsnprintf()" etc just to open a
 * file, so here's a "return static buffer with printf"
 * interface for paths.
 *
 * It's obviously not thread-safe. Sue me. But it's quite
 * useful for doing things like
 *
 *   f = open(mkpath("%s/%s.git", base, name), O_RDONLY);
 *
 * which is what it's designed for.
 */
#include "cache.h"

static char bad_path[] = "/bad-path/";

static char *get_pathname(void)
{
	static char pathname_array[4][PATH_MAX];
	static int index;
	return pathname_array[3 & ++index];
}

static char *cleanup_path(char *path)
{
	/* Clean it up */
	if (!memcmp(path, "./", 2)) {
		path += 2;
		while (*path == '/')
			path++;
	}
	return path;
}

char *mkpath(const char *fmt, ...)
{
	va_list args;
	unsigned len;
	char *pathname = get_pathname();

	va_start(args, fmt);
	len = vsnprintf(pathname, PATH_MAX, fmt, args);
	va_end(args);
	if (len >= PATH_MAX)
		return bad_path;
	return cleanup_path(pathname);
}

char *git_path(const char *fmt, ...)
{
	const char *git_dir = get_git_dir();
	char *pathname = get_pathname();
	va_list args;
	unsigned len;

	len = strlen(git_dir);
	if (len > PATH_MAX-100)
		return bad_path;
	memcpy(pathname, git_dir, len);
	if (len && git_dir[len-1] != '/')
		pathname[len++] = '/';
	va_start(args, fmt);
	len += vsnprintf(pathname + len, PATH_MAX - len, fmt, args);
	va_end(args);
	if (len >= PATH_MAX)
		return bad_path;
	return cleanup_path(pathname);
}


/* git_mkstemp() - create tmp file honoring TMPDIR variable */
int git_mkstemp(char *path, size_t len, const char *template)
{
	const char *tmp;
	size_t n;

	tmp = getenv("TMPDIR");
	if (!tmp)
		tmp = "/tmp";
	n = snprintf(path, len, "%s/%s", tmp, template);
	if (len <= n) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return mkstemp(path);
}


int validate_headref(const char *path)
{
	struct stat st;
	char *buf, buffer[256];
	unsigned char sha1[20];
	int fd;
	ssize_t len;

	if (lstat(path, &st) < 0)
		return -1;

	/* Make sure it is a "refs/.." symlink */
	if (S_ISLNK(st.st_mode)) {
		len = readlink(path, buffer, sizeof(buffer)-1);
		if (len >= 5 && !memcmp("refs/", buffer, 5))
			return 0;
		return -1;
	}

	/*
	 * Anything else, just open it and try to see if it is a symbolic ref.
	 */
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	len = read_in_full(fd, buffer, sizeof(buffer)-1);
	close(fd);

	/*
	 * Is it a symbolic ref?
	 */
	if (len < 4)
		return -1;
	if (!memcmp("ref:", buffer, 4)) {
		buf = buffer + 4;
		len -= 4;
		while (len && isspace(*buf))
			buf++, len--;
		if (len >= 5 && !memcmp("refs/", buf, 5))
			return 0;
	}

	/*
	 * Is this a detached HEAD?
	 */
	if (!get_sha1_hex(buffer, sha1))
		return 0;

	return -1;
}

static char *user_path(char *buf, char *path, int sz)
{
	struct passwd *pw;
	char *slash;
	int len, baselen;

	if (!path || path[0] != '~')
		return NULL;
	path++;
	slash = strchr(path, '/');
	if (path[0] == '/' || !path[0]) {
		pw = getpwuid(getuid());
	}
	else {
		if (slash) {
			*slash = 0;
			pw = getpwnam(path);
			*slash = '/';
		}
		else
			pw = getpwnam(path);
	}
	if (!pw || !pw->pw_dir || sz <= strlen(pw->pw_dir))
		return NULL;
	baselen = strlen(pw->pw_dir);
	memcpy(buf, pw->pw_dir, baselen);
	while ((1 < baselen) && (buf[baselen-1] == '/')) {
		buf[baselen-1] = 0;
		baselen--;
	}
	if (slash && slash[1]) {
		len = strlen(slash);
		if (sz <= baselen + len)
			return NULL;
		memcpy(buf + baselen, slash, len + 1);
	}
	return buf;
}

/*
 * First, one directory to try is determined by the following algorithm.
 *
 * (0) If "strict" is given, the path is used as given and no DWIM is
 *     done. Otherwise:
 * (1) "~/path" to mean path under the running user's home directory;
 * (2) "~user/path" to mean path under named user's home directory;
 * (3) "relative/path" to mean cwd relative directory; or
 * (4) "/absolute/path" to mean absolute directory.
 *
 * Unless "strict" is given, we try access() for existence of "%s.git/.git",
 * "%s/.git", "%s.git", "%s" in this order.  The first one that exists is
 * what we try.
 *
 * Second, we try chdir() to that.  Upon failure, we return NULL.
 *
 * Then, we try if the current directory is a valid git repository.
 * Upon failure, we return NULL.
 *
 * If all goes well, we return the directory we used to chdir() (but
 * before ~user is expanded), avoiding getcwd() resolving symbolic
 * links.  User relative paths are also returned as they are given,
 * except DWIM suffixing.
 */
char *enter_repo(char *path, int strict)
{
	static char used_path[PATH_MAX];
	static char validated_path[PATH_MAX];

	if (!path)
		return NULL;

	if (!strict) {
		static const char *suffix[] = {
			".git/.git", "/.git", ".git", "", NULL,
		};
		int len = strlen(path);
		int i;
		while ((1 < len) && (path[len-1] == '/')) {
			path[len-1] = 0;
			len--;
		}
		if (PATH_MAX <= len)
			return NULL;
		if (path[0] == '~') {
			if (!user_path(used_path, path, PATH_MAX))
				return NULL;
			strcpy(validated_path, path);
			path = used_path;
		}
		else if (PATH_MAX - 10 < len)
			return NULL;
		else {
			path = strcpy(used_path, path);
			strcpy(validated_path, path);
		}
		len = strlen(path);
		for (i = 0; suffix[i]; i++) {
			strcpy(path + len, suffix[i]);
			if (!access(path, F_OK)) {
				strcat(validated_path, suffix[i]);
				break;
			}
		}
		if (!suffix[i] || chdir(path))
			return NULL;
		path = validated_path;
	}
	else if (chdir(path))
		return NULL;

	if (access("objects", X_OK) == 0 && access("refs", X_OK) == 0 &&
	    validate_headref("HEAD") == 0) {
		setenv(GIT_DIR_ENVIRONMENT, ".", 1);
		check_repository_format();
		return path;
	}

	return NULL;
}

int adjust_shared_perm(const char *path)
{
	struct stat st;
	int mode;

	if (!shared_repository)
		return 0;
	if (lstat(path, &st) < 0)
		return -1;
	mode = st.st_mode;

	if (shared_repository) {
		int tweak = shared_repository;
		if (!(mode & S_IWUSR))
			tweak &= ~0222;
		mode = (mode & ~0777) | tweak;
	} else {
		/* Preserve old PERM_UMASK behaviour */
		if (mode & S_IWUSR)
			mode |= S_IWGRP;
	}

	if (S_ISDIR(mode)) {
		mode |= FORCE_DIR_SET_GID;

		/* Copy read bits to execute bits */
		mode |= (shared_repository & 0444) >> 2;
	}

	if ((mode & st.st_mode) != mode && chmod(path, mode) < 0)
		return -2;
	return 0;
}

/* We allow "recursive" symbolic links. Only within reason, though. */
#define MAXDEPTH 5

const char *make_absolute_path(const char *path)
{
	static char bufs[2][PATH_MAX + 1], *buf = bufs[0], *next_buf = bufs[1];
	char cwd[1024] = "";
	int buf_index = 1, len;

	int depth = MAXDEPTH;
	char *last_elem = NULL;
	struct stat st;

	if (strlcpy(buf, path, PATH_MAX) >= PATH_MAX)
		die ("Too long path: %.*s", 60, path);

	while (depth--) {
		if (stat(buf, &st) || !S_ISDIR(st.st_mode)) {
			char *last_slash = strrchr(buf, '/');
			if (last_slash) {
				*last_slash = '\0';
				last_elem = xstrdup(last_slash + 1);
			} else {
				last_elem = xstrdup(buf);
				*buf = '\0';
			}
		}

		if (*buf) {
			if (!*cwd && !getcwd(cwd, sizeof(cwd)))
				die ("Could not get current working directory");

			if (chdir(buf))
				die ("Could not switch to '%s'", buf);
		}
		if (!getcwd(buf, PATH_MAX))
			die ("Could not get current working directory");

		if (last_elem) {
			int len = strlen(buf);
			if (len + strlen(last_elem) + 2 > PATH_MAX)
				die ("Too long path name: '%s/%s'",
						buf, last_elem);
			buf[len] = '/';
			strcpy(buf + len + 1, last_elem);
			free(last_elem);
			last_elem = NULL;
		}

		if (!lstat(buf, &st) && S_ISLNK(st.st_mode)) {
			len = readlink(buf, next_buf, PATH_MAX);
			if (len < 0)
				die ("Invalid symlink: %s", buf);
			next_buf[len] = '\0';
			buf = next_buf;
			buf_index = 1 - buf_index;
			next_buf = bufs[buf_index];
		} else
			break;
	}

	if (*cwd && chdir(cwd))
		die ("Could not change back to '%s'", cwd);

	return buf;
}
