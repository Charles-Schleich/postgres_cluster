/*-------------------------------------------------------------------------
 *
 * validate.c: validate backup files.
 *
 * Copyright (c) 2009-2011, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */

#include "pg_probackup.h"

#include <sys/stat.h>
#include <pthread.h>

static void pgBackupValidateFiles(void *arg);

typedef struct
{
	parray *files;
	const char *root;
	bool size_only;
	bool corrupted;
} validate_files_args;

/*
 * Validate files in the backup and update its status to OK.
 * If any of files are corrupted, update its stutus to CORRUPT.
 */
int
do_validate(time_t backup_id)
{
	int		i;
	parray *backup_list;
	int ret;
	bool another_pg_probackup = false;

	ret = catalog_lock();
	if (ret == 1)
		another_pg_probackup = true;

	/* get backup list matches given range */
	backup_list = catalog_get_backup_list(backup_id);
	if (!backup_list)
		elog(ERROR, "cannot process any more.");

	parray_qsort(backup_list, pgBackupCompareId);
	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup *backup = (pgBackup *)parray_get(backup_list, i);

		/* clean extra backups (switch STATUS to ERROR) */
		if (!another_pg_probackup &&
			(backup->status == BACKUP_STATUS_RUNNING ||
			 backup->status == BACKUP_STATUS_DELETING))
		{
			backup->status = BACKUP_STATUS_ERROR;
			pgBackupWriteIni(backup);
		}

		/* Validate completed backups only. */
		if (backup_id == 0 && backup->status != BACKUP_STATUS_DONE)
			continue;

		if (backup_id != 0 && backup->start_time == backup_id)
		{
			pgBackupValidate(backup, false, false);
			break;
		}
		else
			/* validate with CRC value and update status to OK */
			pgBackupValidate(backup, false, false);

	}

	/* cleanup */
	parray_walk(backup_list, pgBackupFree);
	parray_free(backup_list);

	catalog_unlock();

	return 0;
}

/*
 * Validate each files in the backup with its size.
 */
void
pgBackupValidate(pgBackup *backup,
				 bool size_only,
				 bool for_get_timeline)
{
	char	timestamp[100];
	char	base_path[MAXPGPATH];
	char	path[MAXPGPATH];
	parray *files;
	bool	corrupted = false;
	pthread_t	validate_threads[num_threads];
	validate_files_args *validate_threads_args[num_threads];

	time2iso(timestamp, lengthof(timestamp), backup->recovery_time);
	if (!for_get_timeline)
	{
		if (backup->backup_mode == BACKUP_MODE_FULL ||
			backup->backup_mode == BACKUP_MODE_DIFF_PAGE ||
			backup->backup_mode == BACKUP_MODE_DIFF_PTRACK)
			elog(INFO, "validate: %s backup and archive log files by %s",
				 timestamp, (size_only ? "SIZE" : "CRC"));
	}

	if (!check)
	{
		if (backup->backup_mode == BACKUP_MODE_FULL ||
			backup->backup_mode == BACKUP_MODE_DIFF_PAGE ||
			backup->backup_mode == BACKUP_MODE_DIFF_PTRACK)
		{
			int i;
			elog(LOG, "database files...");
			pgBackupGetPath(backup, base_path, lengthof(base_path), DATABASE_DIR);
			pgBackupGetPath(backup, path, lengthof(path),
				DATABASE_FILE_LIST);
			files = dir_read_file_list(base_path, path);

			/* setup threads */
			for (i = 0; i < parray_num(files); i++)
			{
				pgFile *file = (pgFile *) parray_get(files, i);
				__sync_lock_release(&file->lock);
			}

			/* restore files into $PGDATA */
			for (i = 0; i < num_threads; i++)
			{
				validate_files_args *arg = pg_malloc(sizeof(validate_files_args));
				arg->files = files;
				arg->root = base_path;
				arg->size_only = size_only;
				arg->corrupted = false;

				validate_threads_args[i] = arg;
				pthread_create(&validate_threads[i], NULL, (void *(*)(void *)) pgBackupValidateFiles, arg);
			}

			/* Wait theads */
			for (i = 0; i < num_threads; i++)
			{
				pthread_join(validate_threads[i], NULL);
				if (validate_threads_args[i]->corrupted)
					corrupted = true;
				pg_free(validate_threads_args[i]);
			}
			parray_walk(files, pgFileFree);
			parray_free(files);
		}

		/* update status to OK */
		if (corrupted)
			backup->status = BACKUP_STATUS_CORRUPT;
		else
			backup->status = BACKUP_STATUS_OK;
		pgBackupWriteIni(backup);

		if (corrupted)
			elog(WARNING, "backup %s is corrupted", timestamp);
		else
			elog(LOG, "backup %s is valid", timestamp);
	}
}

static const char *
get_relative_path(const char *path, const char *root)
{
	size_t	rootlen = strlen(root);
	if (strncmp(path, root, rootlen) == 0 && path[rootlen] == '/')
		return path + rootlen + 1;
	else
		return path;
}

/*
 * Validate files in the backup with size or CRC.
 */
static void
pgBackupValidateFiles(void *arg)
{
	int		i;

	validate_files_args *arguments = (validate_files_args *)arg;


	for (i = 0; i < parray_num(arguments->files); i++)
	{
		struct stat st;

		pgFile *file = (pgFile *) parray_get(arguments->files, i);
		if (__sync_lock_test_and_set(&file->lock, 1) != 0)
			continue;

		if (interrupted)
			elog(ERROR, "interrupted during validate");

		/* skipped backup while differential backup */
		if (file->write_size == BYTES_INVALID
			|| !S_ISREG(file->mode)
			|| file->generation != -1)
			continue;

		/* print progress */
		elog(LOG, "(%d/%lu) %s", i + 1, (unsigned long) parray_num(arguments->files),
			get_relative_path(file->path, arguments->root));

		/* always validate file size */
		if (stat(file->path, &st) == -1)
		{
			if (errno == ENOENT)
				elog(WARNING, "backup file \"%s\" vanished", file->path);
			else
				elog(ERROR, "cannot stat backup file \"%s\": %s",
					get_relative_path(file->path, arguments->root), strerror(errno));
			arguments->corrupted = true;
			return;
		}
		if (file->write_size != st.st_size)
		{
			elog(WARNING, "size of backup file \"%s\" must be %lu but %lu",
				get_relative_path(file->path, arguments->root),
				(unsigned long) file->write_size,
				(unsigned long) st.st_size);
			arguments->corrupted = true;
			return;
		}

		/* validate CRC too */
		if (!arguments->size_only)
		{
			pg_crc32	crc;

			crc = pgFileGetCRC(file);
			if (crc != file->crc)
			{
				elog(WARNING, "CRC of backup file \"%s\" must be %X but %X",
					get_relative_path(file->path, arguments->root), file->crc, crc);
				arguments->corrupted = true;
				return;
			}
		}
	}
}
