/*-------------------------------------------------------------------------
 *
 * help.c
 *
 * Copyright (c) 2017-2017, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */
#include "pg_probackup.h"

static void help_init(void);
static void help_backup(void);
static void help_restore(void);
static void help_validate(void);
static void help_show(void);
static void help_delete(void);
static void help_set_config(void);
static void help_show_config(void);
static void help_add_instance(void);
static void help_del_instance(void);

void
help_command(char *command)
{
	if (strcmp(command, "init") == 0)
		help_init();
	else if (strcmp(command, "backup") == 0)
		help_backup();
	else if (strcmp(command, "restore") == 0)
		help_restore();
	else if (strcmp(command, "validate") == 0)
		help_validate();
	else if (strcmp(command, "show") == 0)
		help_show();
	else if (strcmp(command, "delete") == 0)
		help_delete();
	else if (strcmp(command, "set-config") == 0)
		help_set_config();
	else if (strcmp(command, "show-config") == 0)
		help_show_config();
	else if (strcmp(command, "add-instance") == 0)
		help_add_instance();
	else if (strcmp(command, "del-instance") == 0)
		help_del_instance();
	else if (strcmp(command, "--help") == 0
			 || strcmp(command, "help") == 0
			 || strcmp(command, "-?") == 0
			 || strcmp(command, "--version") == 0
			 || strcmp(command, "version") == 0
			 || strcmp(command, "-V") == 0)
		printf(_("No help page for \"%s\" command. Try pg_probackup help\n"), command);
	else
		printf(_("Unknown command. Try pg_probackup help\n"));
	exit(0);
}

void
help_pg_probackup(void)
{
	printf(_("\n%s - utility to manage backup/recovery of PostgreSQL database.\n\n"), PROGRAM_NAME);

	printf(_("  %s help [COMMAND]\n"), PROGRAM_NAME);

	printf(_("\n  %s version\n"), PROGRAM_NAME);

	printf(_("\n  %s init -B backup-path [-l]\n"), PROGRAM_NAME);

	printf(_("\n  %s set-config -B backup-dir --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 [--log-level=log-level]\n"));
	printf(_("                 [--log-filename=log-filename]\n"));
	printf(_("                 [--error-log-filename=error-log-filename]\n"));
	printf(_("                 [--log-directory=log-directory]\n"));
	printf(_("                 [--log-rotation-size=log-rotation-size]\n"));
	printf(_("                 [--log-rotation-age=log-rotation-age]\n"));
	printf(_("                 [--retention-redundancy=retention-redundancy]\n"));
	printf(_("                 [--retention-window=retention-window]\n"));
	printf(_("                 [--compress-algorithm=compress-algorithm]\n"));
	printf(_("                 [--compress-level=compress-level]\n"));
	printf(_("                 [-d dbname] [-h host] [-p port] [-U username]\n"));
	printf(_("                 [--master-db=db_name] [--master-host=host_name]\n"));
	printf(_("                 [--master-port=port] [--master-user=user_name]\n"));
	printf(_("                 [--replica-timeout=timeout]\n"));

	printf(_("\n  %s show-config -B backup-dir --instance=instance_name\n"), PROGRAM_NAME);

	printf(_("\n  %s backup -B backup-path -b backup-mode --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 [-C] [-l] [--stream [-S slot-name]] [--backup-pg-log]\n"));
	printf(_("                 [-j num-threads] [--archive-timeout=archive-timeout]\n"));
	printf(_("                 [--compress-algorithm=compress-algorithm]\n"));
	printf(_("                 [--compress-level=compress-level]\n"));
	printf(_("                 [--progress] [--delete-expired]\n"));
	printf(_("                 [-d dbname] [-h host] [-p port] [-U username]\n"));
	printf(_("                 [--master-db=db_name] [--master-host=host_name]\n"));
	printf(_("                 [--master-port=port] [--master-user=user_name]\n"));
	printf(_("                 [--replica-timeout=timeout]\n"));

	printf(_("\n  %s restore -B backup-dir --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 [-D pgdata-dir] [-l] [-i backup-id] [--progress]\n"));
	printf(_("                 [--time=time|--xid=xid [--inclusive=boolean]]\n"));
	printf(_("                 [--timeline=timeline] [-T OLDDIR=NEWDIR]\n"));

	printf(_("\n  %s validate -B backup-dir [--instance=instance_name]\n"), PROGRAM_NAME);
	printf(_("                 [-i backup-id] [-l] [--progress]\n"));
	printf(_("                 [--time=time|--xid=xid [--inclusive=boolean]]\n"));
	printf(_("                 [--timeline=timeline]\n"));

	printf(_("\n  %s show -B backup-dir\n"), PROGRAM_NAME);
	printf(_("                 [--instance=instance_name [-i backup-id]]\n"));

	printf(_("\n  %s delete -B backup-dir --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 [--wal] [-i backup-id | --expired] [-l]\n"));

	printf(_("\n  %s add-instance -B backup-dir -D pgdata-dir\n"), PROGRAM_NAME);
	printf(_("                 --instance=instance_name\n"));

	printf(_("\n  %s del-instance -B backup-dir\n"), PROGRAM_NAME);
	printf(_("                 --instance=instance_name\n"));

	if ((PROGRAM_URL || PROGRAM_EMAIL))
	{
		printf("\n");
		if (PROGRAM_URL)
			printf("Read the website for details. <%s>\n", PROGRAM_URL);
		if (PROGRAM_EMAIL)
			printf("Report bugs to <%s>.\n", PROGRAM_EMAIL);
	}
	exit(0);
}

static void
help_init(void)
{
	printf(_("%s init -B backup-path -D pgdata-dir [-l]\n\n"), PROGRAM_NAME);
	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("  -l, --log                        store messages in a log file\n"));
}

static void
help_backup(void)
{
	printf(_("%s backup -B backup-path -b backup-mode --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 [-C] [-l] [--stream [-S slot-name]] [--backup-pg-log]\n"));
	printf(_("                 [-j num-threads] [--archive-timeout=archive-timeout]\n"));
	printf(_("                 [--progress] [--delete-expired]\n"));
	printf(_("                 [--compress-algorithm=compress-algorithm]\n"));
	printf(_("                 [--compress-level=compress-level]\n"));
	printf(_("                 [-d dbname] [-h host] [-p port] [-U username]\n"));
	printf(_("                 [--master-db=db_name] [--master-host=host_name]\n"));
	printf(_("                 [--master-port=port] [--master-user=user_name]\n"));
	printf(_("                 [--replica-timeout=timeout]\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("  -b, --backup-mode=backup-mode    backup mode=FULL|PAGE|PTRACK\n"));
	printf(_("      --instance=instance_name     name of the instance\n"));
	printf(_("  -C, --smooth-checkpoint          do smooth checkpoint before backup\n"));
	printf(_("  -l, --log                        store messages in a log file\n"));
	printf(_("      --stream                     stream the transaction log and include it in the backup\n"));
	printf(_("      --archive-timeout            wait timeout for WAL segment archiving\n"));
	printf(_("  -S, --slot=SLOTNAME              replication slot to use\n"));
	printf(_("      --backup-pg-log              backup of pg_log directory\n"));
	printf(_("  -j, --threads=NUM                number of parallel threads\n"));
	printf(_("      --progress                   show progress\n"));
	printf(_("      --delete-expired             delete backups expired according to current\n"));
	printf(_("                                   retention policy after successful backup completion\n"));

	printf(_("\n  Compression options:\n"));
	printf(_("      --compress-algorithm=compress-algorithm\n"));
	printf(_("                                   available options: 'zlib','pglz','none'\n"));
	printf(_("      --compress-level=compress-level\n"));
	printf(_("                                   level of compression [0-9]\n"));

	printf(_("\n  Connection options:\n"));
	printf(_("  -d, --dbname=DBNAME              database to connect\n"));
	printf(_("  -h, --host=HOSTNAME              database server host or socket directory\n"));
	printf(_("  -p, --port=PORT                  database server port\n"));
	printf(_("  -U, --username=USERNAME          user name to connect as\n"));

	printf(_("\n  Replica options:\n"));
	printf(_("      --master-db=db_name          database to connect to master\n"));
	printf(_("      --master-host=host_name      database server host of master\n"));
	printf(_("      --master-port=port           database server port of master\n"));
	printf(_("      --master-user=user_name      user name to connect to master\n"));
	printf(_("      --replica-timeout=timeout    wait timeout for WAL segment streaming through replication in seconds\n"));
}

static void
help_restore(void)
{
	printf(_("%s restore -B backup-dir --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 [-D pgdata-dir] [-i backup-id] [--progress]\n"));
	printf(_("                 [--time=time|--xid=xid [--inclusive=boolean]]\n"));
	printf(_("                 [--timeline=timeline] [-T OLDDIR=NEWDIR]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --instance=instance_name     name of the instance\n"));

	printf(_("  -D, --pgdata=pgdata-dir          location of the database storage area\n"));
	printf(_("  -l, --log                        store messages in a log file\n"));
	printf(_("  -i, --backup-id=backup-id        backup to restore\n"));

	printf(_("      --progress                   show progress\n"));
	printf(_("      --time=time                  time stamp up to which recovery will proceed\n"));
	printf(_("      --xid=xid                    transaction ID up to which recovery will proceed\n"));
	printf(_("      --inclusive=boolean          whether we stop just after the recovery target\n"));
	printf(_("      --timeline=timeline          recovering into a particular timeline\n"));
	printf(_("  -T, --tablespace-mapping=OLDDIR=NEWDIR\n"));
	printf(_("                                   relocate the tablespace from directory OLDDIR to NEWDIR\n"));
}

static void
help_validate(void)
{
	printf(_("%s validate -B backup-dir [--instance=instance_name]\n"), PROGRAM_NAME);
	printf(_("                 [-i backup-id] [--progress]\n"));
	printf(_("                 [--time=time|--xid=xid [--inclusive=boolean]]\n"));
	printf(_("                 [--timeline=timeline]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --instance=instance_name     name of the instance\n"));
	printf(_("  -i, --backup-id=backup-id        backup to validate\n"));

	printf(_("  -l, --log                        store messages in a log file\n"));
	printf(_("      --progress                   show progress\n"));
	printf(_("      --time=time                  time stamp up to which recovery will proceed\n"));
	printf(_("      --xid=xid                    transaction ID up to which recovery will proceed\n"));
	printf(_("      --inclusive=boolean          whether we stop just after the recovery target\n"));
	printf(_("      --timeline=timeline          recovering into a particular timeline\n"));
}

static void
help_show(void)
{
	printf(_("%s show -B backup-dir\n"), PROGRAM_NAME);
	printf(_("                 [--instance=instance_name [-i backup-id]]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --instance=instance_name     show info about specific intstance\n"));
	printf(_("  -i, --backup-id=backup-id        show info about specific backups\n"));
}

static void
help_delete(void)
{
	printf(_("%s delete -B backup-dir --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 [--wal] [-i backup-id | --expired]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --instance=instance_name     name of the instance\n"));
	printf(_("      --wal                        remove unnecessary wal files\n"));
	printf(_("  -i, --backup-id=backup-id        backup to delete\n"));
	printf(_("      --expired                    delete backups expired according to current\n"));
	printf(_("                                   retention policy\n"));
	printf(_("  -l, --log                        store messages in a log file\n"));
}

static void
help_set_config(void)
{
	printf(_("%s set-config -B backup-dir --instance=instance_name\n"), PROGRAM_NAME);
	printf(_("                 [--log-level=log-level]\n"));
	printf(_("                 [--log-filename=log-filename]\n"));
	printf(_("                 [--error-log-filename=error-log-filename]\n"));
	printf(_("                 [--log-directory=log-directory]\n"));
	printf(_("                 [--log-rotation-size=log-rotation-size]\n"));
	printf(_("                 [--log-rotation-age=log-rotation-age]\n"));
	printf(_("                 [--retention-redundancy=retention-redundancy]\n"));
	printf(_("                 [--retention-window=retention-window]\n"));
	printf(_("                 [--compress-algorithm=compress-algorithm]\n"));
	printf(_("                 [--compress-level=compress-level]\n"));
	printf(_("                 [-d dbname] [-h host] [-p port] [-U username]\n"));
	printf(_("                 [--master-db=db_name] [--master-host=host_name]\n"));
	printf(_("                 [--master-port=port] [--master-user=user_name]\n"));
	printf(_("                 [--replica-timeout=timeout]\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --instance=instance_name     name of the instance\n"));

	printf(_("\n  Logging options:\n"));
	printf(_("      --log-level=log-level        controls which message levels are sent to the log\n"));
	printf(_("      --log-filename=log-filename  file names of the created log files which is treated as as strftime pattern\n"));
	printf(_("      --error-log-filename=error-log-filename\n"));
	printf(_("                                   file names of the created log files for error messages\n"));
	printf(_("      --log-directory=log-directory\n"));
	printf(_("                                   directory in which log files will be created\n"));
	printf(_("      --log-rotation-size=log-rotation-size\n"));
	printf(_("                                   maximum size of an individual log file in kilobytes\n"));
	printf(_("      --log-rotation-age=log-rotation-age\n"));
	printf(_("                                   maximum lifetime of an individual log file in minutes\n"));

	printf(_("\n  Retention options:\n"));
	printf(_("      --retention-redundancy=retention-redundancy\n"));
	printf(_("                                   number of full backups to keep\n"));
	printf(_("      --retention-window=retention-window\n"));
	printf(_("                                   number of days of recoverability\n"));

	printf(_("\n  Compression options:\n"));
	printf(_("      --compress-algorithm=compress-algorithm\n"));
	printf(_("                                   available options: 'zlib','pglz','none'\n"));
	printf(_("      --compress-level=compress-level\n"));
	printf(_("                                   level of compression [0-9]\n"));

	printf(_("\n  Connection options:\n"));
	printf(_("  -d, --dbname=DBNAME              database to connect\n"));
	printf(_("  -h, --host=HOSTNAME              database server host or socket directory\n"));
	printf(_("  -p, --port=PORT                  database server port\n"));
	printf(_("  -U, --username=USERNAME          user name to connect as\n"));

	printf(_("\n  Replica options:\n"));
	printf(_("      --master-db=db_name          database to connect to master\n"));
	printf(_("      --master-host=host_name      database server host of master\n"));
	printf(_("      --master-port=port=port           database server port of master\n"));
	printf(_("      --master-user=user_name      user name to connect to master\n"));
	printf(_("      --replica-timeout=timeout    wait timeout for WAL segment streaming through replication\n"));
}

static void
help_show_config(void)
{
	printf(_("%s show-config -B backup-dir --instance=instance_name\n\n"), PROGRAM_NAME);

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --instance=instance_name     name of the instance\n"));
}

static void
help_add_instance(void)
{
	printf(_("%s add-instance -B backup-dir -D pgdata-dir\n"), PROGRAM_NAME);
	printf(_("                 --instance=instance_name\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("  -D, --pgdata=pgdata-dir          location of the database storage area\n"));
	printf(_("      --instance=instance_name     name of the new instance\n"));
}

static void
help_del_instance(void)
{
	printf(_("%s del-instance -B backup-dir\n"), PROGRAM_NAME);
	printf(_("                 --instance=instance_name\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --instance=instance_name     name of the instance to delete\n"));
}
