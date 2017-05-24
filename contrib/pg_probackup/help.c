/*-------------------------------------------------------------------------
 *
 * help.c
 *
 * Portions Copyright (c) 2017-2017, Postgres Professional
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

	printf(_("\n  %s init -B backup-path -D pgdata-dir\n"), PROGRAM_NAME);

	printf(_("\n  %s set-config -B backup-dir\n"), PROGRAM_NAME);
	printf(_("                 [-d dbname] [-h host] [-p port] [-U username]\n"));
	printf(_("                 [--retention-redundancy=retention-redundancy]]\n"));
	printf(_("                 [--retention-window=retention-window]\n"));

	printf(_("\n  %s show-config -B backup-dir\n"), PROGRAM_NAME);

	printf(_("\n  %s backup -B backup-path -b backup-mode\n"), PROGRAM_NAME);
	printf(_("                 [-D pgdata-dir] [-C] [--stream [-S slot-name]] [--backup-pg-log]\n"));
	printf(_("                 [-j num-threads] [--archive-timeout=archive-timeout]\n"));
	printf(_("                 [--progress] [-q] [-v] [--delete-expired]\n"));
	printf(_("                 [-d dbname] [-h host] [-p port] [-U username]\n"));

	printf(_("\n  %s restore -B backup-dir\n"), PROGRAM_NAME);
	printf(_("                [-D pgdata-dir] [-i backup-id] [--progress] [-q] [-v]\n"));
	printf(_("                [--time=time|--xid=xid [--inclusive=boolean]]\n"));
	printf(_("                [--timeline=timeline] [-T OLDDIR=NEWDIR]\n"));

	printf(_("\n  %s validate -B backup-dir\n"), PROGRAM_NAME);
	printf(_("                [-D pgdata-dir] [-i backup-id] [--progress] [-q] [-v]\n"));
	printf(_("                [--time=time|--xid=xid [--inclusive=boolean]]\n"));
	printf(_("                [--timeline=timeline]\n"));

	printf(_("\n  %s show -B backup-dir\n"), PROGRAM_NAME);
	printf(_("                [-i backup-id]\n"));

	printf(_("\n  %s delete -B backup-dir\n"), PROGRAM_NAME);
	printf(_("                [--wal] [-i backup-id | --expired]\n"));

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
	printf(_("%s init -B backup-path -D pgdata-dir\n\n"), PROGRAM_NAME);
	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("  -D, --pgdata=pgdata-dir          location of the database storage area\n"));
}

static void
help_backup(void)
{
	printf(_("%s backup -B backup-path -b backup-mode\n"), PROGRAM_NAME);
	printf(_("                 [-D pgdata-dir] [-C] [--stream [-S slot-name]] [--backup-pg-log]\n"));
	printf(_("                 [-j num-threads] [--archive-timeout=archive-timeout]\n"));
	printf(_("                 [--progress] [-q] [-v] [--delete-expired]\n"));
	printf(_("                 [-d dbname] [-h host] [-p port] [-U username]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("  -b, --backup-mode=backup-mode    backup mode=FULL|PAGE|PTRACK\n"));
	printf(_("  -D, --pgdata=pgdata-dir          location of the database storage area\n"));
	printf(_("  -C, --smooth-checkpoint          do smooth checkpoint before backup\n"));
	printf(_("      --stream                     stream the transaction log and include it in the backup\n"));
	printf(_("      --archive-timeout            wait timeout for WAL segment archiving\n"));
	printf(_("  -S, --slot=SLOTNAME              replication slot to use\n"));
	printf(_("      --backup-pg-log              backup of pg_log directory\n"));
	printf(_("  -j, --threads=NUM                number of parallel threads\n"));
	printf(_("      --progress                   show progress\n"));
	printf(_("  -q, --quiet                      don't write any messages\n"));
	printf(_("  -v, --verbose                    verbose mode\n"));
	printf(_("      --delete-expired             delete backups expired according to current\n"));
	printf(_("                                   retention policy after successful backup completion\n"));

	printf(_("\n  Connection options:\n"));
	printf(_("  -d, --dbname=DBNAME              database to connect\n"));
	printf(_("  -h, --host=HOSTNAME              database server host or socket directory\n"));
	printf(_("  -p, --port=PORT                  database server port\n"));
	printf(_("  -U, --username=USERNAME          user name to connect as\n"));
}

static void
help_restore(void)
{
	printf(_("%s restore -B backup-dir\n"), PROGRAM_NAME);
	printf(_("                [-D pgdata-dir] [-i backup-id] [--progress] [-q] [-v]\n"));
	printf(_("                [--time=time|--xid=xid [--inclusive=boolean]]\n"));
	printf(_("                [--timeline=timeline] [-T OLDDIR=NEWDIR]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("  -D, --pgdata=pgdata-dir          location of the database storage area\n"));
	printf(_("  -i, --backup-id=backup-id        backup to restore\n"));

	printf(_("      --progress                   show progress\n"));
	printf(_("  -q, --quiet                      don't write any messages\n"));
	printf(_("  -v, --verbose                    verbose mode\n"));
	printf(_("      --time=time                  time stamp up to which recovery will proceed\n"));
	printf(_("      --xid=xid                    transaction ID up to which recovery will proceed\n"));
	printf(_("      --inclusive=boolean          whether we stop just after the recovery target\n"));
	printf(_("      --timeline=timeline          recovering into a particular timeline\n"));
	printf(_("  -T, --tablespace-mapping=OLDDIR=NEWDIR\n"));
	printf(_("           						 relocate the tablespace from directory OLDDIR to NEWDIR\n"));
}

static void
help_validate(void)
{
	printf(_("%s validate -B backup-dir\n"), PROGRAM_NAME);
	printf(_("                [-D pgdata-dir] [-i backup-id] [--progress] [-q] [-v]\n"));
	printf(_("                [--time=time|--xid=xid [--inclusive=boolean]]\n"));
	printf(_("                [--timeline=timeline]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("  -D, --pgdata=pgdata-dir          location of the database storage area\n"));
	printf(_("  -i, --backup-id=backup-id        backup to validate\n"));

	printf(_("      --progress                   show progress\n"));
	printf(_("  -q, --quiet                      don't write any messages\n"));
	printf(_("  -v, --verbose                    verbose mode\n"));
	printf(_("      --time=time                  time stamp up to which recovery will proceed\n"));
	printf(_("      --xid=xid                    transaction ID up to which recovery will proceed\n"));
	printf(_("      --inclusive=boolean          whether we stop just after the recovery target\n"));
	printf(_("      --timeline=timeline          recovering into a particular timeline\n"));
}

static void
help_show(void)
{
	printf(_("%s show -B backup-dir\n"), PROGRAM_NAME);
	printf(_("                [-i backup-id]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("  -i, --backup-id=backup-id        show info about specific backups\n"));
}

static void
help_delete(void)
{
	printf(_("%s delete -B backup-dir\n"), PROGRAM_NAME);
	printf(_("                [--wal] [-i backup-id | --expired]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
	printf(_("      --wal                        remove unnecessary wal files\n"));
	printf(_("  -i, --backup-id=backup-id        backup to delete\n"));
	printf(_("      --expired                    delete backups expired according to current\n"));
	printf(_("                                   retention policy\n"));
}

static void
help_set_config(void)
{
	printf(_("%s set-config -B backup-dir\n"), PROGRAM_NAME);
	printf(_("                 [-d dbname] [-h host] [-p port] [-U username]\n"));
	printf(_("                 [--retention-redundancy=retention-redundancy]]\n"));
	printf(_("                 [--retention-window=retention-window]\n\n"));

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));

	printf(_("\n  Connection options:\n"));
	printf(_("  -d, --dbname=DBNAME              database to connect\n"));
	printf(_("  -h, --host=HOSTNAME              database server host or socket directory\n"));
	printf(_("  -p, --port=PORT                  database server port\n"));
	printf(_("  -U, --username=USERNAME          user name to connect as\n"));

	printf(_("\n  Retention options:\n"));
	printf(_("      --retention-redundancy=retention-redundancy\n"));
	printf(_("                                   number of full backups to keep\n"));
	printf(_("      --retention-window=retention-window\n"));
	printf(_("                                   number of days of recoverability\n"));

}

static void
help_show_config(void)
{
	printf(_("%s show-config -B backup-dir\n\n"), PROGRAM_NAME);

	printf(_("  -B, --backup-path=backup-path    location of the backup storage area\n"));
}
