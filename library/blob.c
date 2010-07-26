/*
 * 2008+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "eblob/blob.h"
#include "hash.h"

#define EBLOB_BLOB_INDEX_SUFFIX			".index"
#define EBLOB_BLOB_DEFAULT_HASH_SIZE		1024*1024*10
#define EBLOB_BLOB_DEFAULT_BLOB_SIZE		50*1024*1024*1024ULL

struct eblob_backend {
	struct eblob_config	cfg;

	pthread_mutex_t		lock;

	int			index;
	struct eblob_backend_io	*data;

	struct eblob_hash	*hash;
};

struct blob_ram_control {
	size_t			offset;
	off_t			index_pos;
	uint64_t		size;

	int			file_index;
};

struct eblob_blob_iterator_data {
	pthread_t		id;

	struct eblob_backend	*b;
	struct eblob_backend_io	*io;

	size_t			size;
	off_t			off;

	int			(* iterator)(struct eblob_disk_control *dc, int file_index,
					void *data, off_t position, void *priv);
	void			*priv;

	int			check_index;

	int			err;
};

static void *eblob_blob_iterator(void *data)
{
	struct eblob_blob_iterator_data *p = data;

	p->err = eblob_iterate(p->io, p->off, p->size, p->b->cfg.log, p->check_index, p->iterator, p->priv);
	if (p->err)
		eblob_log(p->b->cfg.log, EBLOB_LOG_ERROR, "blob: data iteration failed: %d.\n", p->err);

	return &p->err;
};

int eblob_blob_iterate(struct eblob_backend *b, int check_index,
	int (* iterator)(struct eblob_disk_control *dc, int file_index, void *data, off_t position, void *priv),
	void *priv)
{
	int iterate_threads = b->cfg.iterate_threads;
	int j, index_num = b->index;
	int error = 0;

	for (j=0; j<index_num + 1; ++j) {
		struct eblob_backend_io *io = &b->data[j];

		if (!io->index_pos)
			break;

		if (!check_index || (uint64_t)io->index_pos < iterate_threads + b->cfg.blob_size / sizeof(struct eblob_disk_control))
			iterate_threads = 1;

		{
			int i, err;
			int thread_num = iterate_threads - 1;
			struct eblob_blob_iterator_data p[thread_num + 1];
			off_t off = 0;
			size_t size = check_index ? io->index_pos * sizeof(struct eblob_disk_control) / iterate_threads : io->offset;
			off_t rest = check_index ? io->index_pos * sizeof(struct eblob_disk_control) : io->offset;

			memset(p, 0, sizeof(p));

			for (i=0; i<thread_num + 1; ++i) {
				p[i].check_index = check_index;
				p[i].size = size;
				p[i].off = off;
				p[i].b = b;
				p[i].io = io;
				p[i].iterator = iterator;
				p[i].priv = priv;

				off += size;
				rest -= size;
			}
			p[thread_num].size = rest + size;

			for (i=0; i<thread_num; ++i) {
				err = pthread_create(&p[i].id, NULL, eblob_blob_iterator, &p[i]);
				if (err) {
					eblob_log(b->cfg.log, EBLOB_LOG_ERROR, "blob: failed to create iterator thread: %d.\n", err);
					break;
				}
			}

			eblob_blob_iterator(&p[thread_num]);

			error = p[thread_num].err;

			for (i=0; i<thread_num; ++i) {
				pthread_join(p[i].id, NULL);

				if (p[i].err)
					error = p[i].err;
			}

			posix_fadvise(io->fd, 0, io->offset, POSIX_FADV_RANDOM);

			eblob_log(b->cfg.log, EBLOB_LOG_INFO, "blob: %d/%d: iteration completed: num: %llu, threads: %u, status: %d.\n",
					j, index_num, (unsigned long long)io->index_pos, iterate_threads, error);
		}

	}

	return error;
}

static int eblob_blob_open_file(char *file, off_t *off_ptr)
{
	int fd, err = 0;
	off_t offset;

	fd = open(file, O_RDWR | O_CREAT, 0644);
	if (fd < 0) {
		err = -errno;
		goto err_out_exit;
	}

	offset = lseek(fd, 0, SEEK_END);
	if (offset == (off_t) -1) {
		goto err_out_close;
	}

	posix_fadvise(fd, 0, offset, POSIX_FADV_SEQUENTIAL);

	*off_ptr = offset;
	return fd;

err_out_close:
	close(fd);
err_out_exit:
	return err;
}

static int eblob_blob_open_files(char *path, struct eblob_backend_io *io)
{
	char index[strlen(path)+sizeof(EBLOB_BLOB_INDEX_SUFFIX) + 1]; /* 0-byte */
	int err;

	io->fd = eblob_blob_open_file(path, &io->offset);
	if (io->fd < 0) {
		err = io->fd;
		goto err_out_exit;
	}

	sprintf(index, "%s%s", path, EBLOB_BLOB_INDEX_SUFFIX);

	io->index = eblob_blob_open_file(index, &io->index_pos);
	if (io->index < 0) {
		err = io->index;
		goto err_out_close;
	}

	io->index_pos = io->index_pos / sizeof(struct eblob_disk_control);

	return 0;

err_out_close:
	close(io->fd);
err_out_exit:
	return err;
}

static void eblob_blob_close_files_all(struct eblob_backend *b)
{
	int i;

	for (i=0; i<b->index; ++i)
		close(b->data[i].fd);
}

static int eblob_blob_extend_io(struct eblob_backend *b, struct eblob_backend_io *new_io, int num)
{
	struct eblob_backend_io *io = b->data;

	io = realloc(io, num * sizeof(struct eblob_backend_io));
	if (!io)
		return -ENOMEM;

	memcpy(&io[num - 1], new_io, sizeof(struct eblob_backend_io));

	b->data = io;
	b->index++;

	return 0;
}

static int eblob_blob_allocate_io(struct eblob_backend *b)
{
	/* 15 should be enough for file index in decimal */
	char file[strlen(b->cfg.file) + 16 + sizeof(EBLOB_BLOB_INDEX_SUFFIX)];
	struct eblob_backend_io tmp;
	int err, i = 0, last = 0, idx;

	idx = b->index + 1;

	for (i=idx; ; i++) {
		snprintf(file, sizeof(file), "%s.%d", b->cfg.file, i);

		err = open(file, O_RDWR);
		if (err < 0 && (errno == ENOENT)) {
			last = 1;
			if (i > idx) {
				err = -errno;
				break;
			}
		}
		if (err >= 0)
			close(err);

		memset(&tmp, 0, sizeof(tmp));

		err = eblob_blob_open_files(file, &tmp);
		if (err) {
			eblob_log(b->cfg.log, EBLOB_LOG_INFO, "Failed to open files: pattern: %s, index: %d, err: %d.\n",
					file, i, err);
			break;
		}

		tmp.file_index = i;

		eblob_log(b->cfg.log, EBLOB_LOG_INFO, "file: %s, file_index: %d, size: %llu, indexed %llu entries, fds: %d, %d.\n",
			file, tmp.file_index, tmp.offset, tmp.index_pos, tmp.fd, tmp.index);

		err = eblob_blob_extend_io(b, &tmp, i + 1);
		if (err)
			break;

		if (last)
			break;
	}

	if (i && (err == -ENOENT))
		err = 0;

	return err;
}

static int blob_mark_index_removed(int fd, off_t offset)
{
	uint64_t flags = eblob_bswap64(BLOB_DISK_CTL_REMOVE);
	int err;

	err = pwrite(fd, &flags, sizeof(flags), offset + offsetof(struct eblob_disk_control, flags));
	if (err != (int)sizeof(flags))
		err = -errno;

	return 0;
}

static int blob_update_index(struct eblob_backend *b, struct eblob_backend_io *io, unsigned char *key, unsigned int ksize,
		struct blob_ram_control *data_ctl, struct blob_ram_control *old)
{
	struct eblob_disk_control dc;
	off_t *offset = &io->index_pos;
	int err;

	memset(&dc, 0, sizeof(struct eblob_disk_control));

	memcpy(dc.id, key, (ksize < EBLOB_ID_SIZE) ? ksize : EBLOB_ID_SIZE);
	dc.flags = 0;
	dc.data_size = data_ctl->size;
	dc.disk_size = sizeof(struct eblob_disk_control);
	dc.position = data_ctl->offset;

	eblob_log(b->cfg.log, EBLOB_LOG_NOTICE, "%s: updated index at position %llu (0x%llx), data position: %llu (0x%llx), data size: %llu.\n",
			eblob_dump_id(key),
			(unsigned long long)(*offset)*sizeof(dc), (unsigned long long)(*offset)*sizeof(dc),
			(unsigned long long)data_ctl->offset, (unsigned long long)data_ctl->offset,
			data_ctl->size);

	blob_convert_disk_control(&dc);

	err = pwrite(io->index, &dc, sizeof(dc), (*offset)*sizeof(dc));
	if (err != (int)sizeof(dc)) {
		err = -errno;
		eblob_log(b->cfg.log, EBLOB_LOG_ERROR, "%s: failed to write index data at %llu: %s.\n",
			eblob_dump_id(key), (unsigned long long)(*offset)*sizeof(dc), strerror(errno));
		goto err_out_exit;
	}

	eblob_log(b->cfg.log, EBLOB_LOG_NOTICE, "%s: wrote %u bytes at %llu into %d\n",
			eblob_dump_id(key), sizeof(dc), (*offset)*sizeof(dc), io->index);

	*offset = *offset + 1;
	err = 0;

	if (old) {
		io = &b->data[old->file_index];


		eblob_log(b->cfg.log, EBLOB_LOG_NOTICE, "backend: marking index entry as removed: "
			"position: %llu (0x%llx)/fd: %d, position: %llu (0x%llx)/fd: %d.\n",
			(unsigned long long)old->index_pos * sizeof(dc),
			(unsigned long long)old->index_pos * sizeof(dc), io->index,
			(unsigned long long)old->offset,
			(unsigned long long)old->offset, io->fd);

		blob_mark_index_removed(io->index, old->index_pos * sizeof(dc));
		blob_mark_index_removed(io->fd, old->offset);
	}

err_out_exit:
	return err;
}

static int blob_write_low_level(int fd, void *data, size_t size, size_t offset)
{
	ssize_t err = 0;

	while (size) {
		err = pwrite(fd, data, size, offset);
		if (err <= 0) {
			err = -errno;
			if (!err)
				err = -EINVAL;
			goto err_out_exit;
		}

		data += err;
		size -= err;
		offset += err;
	}

	err = 0;

err_out_exit:
	return err;
}

static unsigned char blob_empty_buf[40960];

static ssize_t blob_write_data(struct eblob_backend *b, struct eblob_backend_io *io, unsigned char *key, unsigned int ksize,
		void *data, uint64_t offset __eblob_unused, uint64_t size, uint64_t flags)
{
	struct eblob_disk_control disk_ctl;
	struct blob_ram_control ctl, old;
	unsigned int dsize = sizeof(old);
	off_t local_offset;
	size_t disk_size;
	ssize_t err;
	int have_old = 0, bsize = b->cfg.bsize;

	memset(&disk_ctl, 0, sizeof(disk_ctl));

	ctl.offset = io->offset;
	ctl.index_pos = io->index_pos;
	ctl.file_index = io->file_index;

	disk_ctl.flags = flags;
	disk_ctl.position = ctl.offset;
	disk_ctl.data_size = size;
	disk_ctl.disk_size = size + sizeof(struct eblob_disk_control);
	if (bsize)
		disk_ctl.disk_size = ALIGN(disk_ctl.disk_size, bsize);

	memcpy(disk_ctl.id, key, (ksize < EBLOB_ID_SIZE) ? ksize : EBLOB_ID_SIZE);

	blob_convert_disk_control(&disk_ctl);

	local_offset = ctl.offset;
	err = blob_write_low_level(io->fd, &disk_ctl, sizeof(struct eblob_disk_control), local_offset);
	if (err)
		goto err_out_exit;
	local_offset += sizeof(struct eblob_disk_control);

	err = blob_write_low_level(io->fd, data, size, local_offset);
	if (err)
		goto err_out_exit;
	local_offset += size;

	if (bsize) {
		int alignment = bsize - ((local_offset - ctl.offset) % bsize);

		while (alignment && alignment < bsize) {
			unsigned int sz = alignment;

			if (sz > sizeof(blob_empty_buf))
				sz = sizeof(blob_empty_buf);

			err = blob_write_low_level(io->fd, blob_empty_buf, sz, local_offset);
			if (err)
				goto err_out_exit;

			alignment -= sz;
			local_offset += sz;
		}
	}
	disk_size = local_offset - ctl.offset;
	ctl.size = size;

	err = eblob_hash_lookup(b->hash, key, ksize, &old, &dsize);
	if (!err)
		have_old = 1;

	err = eblob_hash_replace(b->hash, key, ksize, &ctl, sizeof(ctl));
	if (err) {
		eblob_log(b->cfg.log, EBLOB_LOG_ERROR, "blob: %s: failed to add hash entry: %s [%d].\n",
				eblob_dump_id(key), strerror(-err), err);
		goto err_out_exit;
	}

	io->offset += disk_size;

	err = blob_update_index(b, io, key, ksize, &ctl, have_old ? &old : NULL);
	if (err)
		goto err_out_exit;

	eblob_log(b->cfg.log, EBLOB_LOG_INFO, "blob: %s: written data at position: %zu, size: %llu, on-disk-size: %zu.\n",
			eblob_dump_id(key), ctl.offset, (unsigned long long)size, disk_size);

	return 0;

err_out_exit:
	eblob_log(b->cfg.log, EBLOB_LOG_ERROR, "blob: %s: failed (%zd) to write %llu bytes into (fd: %d) datafile: %s.\n",
			eblob_dump_id(key), err, (unsigned long long)size, io->fd, strerror(-err));
	return err;
}

int eblob_write_data(struct eblob_backend *b, unsigned char *key, unsigned int ksize,
		void *data, uint64_t offset, uint64_t size, uint64_t flags)
{
	ssize_t err;
	struct eblob_backend_io *io;

	pthread_mutex_lock(&b->lock);

	io = &b->data[b->index];

	err = blob_write_data(b, io, key, ksize, data, offset, size, flags);
	if (err)
		goto err_out_unlock;

	if (io->offset >= (off_t)b->cfg.blob_size) {
		err = eblob_blob_allocate_io(b);
		if (err)
			goto err_out_unlock;
	}

err_out_unlock:
	pthread_mutex_unlock(&b->lock);
	return err;
}

int eblob_remove(struct eblob_backend *b, unsigned char *key, unsigned int ksize)
{
	struct blob_ram_control ctl;
	unsigned int dsize = sizeof(struct blob_ram_control);
	struct eblob_disk_control dc;
	int err, fd;

	err = eblob_hash_lookup(b->hash, key, ksize, &ctl, &dsize);
	if (err) {
		eblob_log(b->cfg.log, EBLOB_LOG_ERROR, "blob: %s: could not find data to be removed: %d.\n",
				eblob_dump_id(key), err);
		goto err_out_exit;
	}

	fd = b->data[ctl.file_index].fd;

	eblob_log(b->cfg.log, EBLOB_LOG_INFO, "%s: removing block at: %llu, size: %llu.\n",
		eblob_dump_id(key), (unsigned long long)ctl.offset, (unsigned long long)ctl.size);

	err = pread(fd, &dc, sizeof(dc), ctl.offset);
	if (err != (int)sizeof(dc)) {
		err = -errno;
		eblob_log(b->cfg.log, EBLOB_LOG_ERROR, "%s: failed to read disk control structure from history at %llu: %s.\n",
			eblob_dump_id(key), (unsigned long long)ctl.offset, strerror(errno));
		goto err_out_exit;
	}

	blob_convert_disk_control(&dc);
	dc.flags |= BLOB_DISK_CTL_REMOVE;
	blob_convert_disk_control(&dc);

	err = pwrite(fd, &dc, sizeof(struct eblob_disk_control), ctl.offset);
	if (err != (int)sizeof(dc)) {
		err = -errno;
		eblob_log(b->cfg.log, EBLOB_LOG_ERROR, "%s: failed to erase (mark) entry at %llu: %s.\n",
			eblob_dump_id(key), (unsigned long long)ctl.offset, strerror(errno));
		goto err_out_exit;
	}
	err = 0;

	blob_mark_index_removed(b->data[ctl.file_index].index, ctl.index_pos * sizeof(struct eblob_disk_control));

err_out_exit:
	return err;
}

int eblob_read(struct eblob_backend *b, unsigned char *key, unsigned int ksize, int *fd, uint64_t *offset, uint64_t *size)
{
	unsigned int dsize = sizeof(struct blob_ram_control);
	struct blob_ram_control ctl;
	int err;

	err = eblob_hash_lookup(b->hash, key, ksize, &ctl, &dsize);
	if (err) {
		eblob_log(b->cfg.log, EBLOB_LOG_ERROR, "blob: %s: could not find data: %d.\n",
				eblob_dump_id(key), err);
		goto err_out_exit;
	}

	*fd = b->data[ctl.file_index].fd;
	*size = ctl.size;
	*offset = ctl.offset;

err_out_exit:
	return err;
}

static int eblob_blob_iter(struct eblob_disk_control *dc, int file_index,
		void *data __eblob_unused, off_t position __eblob_unused, void *priv)
{
	struct eblob_backend *b = priv;
	struct blob_ram_control ctl;
	unsigned char key[EBLOB_ID_SIZE];
	char id[EBLOB_ID_SIZE*2+1];
	int err;

	eblob_log(b->cfg.log, EBLOB_LOG_NOTICE, "%s: file index: %d, index position: %llu (0x%llx), "
			"data position: %llu (0x%llx), data size: %llu, disk size: %llu, flags: %llx.\n",
			eblob_dump_id_len_raw(dc->id, EBLOB_ID_SIZE, id), file_index,
			(unsigned long long)position, (unsigned long long)position,
			(unsigned long long)dc->position, (unsigned long long)dc->position,
			(unsigned long long)dc->data_size, (unsigned long long)dc->disk_size,
			(unsigned long long)dc->flags);

	if (dc->flags & BLOB_DISK_CTL_REMOVE)
		return 0;

	memcpy(key, dc->id, EBLOB_ID_SIZE);
	ctl.index_pos = position / sizeof(struct eblob_disk_control);
	ctl.offset = dc->position;
	ctl.size = dc->data_size;
	ctl.file_index = file_index;

	err = eblob_hash_replace(b->hash, key, sizeof(key), &ctl, sizeof(ctl));
	if (err)
		return err;

	return 0;
}

void eblob_cleanup(struct eblob_backend *b)
{
	eblob_hash_exit(b->hash);
	eblob_blob_close_files_all(b);
	pthread_mutex_destroy(&b->lock);
	free(b);
}

struct eblob_backend *eblob_init(struct eblob_config *c)
{
	struct eblob_backend *b;
	int err;

	b = malloc(sizeof(struct eblob_backend));
	if (!b) {
		err = -ENOMEM;
		goto err_out_exit;
	}

	memset(b, 0, sizeof(struct eblob_backend));

	if (!c->blob_size)
		c->blob_size = EBLOB_BLOB_DEFAULT_BLOB_SIZE;

	if (!c->iterate_threads)
		c->iterate_threads = 1;

	if (!c->hash_size)
		c->hash_size = EBLOB_BLOB_DEFAULT_HASH_SIZE;

	memcpy(&b->cfg, c, sizeof(struct eblob_config));

	b->index = -1;

	b->cfg.file = strdup(c->file);
	if (!b->cfg.file) {
		err = -ENOMEM;
		goto err_out_free;
	}

	err = eblob_blob_allocate_io(b);
	if (err)
		goto err_out_free_file;

	err = pthread_mutex_init(&b->lock, NULL);
	if (err) {
		err = -errno;
		goto err_out_close;
	}

	b->hash = eblob_hash_init(c->hash_size, c->hash_flags);
	if (!b->hash) {
		err = -EINVAL;
		goto err_out_lock_destroy;
	}
	
	err = eblob_blob_iterate(b, 1, eblob_blob_iter, b);
	if (err) {
		eblob_log(b->cfg.log, EBLOB_LOG_ERROR, "blob: history iteration failed: %d.\n", err);
		goto err_out_hash_destroy;
	}

	return b;

err_out_hash_destroy:
	eblob_hash_exit(b->hash);
err_out_lock_destroy:
	pthread_mutex_destroy(&b->lock);
err_out_close:
	eblob_blob_close_files_all(b);
err_out_free_file:
	free(b->cfg.file);
err_out_free:
	free(b);
err_out_exit:
	return NULL;
}
