/*-------------------------------------------------------------------------
 *
 * restore.c: restore DB cluster and archived WAL.
 *
 * Copyright (c) 2009-2013, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */

#include "pg_probackup.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>

#include "catalog/pg_control.h"

typedef struct
{
	parray *files;
	pgBackup *backup;
} restore_files_args;

/* Tablespace mapping structures */

typedef struct TablespaceListCell
{
	struct TablespaceListCell *next;
	char		old_dir[MAXPGPATH];
	char		new_dir[MAXPGPATH];
} TablespaceListCell;

typedef struct TablespaceList
{
	TablespaceListCell *head;
	TablespaceListCell *tail;
} TablespaceList;

typedef struct TablespaceCreatedListCell
{
	struct TablespaceCreatedListCell *next;
	char		link_name[MAXPGPATH];
	char		linked_dir[MAXPGPATH];
} TablespaceCreatedListCell;

typedef struct TablespaceCreatedList
{
	TablespaceCreatedListCell *head;
	TablespaceCreatedListCell *tail;
} TablespaceCreatedList;

static void restore_database(pgBackup *backup);
static void restore_directories(const char *pg_data_dir,
								const char *backup_dir);
static void create_recovery_conf(time_t backup_id,
								 const char *target_time,
								 const char *target_xid,
								 const char *target_inclusive,
								 TimeLineID target_tli);
static void print_backup_lsn(const pgBackup *backup);
static void restore_files(void *arg);

static bool existsTimeLineHistory(TimeLineID probeTLI);

static const char *get_tablespace_mapping(const char *dir);
static void set_tablespace_created(const char *link, const char *dir);
static const char *get_tablespace_created(const char *link);

/* Tablespace mapping */
static TablespaceList tablespace_dirs = {NULL, NULL};
static TablespaceCreatedList tablespace_created_dirs = {NULL, NULL};


int
do_restore(time_t backup_id,
		   const char *target_time,
		   const char *target_xid,
		   const char *target_inclusive,
		   TimeLineID target_tli)
{
	int			i;
	int			base_index;				/* index of base (full) backup */
	int			ret;
	parray	   *backups;

	parray	   *timelines;
	pgBackup   *base_backup = NULL;
	pgBackup   *dest_backup = NULL;
	pgRecoveryTarget *rt = NULL;
	bool		backup_id_found = false;

	/* PGDATA and ARCLOG_PATH are always required */
	if (pgdata == NULL)
		elog(ERROR,
			 "required parameter not specified: PGDATA (-D, --pgdata)");

	elog(LOG, "========================================");
	elog(LOG, "restore start");

	/* get exclusive lock of backup catalog */
	ret = catalog_lock(false);
	if (ret == -1)
		elog(ERROR, "cannot lock backup catalog.");
	else if (ret == 1)
		elog(ERROR,
			 "another pg_probackup is running, stop restore.");

	/* confirm the PostgreSQL server is not running */
	if (is_pg_running())
		elog(ERROR, "PostgreSQL server is running");

	rt = checkIfCreateRecoveryConf(target_time, target_xid, target_inclusive);
	if (rt == NULL)
		elog(ERROR, "cannot create recovery.conf. specified args are invalid.");

	/* get list of backups. (index == 0) is the last backup */
	backups = catalog_get_backup_list(0);
	if (!backups)
		elog(ERROR, "cannot process any more.");

	if (target_tli)
	{
		elog(LOG, "target timeline ID = %u", target_tli);
		/* Read timeline history files from archives */
		timelines = readTimeLineHistory(target_tli);
	}

	/* find last full backup which can be used as base backup. */
	elog(LOG, "searching recent full backup");
	for (i = 0; i < parray_num(backups); i++)
	{
		base_backup = (pgBackup *) parray_get(backups, i);

		if (backup_id && base_backup->start_time > backup_id)
			continue;

		if (backup_id == base_backup->start_time &&
			base_backup->status == BACKUP_STATUS_OK)
		{
			backup_id_found = true;
			dest_backup = base_backup;
		}

		if (backup_id == base_backup->start_time &&
			base_backup->status != BACKUP_STATUS_OK)
			elog(ERROR, "given backup %s is %s", base36enc(backup_id),
				 status2str(base_backup->status));

		if (dest_backup != NULL &&
			base_backup->backup_mode == BACKUP_MODE_FULL &&
			base_backup->status != BACKUP_STATUS_OK)
			elog(ERROR, "base backup %s for given backup %s is %s",
				 base36enc(base_backup->start_time),
				 base36enc(dest_backup->start_time),
				 status2str(base_backup->status));

		if (base_backup->backup_mode < BACKUP_MODE_FULL ||
			base_backup->status != BACKUP_STATUS_OK)
			continue;

		if (target_tli)
		{
			if (satisfy_timeline(timelines, base_backup) &&
				satisfy_recovery_target(base_backup, rt) &&
				(backup_id_found || backup_id == 0))
				goto base_backup_found;
		}
		else
			if (satisfy_recovery_target(base_backup, rt) &&
				(backup_id_found || backup_id == 0))
				goto base_backup_found;

		backup_id_found = false;
	}
	/* no full backup found, cannot restore */
	elog(ERROR, "no full backup found, cannot restore.");

base_backup_found:
	base_index = i;

	/* Check if restore destination empty */
	if (!dir_is_empty(pgdata))
		elog(ERROR, "restore destination is not empty");

	print_backup_lsn(base_backup);

	if (backup_id != 0)
		stream_wal = base_backup->stream;

	/* restore base backup */
	restore_database(base_backup);

	/* restore following differential backup */
	elog(LOG, "searching differential backup...");

	for (i = base_index - 1; i >= 0; i--)
	{
		pgBackup *backup = (pgBackup *) parray_get(backups, i);

		/* don't use incomplete nor different timeline backup */
		if (backup->status != BACKUP_STATUS_OK ||
			backup->tli != base_backup->tli)
			continue;

		if (backup->backup_mode == BACKUP_MODE_FULL)
			break;

		if (backup_id && backup->start_time > backup_id)
			break;

		/* use database backup only */
		if (backup->backup_mode != BACKUP_MODE_DIFF_PAGE &&
			backup->backup_mode != BACKUP_MODE_DIFF_PTRACK)
			continue;

		/* is the backup is necessary for restore to target timeline ? */
		if (target_tli)
		{
			if (!satisfy_timeline(timelines, backup) ||
				!satisfy_recovery_target(backup, rt))
				continue;
		}
		else
			if (!satisfy_recovery_target(backup, rt))
				continue;

		if (backup_id != 0)
			stream_wal = backup->stream;

		print_backup_lsn(backup);
		restore_database(backup);
	}

	/* create recovery.conf */
	if (!stream_wal || target_time != NULL || target_xid != NULL)
		create_recovery_conf(backup_id, target_time, target_xid,
							 target_inclusive, base_backup->tli);

	/* release catalog lock */
	catalog_unlock();

	/* cleanup */
	parray_walk(backups, pgBackupFree);
	parray_free(backups);

	/* print restore complete message */
	if (!check)
	{
		elog(LOG, "all restore completed");
		elog(LOG, "========================================");
	}
	if (!check)
		elog(INFO, "restore complete. Recovery starts automatically when the PostgreSQL server is started.");

	return 0;
}

/*
 * Validate and restore backup.
 */
void
restore_database(pgBackup *backup)
{
	char		timestamp[100];
	char		backup_path[MAXPGPATH];
	char		database_path[MAXPGPATH];
	char		list_path[MAXPGPATH];
	parray	   *files;
	int			i;
	pthread_t	restore_threads[num_threads];
	restore_files_args *restore_threads_args[num_threads];

	/* confirm block size compatibility */
	if (backup->block_size != BLCKSZ)
		elog(ERROR,
			"BLCKSZ(%d) is not compatible(%d expected)",
			backup->block_size, BLCKSZ);
	if (backup->wal_block_size != XLOG_BLCKSZ)
		elog(ERROR,
			"XLOG_BLCKSZ(%d) is not compatible(%d expected)",
			backup->wal_block_size, XLOG_BLCKSZ);

	time2iso(timestamp, lengthof(timestamp), backup->start_time);
	if (!check)
	{
		elog(LOG, "----------------------------------------");
		elog(LOG, "restoring database from backup %s", timestamp);
	}

	/*
	 * Validate backup files with its size, because load of CRC calculation is
	 * not right.
	 */
	pgBackupValidate(backup, true, false);

	/*
	 * Restore backup directories.
	 */
	pgBackupGetPath(backup, backup_path, lengthof(backup_path), NULL);
	restore_directories(pgdata, backup_path);

	/*
	 * Get list of files which need to be restored.
	 */
	pgBackupGetPath(backup, database_path, lengthof(database_path), DATABASE_DIR);
	pgBackupGetPath(backup, list_path, lengthof(list_path), DATABASE_FILE_LIST);
	files = dir_read_file_list(database_path, list_path);
	for (i = parray_num(files) - 1; i >= 0; i--)
	{
		pgFile *file = (pgFile *) parray_get(files, i);

		/* Remove files which are not backed up */
		if (file->write_size == BYTES_INVALID)
			pgFileFree(parray_remove(files, i));
	}

	for (i = 0; i < parray_num(files); i++)
	{
		pgFile *file = (pgFile *) parray_get(files, i);

		__sync_lock_release(&file->lock);
	}

	/* Restore files into $PGDATA */
	for (i = 0; i < num_threads; i++)
	{
		restore_files_args *arg = pg_malloc(sizeof(restore_files_args));
		arg->files = files;
		arg->backup = backup;

		if (verbose)
			elog(WARNING, "Start thread for num:%li", parray_num(files));

		restore_threads_args[i] = arg;
		pthread_create(&restore_threads[i], NULL, (void *(*)(void *)) restore_files, arg);
	}

	/* Wait theads */
	for (i = 0; i < num_threads; i++)
	{
		pthread_join(restore_threads[i], NULL);
		pg_free(restore_threads_args[i]);
	}

	/* Delete files which are not in file list. */
	if (!check)
	{
		parray	   *files_now;

		parray_walk(files, pgFileFree);
		parray_free(files);

		/* re-read file list to change base path to $PGDATA */
		files = dir_read_file_list(pgdata, list_path);
		parray_qsort(files, pgFileComparePathDesc);

		/* get list of files restored to pgdata */
		files_now = parray_new();
		dir_list_file(files_now, pgdata, true, true, false);
		/* to delete from leaf, sort in reversed order */
		parray_qsort(files_now, pgFileComparePathDesc);

		for (i = 0; i < parray_num(files_now); i++)
		{
			pgFile	   *file = (pgFile *) parray_get(files_now, i);

			/* If the file is not in the file list, delete it */
			if (parray_bsearch(files, file, pgFileComparePathDesc) == NULL)
			{
				elog(LOG, "deleted %s", file->path + strlen(pgdata) + 1);
				pgFileDelete(file);
			}
		}

		parray_walk(files_now, pgFileFree);
		parray_free(files_now);
	}

	/* cleanup */
	parray_walk(files, pgFileFree);
	parray_free(files);

	if (!check)
		elog(LOG, "restore backup completed");
}

/*
 * Restore backup directories from **backup_database_dir** to **pg_data_dir**.
 *
 * TODO: Think about simplification and clarity of the function.
 */
static void
restore_directories(const char *pg_data_dir, const char *backup_dir)
{
	parray	   *dirs,
			   *links;
	size_t		i,
				db_path_len;
	char		backup_database_dir[MAXPGPATH],
				to_path[MAXPGPATH];

	dirs = parray_new();
	links = parray_new();

	join_path_components(backup_database_dir, backup_dir, DATABASE_DIR);
	db_path_len = strlen(backup_database_dir);

	list_data_directories(dirs, backup_database_dir, true, false);
	read_tablespace_map(links, backup_dir);

	elog(LOG, "restore directories and symlinks...");

	for (i = 0; i < parray_num(dirs); i++)
	{
		pgFile	   *dir = (pgFile *) parray_get(dirs, i);
		char	   *relative_ptr = dir->path + db_path_len + 1;

		Assert(S_ISDIR(dir->mode));

		/* First try to create symlink and linked directory */
		if (path_is_prefix_of_path(PG_TBLSPC_DIR, relative_ptr))
		{
			char	   *link_ptr = relative_ptr + strlen(PG_TBLSPC_DIR) + 1,
					   *link_sep,
					   *tmp_ptr;
			char		link_name[MAXPGPATH];
			pgFile	  **link;

			/* Extract link name from relative path */
			link_sep = first_dir_separator(link_ptr);
			if (link_sep)
			{
				int			len = link_sep - link_ptr;
				strncpy(link_name, link_ptr, len);
				link_name[len] = '\0';
			}
			else
				strcpy(link_name, link_ptr);

			tmp_ptr = dir->path;
			dir->path = link_name;
			/* Search only by symlink name without path */
			link = (pgFile **) parray_bsearch(links, dir, pgFileComparePath);
			dir->path = tmp_ptr;

			if (link)
			{
				const char *linked_path = get_tablespace_mapping((*link)->linked);
				const char *dir_created;

				if (!is_absolute_path(linked_path))
					elog(ERROR, "tablespace directory is not an absolute path: %s\n",
						 linked_path);

				/* Check if linked directory was created earlier */
				dir_created = get_tablespace_created(link_name);
				if (dir_created)
				{
					/*
					 * If symlink and linked directory were created do not
					 * create it second time.
					 */
					if (strcmp(dir_created, linked_path) == 0)
						continue;
					else
						elog(ERROR, "tablespace directory \"%s\" of page backup does not "
							 "match with previous created tablespace directory \"%s\" of symlink \"%s\"",
							 linked_path, dir_created, link_name);
				}

				/* Check if restore destination empty */
				if (!dir_is_empty(linked_path))
					elog(ERROR, "restore destination is not empty \"%s\"",
						 linked_path);

				if (link_sep)
					elog(LOG, "create directory \"%s\" and symbolic link \"%.*s\"",
						 linked_path,
						 (int) (link_sep -  relative_ptr), relative_ptr);
				else
					elog(LOG, "create directory \"%s\" and symbolic link \"%s\"",
						 linked_path, relative_ptr);

				/* Firstly, create linked directory */
				dir_create_dir(linked_path, DIR_PERMISSION);
				/* Create rest of directories */
				if (link_sep && (link_sep + 1))
				{
					join_path_components(to_path, linked_path, link_sep + 1);
					dir_create_dir(to_path, DIR_PERMISSION);
				}

				join_path_components(to_path, pg_data_dir, PG_TBLSPC_DIR);
				/* Create pg_tblspc directory just in case */
				dir_create_dir(to_path, DIR_PERMISSION);

				/* Secondly, create link */
				join_path_components(to_path, to_path, link_name);
				if (symlink(linked_path, to_path) < 0)
					elog(ERROR, "could not create symbolic link \"%s\": %s",
						 to_path, strerror(errno));

				/* Save linked directory */
				set_tablespace_created(link_name, linked_path);

				continue;
			}
		}

		elog(LOG, "create directory \"%s\"", relative_ptr);

		/* This is not symlink, create directory */
		join_path_components(to_path, pg_data_dir, relative_ptr);
		dir_create_dir(to_path, DIR_PERMISSION);
	}

	parray_walk(links, pgBackupFree);
	parray_free(links);

	parray_walk(dirs, pgBackupFree);
	parray_free(dirs);
}

/*
 * Restore files into $PGDATA.
 */
static void
restore_files(void *arg)
{
	int			i;

	restore_files_args *arguments = (restore_files_args *)arg;

	for (i = 0; i < parray_num(arguments->files); i++)
	{
		char		from_root[MAXPGPATH];
		char	   *rel_path;
		pgFile	   *file = (pgFile *) parray_get(arguments->files, i);

		if (__sync_lock_test_and_set(&file->lock, 1) != 0)
			continue;

		pgBackupGetPath(arguments->backup, from_root, lengthof(from_root), DATABASE_DIR);

		/* check for interrupt */
		if (interrupted)
			elog(ERROR, "interrupted during restore database");

		rel_path = file->path + strlen(from_root) + 1;

		/* print progress */
		if (!check)
			elog(LOG, "(%d/%lu) %s ", i + 1, (unsigned long) parray_num(arguments->files),
				 rel_path);

		/* Directories are created before */
		if (S_ISDIR(file->mode))
		{
			if (!check)
				elog(LOG, "directory, skip");
			continue;
		}

		/* not backed up */
		if (file->write_size == BYTES_INVALID)
		{
			if (!check)
				elog(LOG, "not backed up, skip");
			continue;
		}

		/* Do not restore tablespace_map file */
		if (path_is_prefix_of_path("tablespace_map", rel_path))
		{
			if (!check)
				elog(LOG, "skip tablespace_map");
			continue;
		}

		/* restore file */
		if (!check)
			restore_data_file(from_root, pgdata, file, arguments->backup);

		/* print size of restored file */
		if (!check)
			elog(LOG, "restored %lu\n", (unsigned long) file->write_size);
	}
}

static void
create_recovery_conf(time_t backup_id,
					 const char *target_time,
					 const char *target_xid,
					 const char *target_inclusive,
					 TimeLineID target_tli)
{
	char path[MAXPGPATH];
	FILE *fp;

	if (!check)
	{
		elog(LOG, "----------------------------------------");
		elog(LOG, "creating recovery.conf");
	}

	if (!check)
	{
		snprintf(path, lengthof(path), "%s/recovery.conf", pgdata);
		fp = fopen(path, "wt");
		if (fp == NULL)
			elog(ERROR, "cannot open recovery.conf \"%s\": %s", path,
				strerror(errno));

		fprintf(fp, "# recovery.conf generated by pg_probackup %s\n",
			PROGRAM_VERSION);
		fprintf(fp, "restore_command = 'cp %s/%%f %%p'\n", arclog_path);

		if (target_time)
			fprintf(fp, "recovery_target_time = '%s'\n", target_time);
		else if (target_xid)
			fprintf(fp, "recovery_target_xid = '%s'\n", target_xid);
		else if (backup_id != 0)
		{
			fprintf(fp, "recovery_target = 'immediate'\n");
			fprintf(fp, "recovery_target_action = 'promote'\n");
		}

		if (target_inclusive)
			fprintf(fp, "recovery_target_inclusive = '%s'\n", target_inclusive);

		fprintf(fp, "recovery_target_timeline = '%u'\n", target_tli);

		fclose(fp);
	}
}

/*
 * Try to read a timeline's history file.
 *
 * If successful, return the list of component pgTimeLine (the ancestor
 * timelines followed by target timeline).	If we cannot find the history file,
 * assume that the timeline has no parents, and return a list of just the
 * specified timeline ID.
 * based on readTimeLineHistory() in xlog.c
 */
parray *
readTimeLineHistory(TimeLineID targetTLI)
{
	parray	   *result;
	char		path[MAXPGPATH];
	char		fline[MAXPGPATH];
	FILE	   *fd;
	pgTimeLine *timeline;
	pgTimeLine *last_timeline = NULL;

	result = parray_new();

	/* search from arclog_path first */
	snprintf(path, lengthof(path), "%s/%08X.history", arclog_path,
		targetTLI);

	fd = fopen(path, "rt");
	if (fd == NULL)
	{
		if (errno != ENOENT)
			elog(ERROR, "could not open file \"%s\": %s", path,
				strerror(errno));
	}

	/*
	 * Parse the file...
	 */
	while (fd && fgets(fline, sizeof(fline), fd) != NULL)
	{
		char	   *ptr;
		TimeLineID	tli;
		uint32		switchpoint_hi;
		uint32		switchpoint_lo;
		int			nfields;

		for (ptr = fline; *ptr; ptr++)
		{
			if (!isspace((unsigned char) *ptr))
				break;
		}
		if (*ptr == '\0' || *ptr == '#')
			continue;

		/* Parse one entry... */
		nfields = sscanf(fline, "%u\t%X/%X", &tli, &switchpoint_hi, &switchpoint_lo);

		timeline = pgut_new(pgTimeLine);
		timeline->tli = 0;
		timeline->end = 0;

		/* expect a numeric timeline ID as first field of line */
		timeline->tli = tli;

		if (nfields < 1)
		{
			/* expect a numeric timeline ID as first field of line */
			elog(ERROR,
				 "syntax error in history file: %s. Expected a numeric timeline ID.",
				   fline);
		}
		if (nfields != 3)
			elog(ERROR,
				 "syntax error in history file: %s. Expected a transaction log switchpoint location.",
				   fline);

		if (last_timeline && timeline->tli <= last_timeline->tli)
			elog(ERROR,
				   "Timeline IDs must be in increasing sequence.");

		/* Build list with newest item first */
		parray_insert(result, 0, timeline);
		last_timeline = timeline;

		/* Calculate the end lsn finally */
		timeline->end = (XLogRecPtr)
			((uint64) switchpoint_hi << 32) | switchpoint_lo;
	}

	if (fd)
		fclose(fd);

	if (last_timeline && targetTLI <= last_timeline->tli)
		elog(ERROR,
			"Timeline IDs must be less than child timeline's ID.");

	/* append target timeline */
	timeline = pgut_new(pgTimeLine);
	timeline->tli = targetTLI;
	/* lsn in target timeline is valid */
	timeline->end = (uint32) (-1UL << 32) | -1UL;
	parray_insert(result, 0, timeline);

	/* dump timeline branches in verbose mode */
	if (verbose)
	{
		int i;

		for (i = 0; i < parray_num(result); i++)
		{
			pgTimeLine *timeline = parray_get(result, i);
			elog(LOG, "%s() result[%d]: %08X/%08X/%08X", __FUNCTION__, i,
				timeline->tli,
				 (uint32) (timeline->end >> 32),
				 (uint32) timeline->end);
		}
	}

	return result;
}

bool
satisfy_recovery_target(const pgBackup *backup, const pgRecoveryTarget *rt)
{
	if (rt->xid_specified)
		return backup->recovery_xid <= rt->recovery_target_xid;

	if (rt->time_specified)
		return backup->recovery_time <= rt->recovery_target_time;

	return true;
}

bool
satisfy_timeline(const parray *timelines, const pgBackup *backup)
{
	int i;
	for (i = 0; i < parray_num(timelines); i++)
	{
		pgTimeLine *timeline = (pgTimeLine *) parray_get(timelines, i);
		if (backup->tli == timeline->tli &&
			backup->stop_lsn < timeline->end)
			return true;
	}
	return false;
}

/* get TLI of the latest full backup */
TimeLineID
get_fullbackup_timeline(parray *backups, const pgRecoveryTarget *rt)
{
	int			i;
	pgBackup   *base_backup = NULL;
	TimeLineID	ret;

	for (i = 0; i < parray_num(backups); i++)
	{
		base_backup = (pgBackup *) parray_get(backups, i);

		if (base_backup->backup_mode >= BACKUP_MODE_FULL)
		{
			/*
			 * Validate backup files with its size, because load of CRC
			 * calculation is not right.
			 */
			if (base_backup->status == BACKUP_STATUS_DONE)
				pgBackupValidate(base_backup, true, true);

			if (!satisfy_recovery_target(base_backup, rt))
				continue;

			if (base_backup->status == BACKUP_STATUS_OK)
				break;
		}
	}
	/* no full backup found, cannot restore */
	if (i == parray_num(backups))
		elog(ERROR, "no full backup found, cannot restore.");

	ret = base_backup->tli;

	return ret;
}

static void
print_backup_lsn(const pgBackup *backup)
{
	char timestamp[100];

	if (!verbose)
		return;

	time2iso(timestamp, lengthof(timestamp), backup->start_time);
	elog(LOG, "  %s (%X/%08X)",
		 timestamp,
		 (uint32) (backup->stop_lsn >> 32),
		 (uint32) backup->stop_lsn);
}

pgRecoveryTarget *
checkIfCreateRecoveryConf(const char *target_time,
                   const char *target_xid,
                   const char *target_inclusive)
{
	time_t			dummy_time;
	TransactionId	dummy_xid;
	bool			dummy_bool;
	pgRecoveryTarget *rt;

	/* Initialize pgRecoveryTarget */
	rt = pgut_new(pgRecoveryTarget);
	rt->time_specified = false;
	rt->xid_specified = false;
	rt->recovery_target_time = 0;
	rt->recovery_target_xid  = 0;
	rt->recovery_target_inclusive = false;

	if (target_time)
	{
		rt->time_specified = true;
		if (parse_time(target_time, &dummy_time))
			rt->recovery_target_time = dummy_time;
		else
			elog(ERROR, "cannot create recovery.conf with %s", target_time);
	}
	if (target_xid)
	{
		rt->xid_specified = true;
#ifdef PGPRO_EE
		if (parse_uint64(target_xid, &dummy_xid))
#else
		if (parse_uint32(target_xid, &dummy_xid))
#endif
			rt->recovery_target_xid = dummy_xid;
		else
			elog(ERROR, "cannot create recovery.conf with %s", target_xid);
	}
	if (target_inclusive)
	{
		if (parse_bool(target_inclusive, &dummy_bool))
			rt->recovery_target_inclusive = dummy_bool;
		else
			elog(ERROR, "cannot create recovery.conf with %s", target_inclusive);
	}

	return rt;

}


/*
 * Probe whether a timeline history file exists for the given timeline ID
 */
static bool
existsTimeLineHistory(TimeLineID probeTLI)
{
	char		path[MAXPGPATH];
	FILE		*fd;

	/* Timeline 1 does not have a history file, so no need to check */
	if (probeTLI == 1)
		return false;

	snprintf(path, lengthof(path), "%s/%08X.history", arclog_path, probeTLI);
	fd = fopen(path, "r");
	if (fd != NULL)
	{
		fclose(fd);
		return true;
	}
	else
	{
		if (errno != ENOENT)
			elog(ERROR, "Failed directory for path: %s", path);
		return false;
	}
}

/*
 * Find the newest existing timeline, assuming that startTLI exists.
 *
 * Note: while this is somewhat heuristic, it does positively guarantee
 * that (result + 1) is not a known timeline, and therefore it should
 * be safe to assign that ID to a new timeline.
 */
TimeLineID
findNewestTimeLine(TimeLineID startTLI)
{
	TimeLineID	newestTLI;
	TimeLineID	probeTLI;

	/*
	 * The algorithm is just to probe for the existence of timeline history
	 * files.  XXX is it useful to allow gaps in the sequence?
	 */
	newestTLI = startTLI;

	for (probeTLI = startTLI + 1;; probeTLI++)
	{
		if (existsTimeLineHistory(probeTLI))
		{
			newestTLI = probeTLI;		/* probeTLI exists */
		}
		else
		{
			/* doesn't exist, assume we're done */
			break;
		}
	}

	return newestTLI;
}

/*
 * Split argument into old_dir and new_dir and append to tablespace mapping
 * list.
 *
 * Copy of function tablespace_list_append() from pg_basebackup.c.
 */
void
opt_tablespace_map(pgut_option *opt, const char *arg)
{
	TablespaceListCell *cell = pgut_new(TablespaceListCell);
	char	   *dst;
	char	   *dst_ptr;
	const char *arg_ptr;

	dst_ptr = dst = cell->old_dir;
	for (arg_ptr = arg; *arg_ptr; arg_ptr++)
	{
		if (dst_ptr - dst >= MAXPGPATH)
			elog(ERROR, "directory name too long");

		if (*arg_ptr == '\\' && *(arg_ptr + 1) == '=')
			;					/* skip backslash escaping = */
		else if (*arg_ptr == '=' && (arg_ptr == arg || *(arg_ptr - 1) != '\\'))
		{
			if (*cell->new_dir)
				elog(ERROR, "multiple \"=\" signs in tablespace mapping\n");
			else
				dst = dst_ptr = cell->new_dir;
		}
		else
			*dst_ptr++ = *arg_ptr;
	}

	if (!*cell->old_dir || !*cell->new_dir)
		elog(ERROR, "invalid tablespace mapping format \"%s\", "
			 "must be \"OLDDIR=NEWDIR\"", arg);

	/*
	 * This check isn't absolutely necessary.  But all tablespaces are created
	 * with absolute directories, so specifying a non-absolute path here would
	 * just never match, possibly confusing users.  It's also good to be
	 * consistent with the new_dir check.
	 */
	if (!is_absolute_path(cell->old_dir))
		elog(ERROR, "old directory is not an absolute path in tablespace mapping: %s\n",
			 cell->old_dir);

	if (!is_absolute_path(cell->new_dir))
		elog(ERROR, "new directory is not an absolute path in tablespace mapping: %s\n",
			 cell->new_dir);

	if (tablespace_dirs.tail)
		tablespace_dirs.tail->next = cell;
	else
		tablespace_dirs.head = cell;
	tablespace_dirs.tail = cell;
}

/*
 * Retrieve tablespace path, either relocated or original depending on whether
 * -T was passed or not.
 *
 * Copy of function get_tablespace_mapping() from pg_basebackup.c.
 */
static const char *
get_tablespace_mapping(const char *dir)
{
	TablespaceListCell *cell;

	for (cell = tablespace_dirs.head; cell; cell = cell->next)
		if (strcmp(dir, cell->old_dir) == 0)
			return cell->new_dir;

	return dir;
}

/*
 * Save create directory path into memory. We can use it in next page restore to
 * not raise the error "restore destination is not empty" in
 * restore_directories().
 */
static void
set_tablespace_created(const char *link, const char *dir)
{
	TablespaceCreatedListCell *cell = pgut_new(TablespaceCreatedListCell);

	strcpy(cell->link_name, link);
	strcpy(cell->linked_dir, dir);

	if (tablespace_created_dirs.tail)
		tablespace_created_dirs.tail->next = cell;
	else
		tablespace_created_dirs.head = cell;
	tablespace_created_dirs.tail = cell;
}

/*
 * Is directory was created when symlink was created in restore_directories().
 */
static const char *
get_tablespace_created(const char *link)
{
	TablespaceCreatedListCell *cell;

	for (cell = tablespace_created_dirs.head; cell; cell = cell->next)
		if (strcmp(link, cell->link_name) == 0)
			return cell->linked_dir;

	return NULL;
}
