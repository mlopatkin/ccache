/*
 * Copyright (C) Joel Rosdahl 2009
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 675 Mass
 * Ave, Cambridge, MA 02139, USA.
 */

#include <assert.h>
#include <inttypes.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ccache.h"
#include "hashtable_itr.h"
#include "hashutil.h"
#include "manifest.h"
#include "murmurhashneutral2.h"

extern char *temp_dir;

/*
 * Sketchy specification of the manifest disk format:
 *
 * <magic>         magic number                        (4 bytes)
 * <version>       version                             (2 bytes unsigned int)
 * ----------------------------------------------------------------------------
 * <n>             number of include file paths        (2 bytes unsigned int)
 * <path_0>        path to include file                (NUL-terminated string,
 * ...                                                  at most 1024 bytes)
 * <path_n-1>
 * ----------------------------------------------------------------------------
 * <n>             number of include file hash entries (2 bytes unsigned int)
 * <index[0]>      index of include file path          (2 bytes unsigned int)
 * <hash[0]>       hash of include file                (16 bytes)
 * <size[0]>       size of include file                (4 bytes unsigned int)
 * ...
 * <hash[n-1]>
 * <size[n-1]>
 * <index[n-1]>
 * ----------------------------------------------------------------------------
 * <n>             number of object name entries       (2 bytes unsigned int)
 * <m[0]>          number of include file hash indexes (2 bytes unsigned int)
 * <index[0][0]>   include file hash index             (2 bytes unsigned int)
 * ...
 * <index[0][m[0]-1]>
 * <hash[0]>       hash part of object name            (16 bytes)
 * <size[0]>       size part of object name            (4 bytes unsigned int)
 * ...
 * <m[n-1]>        number of include file hash indexes
 * <index[n-1][0]> include file hash index
 * ...
 * <index[n-1][m[n-1]]>
 * <hash[n-1]>
 * <size[n-1]>
 */

#define MAGIC 0x63436d46U
#define VERSION 0

#define static_assert(e) do { enum { static_assert__ = 1/(e) }; } while (0)

struct file_info
{
	/* Index to n_files. */
	uint32_t index;
	/* Hash of referenced file. */
	uint8_t hash[16];
	/* Size of referenced file. */
	uint32_t size;
};

struct object
{
	/* Number of entries in file_info_indexes. */
	uint32_t n_file_info_indexes;
	/* Indexes to file_infos. */
	uint32_t *file_info_indexes;
	/* Hash of the object itself. */
	struct file_hash hash;
};

struct manifest
{
	/* Referenced include files. */
	uint32_t n_files;
	char **files;

	/* Information about referenced include files. */
	uint32_t n_file_infos;
	struct file_info *file_infos;

	/* Object names plus references to include file hashes. */
	uint32_t n_objects;
	struct object *objects;
};

static unsigned int hash_from_file_info(void *key)
{
	static_assert(sizeof(struct file_info) == 24); /* No padding. */
	return murmurhashneutral2(key, sizeof(struct file_info), 0);
}

static int file_infos_equal(void *key1, void *key2)
{
	struct file_info *fi1 = (struct file_info *)key1;
	struct file_info *fi2 = (struct file_info *)key2;
	return fi1->index == fi2->index
		&& memcmp(fi1->hash, fi2->hash, 16) == 0
		&& fi1->size == fi2->size;
}

static void free_manifest(struct manifest *mf)
{
	uint16_t i;
	for (i = 0; i < mf->n_files; i++) {
		free(mf->files[i]);
	}
	free(mf->files);
	free(mf->file_infos);
	for (i = 0; i < mf->n_objects; i++) {
		free(mf->objects[i].file_info_indexes);
	}
	free(mf->objects);
}

#define READ_INT(size, var)				\
	do {						\
		int ch_;				\
		size_t i_;				\
		(var) = 0;				\
		for (i_ = 0; i_ < (size); i_++) {	\
			ch_ = getc(f);			\
			if (ch_ == EOF) {		\
				goto error;		\
			}				\
			(var) <<= 8;			\
			(var) |= ch_ & 0xFF;		\
		}					\
	} while (0)

#define READ_STR(var)					\
	do {						\
		char buf_[1024];			\
		size_t i_;				\
		int ch_;				\
		for (i_ = 0; i_ < sizeof(buf_); i_++) {	\
			ch_ = getc(f);			\
			if (ch_ == EOF) {		\
				goto error;		\
			}				\
			buf_[i_] = ch_;			\
			if (ch_ == '\0') {		\
				break;			\
			}				\
		}					\
		if (i_ == sizeof(buf_)) {		\
			goto error;			\
		}					\
		(var) = x_strdup(buf_);			\
	} while (0)

#define READ_BYTES(n, var)			\
	do {					\
		size_t i_;			\
		int ch_;			\
		for (i_ = 0; i_ < (n); i_++) {	\
			ch_ = getc(f);		\
			if (ch_ == EOF) {	\
				goto error;	\
			}			\
			(var)[i_] = ch_;	\
		}				\
	} while (0)

static struct manifest *read_manifest(FILE *f)
{
	struct stat st;
	struct manifest *mf;
	uint16_t i, j;
	size_t n;
	uint32_t magic, version;

	if (fstat(fileno(f), &st) != 0) {
		return NULL;
	}

	mf = x_malloc(sizeof(*mf));
	mf->n_files = 0;
	mf->files = NULL;
	mf->n_file_infos = 0;
	mf->file_infos = NULL;
	mf->n_objects = 0;
	mf->objects = NULL;

	if (st.st_size == 0) {
		/* New file. */
		return mf;
	}

	READ_INT(4, magic);
	if (magic != MAGIC) {
		cc_log("Manifest file has bad magic number %u\n", magic);
		free_manifest(mf);
		return NULL;
	}
	READ_INT(2, version);
	if (version != VERSION) {
		cc_log("Manifest file has unknown version %u\n", version);
		free_manifest(mf);
		return NULL;
	}

	READ_INT(2, mf->n_files);
	n = mf->n_files * sizeof(*mf->files);
	mf->files = x_malloc(n);
	memset(mf->files, 0, n);
	for (i = 0; i < mf->n_files; i++) {
		READ_STR(mf->files[i]);
	}

	READ_INT(2, mf->n_file_infos);
	n = mf->n_file_infos * sizeof(*mf->file_infos);
	mf->file_infos = x_malloc(n);
	memset(mf->file_infos, 0, n);
	for (i = 0; i < mf->n_file_infos; i++) {
		READ_INT(2, mf->file_infos[i].index);
		READ_BYTES(16, mf->file_infos[i].hash);
		READ_INT(4, mf->file_infos[i].size);
	}

	READ_INT(2, mf->n_objects);
	n = mf->n_objects * sizeof(*mf->objects);
	mf->objects = x_malloc(n);
	memset(mf->objects, 0, n);
	for (i = 0; i < mf->n_objects; i++) {
		READ_INT(2, mf->objects[i].n_file_info_indexes);
		n = mf->objects[i].n_file_info_indexes
		    * sizeof(*mf->objects[i].file_info_indexes);
		mf->objects[i].file_info_indexes = x_malloc(n);
		memset(mf->objects[i].file_info_indexes, 0, n);
		for (j = 0; j < mf->objects[i].n_file_info_indexes; j++) {
			READ_INT(2, mf->objects[i].file_info_indexes[j]);
		}
		READ_BYTES(16, mf->objects[i].hash.hash);
		READ_INT(4, mf->objects[i].hash.size);
	}

	return mf;

error:
	cc_log("Corrupt manifest file\n");
	free_manifest(mf);
	return NULL;
}

#define WRITE_INT(size, var)						\
	do {								\
		char ch_;						\
		size_t i_;						\
		for (i_ = 0; i_ < (size); i_++) {			\
			ch_ = ((var) >> (8 * ((size) - i_ - 1)));	\
			if (putc(ch_, f) == EOF) {			\
				goto error;				\
			}						\
		}							\
	} while (0)

#define WRITE_STR(var)							\
	do {								\
		if (fputs(var, f) == EOF || putc('\0', f) == EOF) {	\
			goto error;					\
		}							\
	} while (0)

#define WRITE_BYTES(n, var)					\
	do {							\
		size_t i_;					\
		for (i_ = 0; i_ < (n); i_++) {			\
			if (putc((var)[i_], f) == EOF) {	\
				goto error;			\
			}					\
		}						\
	} while (0)

static int write_manifest(FILE *f, const struct manifest *mf)
{
	uint16_t i, j;

	WRITE_INT(4, MAGIC);
	WRITE_INT(2, VERSION);

	WRITE_INT(2, mf->n_files);
	for (i = 0; i < mf->n_files; i++) {
		WRITE_STR(mf->files[i]);
	}

	WRITE_INT(2, mf->n_file_infos);
	for (i = 0; i < mf->n_file_infos; i++) {
		WRITE_INT(2, mf->file_infos[i].index);
		WRITE_BYTES(16, mf->file_infos[i].hash);
		WRITE_INT(4, mf->file_infos[i].size);
	}

	WRITE_INT(2, mf->n_objects);
	for (i = 0; i < mf->n_objects; i++) {
		WRITE_INT(2, mf->objects[i].n_file_info_indexes);
		for (j = 0; j < mf->objects[i].n_file_info_indexes; j++) {
			WRITE_INT(2, mf->objects[i].file_info_indexes[j]);
		}
		WRITE_BYTES(16, mf->objects[i].hash.hash);
		WRITE_INT(4, mf->objects[i].hash.size);
	}

	return 1;

error:
	cc_log("Error writing to manifest file");
	return 0;
}

static int verify_object(struct manifest *mf, struct object *obj,
                         struct hashtable *hashed_files)
{
	uint32_t i;
	struct file_info *fi;
	struct file_hash *actual;
	struct mdfour hash;

	for (i = 0; i < obj->n_file_info_indexes; i++) {
		fi = &mf->file_infos[obj->file_info_indexes[i]];
		actual = hashtable_search(hashed_files, mf->files[fi->index]);
		if (!actual) {
			actual = x_malloc(sizeof(*actual));
			hash_start(&hash);
			if (!hash_file(&hash, mf->files[fi->index])) {
				cc_log("Failed hashing %s\n",
				       mf->files[fi->index]);
				free(actual);
				return 0;
			}
			hash_result_as_bytes(&hash, actual->hash);
			actual->size = hash.totalN;
			hashtable_insert(hashed_files,
			                 x_strdup(mf->files[fi->index]),
			                 actual);
		}
		if (memcmp(fi->hash, actual->hash, 16) != 0
		    || fi->size != actual->size) {
			return 0;
		}
	}

	return 1;
}

static struct hashtable *create_string_index_map(char **strings, uint32_t len)
{
	uint32_t i;
	struct hashtable *h;
	uint32_t *index;

	h = create_hashtable(1000, hash_from_string, strings_equal);
	for (i = 0; i < len; i++) {
		index = x_malloc(sizeof(*index));
		*index = i;
		hashtable_insert(h, x_strdup(strings[i]), index);
	}
	return h;
}

static struct hashtable *create_file_info_index_map(struct file_info *infos,
                                                    uint32_t len)
{
	uint32_t i;
	struct hashtable *h;
	struct file_info *fi;
	uint32_t *index;

	h = create_hashtable(1000, hash_from_file_info, file_infos_equal);
	for (i = 0; i < len; i++) {
		fi = x_malloc(sizeof(*fi));
		*fi = infos[i];
		index = x_malloc(sizeof(*index));
		*index = i;
		hashtable_insert(h, fi, index);
	}
	return h;
}

static uint32_t get_include_file_index(struct manifest *mf,
                                       char *path,
                                       struct hashtable *mf_files)
{
	uint32_t *index;
	uint32_t n;

	index = hashtable_search(mf_files, path);
	if (index) {
		return *index;
	}

	n = mf->n_files;
	mf->files = x_realloc(mf->files, (n + 1) * sizeof(*mf->files));
	mf->n_files++;
	mf->files[n] = x_strdup(path);

	return n;
}

static uint32 get_file_hash_index(struct manifest *mf,
                                  char *path,
                                  struct file_hash *file_hash,
                                  struct hashtable *mf_files,
                                  struct hashtable *mf_file_infos)
{
	struct file_info fi;
	uint32_t *fi_index;
	uint32_t n;

	fi.index = get_include_file_index(mf, path, mf_files);
	memcpy(fi.hash, file_hash->hash, sizeof(fi.hash));
	fi.size = file_hash->size;

	fi_index = hashtable_search(mf_file_infos, &fi);
	if (fi_index) {
		return *fi_index;
	}

	n = mf->n_file_infos;
	mf->file_infos = x_realloc(mf->file_infos,
	                           (n + 1) * sizeof(*mf->file_infos));
	mf->n_file_infos++;
	mf->file_infos[n] = fi;

	return n;
}

static void
add_file_info_indexes(uint32_t *indexes, uint32_t size,
                      struct manifest *mf, struct hashtable *included_files)
{
	struct hashtable_itr *iter;
	uint32_t i;
	char *path;
	struct file_hash *file_hash;
	struct hashtable *mf_files; /* path --> index */
	struct hashtable *mf_file_infos; /* struct file_info --> index */

	if (size == 0) {
		return;
	}

	mf_files = create_string_index_map(mf->files, mf->n_files);
	mf_file_infos = create_file_info_index_map(mf->file_infos,
	                                           mf->n_file_infos);
	iter = hashtable_iterator(included_files);
	i = 0;
	do {
		path = hashtable_iterator_key(iter);
		file_hash = hashtable_iterator_value(iter);
		indexes[i] = get_file_hash_index(mf, path, file_hash, mf_files,
		                                 mf_file_infos);
		i++;
	} while (hashtable_iterator_advance(iter));
	assert(i == size);

	hashtable_destroy(mf_file_infos, 1);
	hashtable_destroy(mf_files, 1);
}

static void add_object_entry(struct manifest *mf,
                             struct file_hash *object_hash,
                             struct hashtable *included_files)
{
	struct object *obj;
	uint32_t n;

	n = mf->n_objects;
	mf->objects = x_realloc(mf->objects, (n + 1) * sizeof(*mf->objects));
	mf->n_objects++;
	obj = &mf->objects[n];

	n = hashtable_count(included_files);
	obj->n_file_info_indexes = n;
	obj->file_info_indexes = x_malloc(n * sizeof(*obj->file_info_indexes));
	add_file_info_indexes(obj->file_info_indexes, n, mf, included_files);
	memcpy(obj->hash.hash, object_hash->hash, 16);
	obj->hash.size = object_hash->size;
}

/*
 * Try to get the object hash from a manifest file. Caller frees. Returns NULL
 * on failure.
 */
struct file_hash *manifest_get(const char *manifest_path)
{
	int fd;
	FILE *f = NULL;
	struct manifest *mf = NULL;
	struct hashtable *hashed_files = NULL; /* path --> struct file_hash */
	uint32_t i;
	struct file_hash *fh = NULL;

	fd = open(manifest_path, O_RDONLY);
	if (fd == -1) {
		/* Cache miss. */
		goto out;
	}
	if (read_lock_fd(fd) == -1) {
		cc_log("Failed to read lock %s\n", manifest_path);
		goto out;
	}
	f = fdopen(fd, "rb");
	if (!f) {
		cc_log("Failed to fdopen lock %s\n", manifest_path);
		goto out;
	}
	mf = read_manifest(f);
	if (!mf) {
		cc_log("Error reading %s\n", manifest_path);
		goto out;
	}

	hashed_files = create_hashtable(1000, hash_from_string, strings_equal);

	/* Check newest object first since it's a bit more likely to match. */
	for (i = mf->n_objects; i > 0; i--) {
		if (verify_object(mf, &mf->objects[i - 1], hashed_files)) {
			fh = x_malloc(sizeof(*fh));
			*fh = mf->objects[i - 1].hash;
			goto out;
		}
	}

out:
	if (hashed_files) {
		hashtable_destroy(hashed_files, 1);
	}
	if (f) {
		fclose(f);
	}
	if (mf) {
		free_manifest(mf);
	}
	return fh;
}

/*
 * Put the object name into a manifest file given a set of included files.
 * Returns 1 on success, otherwise 0.
 */
int manifest_put(const char *manifest_path, struct file_hash *object_hash,
                 struct hashtable *included_files)
{
	int ret = 0;
	int fd1;
	int fd2;
	FILE *f1 = NULL;
	FILE *f2 = NULL;
	struct manifest *mf = NULL;
	char *tmp_file = NULL;

	fd1 = safe_open(manifest_path);
	if (fd1 == -1) {
		cc_log("Failed to open %s\n", manifest_path);
		goto out;
	}
	if (write_lock_fd(fd1) == -1) {
		cc_log("Failed to write lock %s\n", manifest_path);
		close(fd1);
		goto out;
	}
	f1 = fdopen(fd1, "rb");
	if (!f1) {
		cc_log("Failed to fdopen %s\n", manifest_path);
		close(fd1);
		goto out;
	}
	mf = read_manifest(f1);
	if (!mf) {
		cc_log("Failed to read %s\n", manifest_path);
		goto out;
	}

	x_asprintf(&tmp_file, "%s/manifest.tmp.%s", temp_dir, tmp_string());

	fd2 = safe_open(tmp_file);
	if (fd2 == -1) {
		cc_log("Failed to open %s\n", tmp_file);
		goto out;
	}
	f2 = fdopen(fd2, "wb");
	if (!f2) {
		cc_log("Failed to fdopen %s\n", tmp_file);
		goto out;
	}

	add_object_entry(mf, object_hash, included_files);
	if (write_manifest(f2, mf)) {
		if (rename(tmp_file, manifest_path) == 0) {
			ret = 1;
		} else {
			cc_log("Failed to rename %s to %s\n",
			       tmp_file, manifest_path);
			goto out;
		}
	} else {
		cc_log("Failed to write manifest %s\n", manifest_path);
		goto out;
	}

out:
	if (mf) {
		free_manifest(mf);
	}
	if (tmp_file) {
		free(tmp_file);
	}
	if (f2) {
		fclose(f2);
	}
	if (f1) {
		fclose(f1);
	}
	return ret;
}