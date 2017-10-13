# you need os for unittest to work
import os
from sys import exit, argv, version_info
import subprocess
import shutil
import six
from testgres import get_new_node, clean_all
import hashlib
import re
import pwd


idx_ptrack = {
't_heap': {
    'type': 'heap'
    },
't_btree': {
    'type': 'btree',
    'column': 'text',
    'relation': 't_heap'
    },
't_seq': {
    'type': 'seq',
    'column': 't_seq',
    'relation': 't_heap'
    },
't_spgist': {
    'type': 'spgist',
    'column': 'text',
    'relation': 't_heap'
    },
't_brin': {
    'type': 'brin',
    'column': 'text',
    'relation': 't_heap'
    },
't_gist': {
    'type': 'gist',
    'column': 'tsvector',
    'relation': 't_heap'
    },
't_gin': {
    'type': 'gin',
    'column': 'tsvector',
    'relation': 't_heap'
    },
}

archive_script = """
#!/bin/bash
count=$(ls {backup_dir}/test00* | wc -l)
if [ $count -ge {count_limit} ]
then
    exit 1
else
    cp $1 {backup_dir}/wal/{node_name}/$2
    count=$((count+1))
    touch {backup_dir}/test00$count
    exit 0
fi
"""
warning = """
Wrong splint in show_pb
Original Header:
{header}
Original Body:
{body}
Splitted Header
{header_split}
Splitted Body
{body_split}
"""

# You can lookup error message and cmdline in exception object attributes
class ProbackupException(Exception):
    def __init__(self, message, cmd):
        self.message = message
        self.cmd = cmd
    #need that to make second raise
    def __str__(self):
        return '\n ERROR: {0}\n CMD: {1}'.format(repr(self.message), self.cmd)


def dir_files(base_dir):
    out_list = []
    for dir_name, subdir_list, file_list in os.walk(base_dir):
        if dir_name != base_dir:
            out_list.append(os.path.relpath(dir_name, base_dir))
        for fname in file_list:
            out_list.append(os.path.relpath(os.path.join(dir_name, fname), base_dir))
    out_list.sort()
    return out_list

class ProbackupTest(object):
    def __init__(self, *args, **kwargs):
        super(ProbackupTest, self).__init__(*args, **kwargs)
        if '-v' in argv or '--verbose' in argv:
            self.verbose = True
        else:
            self.verbose = False

        self.test_env = os.environ.copy()
        envs_list = [
            "LANGUAGE",
            "LC_ALL",
            "PGCONNECT_TIMEOUT",
            "PGDATA",
            "PGDATABASE",
            "PGHOSTADDR",
            "PGREQUIRESSL",
            "PGSERVICE",
            "PGSSLMODE",
            "PGUSER",
            "PGPORT",
            "PGHOST"
        ]

        for e in envs_list:
            try:
                del self.test_env[e]
            except:
                pass

        self.test_env["LC_MESSAGES"] = "C"
        self.test_env["LC_TIME"] = "C"
        self.helpers_path = os.path.dirname(os.path.realpath(__file__))
        self.dir_path = os.path.abspath(os.path.join(self.helpers_path, os.pardir))
        self.tmp_path = os.path.abspath(os.path.join(self.dir_path, 'tmp_dirs'))
        try:
            os.makedirs(os.path.join(self.dir_path, 'tmp_dirs'))
        except:
            pass

        self.user = self.get_username()
        self.probackup_path = None
        if "PGPROBACKUPBIN" in self.test_env:
            if os.path.isfile(self.test_env["PGPROBACKUPBIN"]) and os.access(self.test_env["PGPROBACKUPBIN"], os.X_OK):
                self.probackup_path = self.test_env["PGPROBACKUPBIN"]
            else:
                if self.verbose:
                    print('PGPROBINDIR is not an executable file')
        if not self.probackup_path:
            self.probackup_path = os.path.abspath(os.path.join(
                self.dir_path, "../pg_probackup"))

    def make_simple_node(
            self,
            base_dir=None,
            set_replication=False,
            initdb_params=[],
            pg_options={}):

        real_base_dir = os.path.join(self.tmp_path, base_dir)
        shutil.rmtree(real_base_dir, ignore_errors=True)

        node = get_new_node('test', base_dir=real_base_dir)
        node.init(initdb_params=initdb_params)

        # Sane default parameters, not a shit with fsync = off from testgres
        node.append_conf("postgresql.auto.conf", "shared_buffers = 10MB")
        node.append_conf("postgresql.auto.conf", "fsync = on")
        node.append_conf("postgresql.auto.conf", "wal_level = minimal")

        node.append_conf("postgresql.auto.conf", "log_line_prefix = '%t [%p]: [%l-1] '")
        node.append_conf("postgresql.auto.conf", "log_statement = none")
        node.append_conf("postgresql.auto.conf", "log_duration = on")
        node.append_conf("postgresql.auto.conf", "log_min_duration_statement = 0")
        node.append_conf("postgresql.auto.conf", "log_connections = on")
        node.append_conf("postgresql.auto.conf", "log_disconnections = on")

        # Apply given parameters
        for key, value in six.iteritems(pg_options):
            node.append_conf("postgresql.auto.conf", "%s = %s" % (key, value))

        # Allow replication in pg_hba.conf
        if set_replication:
            node.set_replication_conf()
            node.append_conf("postgresql.auto.conf", "max_wal_senders = 10")
        return node

    def create_tblspace_in_node(self, node, tblspc_name, cfs=False):
        res = node.execute(
            "postgres", "select exists (select 1 from pg_tablespace where spcname = '{0}')".format(
                tblspc_name))
        # Check that tablespace with name 'tblspc_name' do not exists already
        self.assertFalse(res[0][0], 'Tablespace "{0}" already exists'.format(tblspc_name))

        tblspc_path = os.path.join(node.base_dir, '{0}'.format(tblspc_name))
        cmd = "CREATE TABLESPACE {0} LOCATION '{1}'".format(tblspc_name, tblspc_path)
        if cfs:
            cmd += " with (compression=true)"
        os.makedirs(tblspc_path)
        res = node.safe_psql("postgres", cmd)
        # Check that tablespace was successfully created
        # self.assertEqual(res[0], 0, 'Failed to create tablespace with cmd: {0}'.format(cmd))

    def get_tblspace_path(self, node, tblspc_name):
        return os.path.join(node.base_dir, tblspc_name)

    def get_fork_size(self, node, fork_name):
        return node.execute("postgres",
            "select pg_relation_size('{0}')/8192".format(fork_name))[0][0]

    def get_fork_path(self, node, fork_name):
        return os.path.join(node.base_dir, 'data',
            node.execute("postgres", "select pg_relation_filepath('{0}')".format(fork_name))[0][0])

    def get_md5_per_page_for_fork(self, file, size):
        file = os.open(file, os.O_RDONLY)
        offset = 0
        md5_per_page = {}
        for page in range(size):
            md5_per_page[page] = hashlib.md5(os.read(file, 8192)).hexdigest()
            offset += 8192
            os.lseek(file, offset, 0)
        os.close(file)
        return md5_per_page

    def get_ptrack_bits_per_page_for_fork(self, node, file, size=[]):
        if len(size) > 1:
            if size[0] > size[1]:
                size = size[0]
            else:
                size = size[1]
        else:
            size = size[0]

        if self.get_pgpro_edition(node) == 'enterprise':
            header_size = 48
        else:
            header_size = 24
        ptrack_bits_for_fork = []
        byte_size = os.path.getsize(file + '_ptrack')
        byte_size_minus_header = byte_size - header_size
        file = os.open(file + '_ptrack', os.O_RDONLY)
        os.lseek(file, header_size, 0)
        lots_of_bytes = os.read(file, byte_size_minus_header)
        byte_list = [lots_of_bytes[i:i+1] for i in range(len(lots_of_bytes))]
        for byte in byte_list:
            #byte_inverted = bin(int(byte, base=16))[2:][::-1]
            #bits = (byte >> x) & 1 for x in range(7, -1, -1)
            byte_inverted = bin(ord(byte))[2:].rjust(8, '0')[::-1]
            for bit in byte_inverted:
                if len(ptrack_bits_for_fork) < size:
                    ptrack_bits_for_fork.append(int(bit))
        os.close(file)
        return ptrack_bits_for_fork

    def check_ptrack_sanity(self, idx_dict):
        success = True
        if idx_dict['new_size'] > idx_dict['old_size']:
            size = idx_dict['new_size']
        else:
            size = idx_dict['old_size']
        for PageNum in range(size):
            if PageNum not in idx_dict['old_pages']:
                # Page was not present before, meaning that relation got bigger
                # Ptrack should be equal to 1
                if idx_dict['ptrack'][PageNum] != 1:
                    if self.verbose:
                        print('Page Number {0} of type {1} was added, but ptrack value is {2}. THIS IS BAD'.format(
                            PageNum, idx_dict['type'], idx_dict['ptrack'][PageNum]))
                        print(idx_dict)
                    success = False
                continue
            if PageNum not in idx_dict['new_pages']:
                # Page is not present now, meaning that relation got smaller
                # Ptrack should be equal to 0, We are not freaking out about false positive stuff
                if idx_dict['ptrack'][PageNum] != 0:
                    if self.verbose:
                        print('Page Number {0} of type {1} was deleted, but ptrack value is {2}'.format(
                            PageNum, idx_dict['type'], idx_dict['ptrack'][PageNum]))
                continue
            # Ok, all pages in new_pages that do not have corresponding page in old_pages
            # are been dealt with. We can now safely proceed to comparing old and new pages 
            if idx_dict['new_pages'][PageNum] != idx_dict['old_pages'][PageNum]:
                # Page has been changed, meaning that ptrack should be equal to 1
                if idx_dict['ptrack'][PageNum] != 1:
                    if self.verbose:
                        print('Page Number {0} of type {1} was changed, but ptrack value is {2}. THIS IS BAD'.format(
                            PageNum, idx_dict['type'], idx_dict['ptrack'][PageNum]))
                        print(idx_dict)
                    if PageNum == 0 and idx_dict['type'] == 'spgist':
                        if self.verbose:
                            print('SPGIST is a special snowflake, so don`t fret about losing ptrack for blknum 0')
                        continue
                    success = False
            else:
                # Page has not been changed, meaning that ptrack should be equal to 0
                if idx_dict['ptrack'][PageNum] != 0:
                    if self.verbose:
                        print('Page Number {0} of type {1} was not changed, but ptrack value is {2}'.format(
                            PageNum, idx_dict['type'], idx_dict['ptrack'][PageNum]))
                        print(idx_dict)
            self.assertEqual(success, True, 'Ptrack of index {0} does not correspond to state of its pages.\n Gory Details: \n{1}'.format(
                idx_dict['type'], idx_dict))

    def check_ptrack_recovery(self, idx_dict):
        size = idx_dict['size']
        for PageNum in range(size):
            if idx_dict['ptrack'][PageNum] != 1:
                self.assertTrue(False, 'Recovery for Page Number {0} of Type {1} was conducted, but ptrack value is {2}. THIS IS BAD\n IDX_DICT: {3}'.format(
                    PageNum, idx_dict['type'], idx_dict['ptrack'][PageNum], idx_dict))

    def check_ptrack_clean(self, idx_dict, size):
        for PageNum in range(size):
            if idx_dict['ptrack'][PageNum] != 0:
                self.assertTrue(False, 'Ptrack for Page Number {0} of Type {1} should be clean, but ptrack value is {2}.\n THIS IS BAD\n IDX_DICT: {3}'.format(
                    PageNum, idx_dict['type'], idx_dict['ptrack'][PageNum], idx_dict))

    def run_pb(self, command):
        try:
            self.cmd = [' '.join(map(str,[self.probackup_path] + command))]
            if self.verbose:
                print(self.cmd)
            self.output = subprocess.check_output(
                [self.probackup_path] + command,
                stderr=subprocess.STDOUT,
                env=self.test_env
                ).decode("utf-8")
            if command[0] == 'backup':
                # return backup ID
                for line in self.output.splitlines():
                    if 'INFO: Backup' and 'completed' in line:
                        return line.split()[2]
            else:
                return self.output
        except subprocess.CalledProcessError as e:
            raise  ProbackupException(e.output.decode("utf-8"), self.cmd)

    def init_pb(self, backup_dir):

        shutil.rmtree(backup_dir, ignore_errors=True)
        return self.run_pb([
            "init",
            "-B", backup_dir
        ])

    def add_instance(self, backup_dir, instance, node):

        return self.run_pb([
            "add-instance",
            "--instance={0}".format(instance),
            "-B", backup_dir,
            "-D", node.data_dir
        ])

    def del_instance(self, backup_dir, instance):

        return self.run_pb([
            "del-instance",
            "--instance={0}".format(instance),
            "-B", backup_dir
        ])

    def clean_pb(self, backup_dir):
        shutil.rmtree(backup_dir, ignore_errors=True)

    def backup_node(self, backup_dir, instance, node, data_dir=False, backup_type="full", options=[]):
        if not node and not data_dir:
            print('You must provide ether node or data_dir for backup')
            exit(1)

        if node:
            pgdata = node.data_dir

        if data_dir:
            pgdata = data_dir

        cmd_list = [
            "backup",
            "-B", backup_dir,
#            "-D", pgdata,
            "-p", "%i" % node.port,
            "-d", "postgres",
            "--instance={0}".format(instance)
        ]
        if backup_type:
            cmd_list += ["-b", backup_type]

        return self.run_pb(cmd_list + options)

    def restore_node(self, backup_dir, instance, node=False, data_dir=None, backup_id=None, options=[]):
        if data_dir is None:
            data_dir = node.data_dir

        cmd_list = [
            "restore",
            "-B", backup_dir,
            "-D", data_dir,
            "--instance={0}".format(instance)
        ]
        if backup_id:
            cmd_list += ["-i", backup_id]

        return self.run_pb(cmd_list + options)

    def show_pb(self, backup_dir, instance=None, backup_id=None, options=[], as_text=False):

        backup_list = []
        specific_record = {}
        cmd_list = [
            "show",
            "-B", backup_dir,
        ]
        if instance:
            cmd_list += ["--instance={0}".format(instance)]

        if backup_id:
            cmd_list += ["-i", backup_id]

        if as_text:
            # You should print it when calling as_text=true
            return self.run_pb(cmd_list + options)

        # get show result as list of lines
        show_splitted = self.run_pb(cmd_list + options).splitlines()
        if instance is not None and backup_id is None:
            # cut header(ID, Mode, etc) from show as single string
            header = show_splitted[1:2][0]
            # cut backup records from show as single list with string for every backup record
            body = show_splitted[3:]
            # inverse list so oldest record come first
            body = body[::-1]
            # split string in list with string for every header element
            header_split = re.split("  +", header)
            # Remove empty items
            for i in header_split:
                if i == '':
                    header_split.remove(i)
            for backup_record in body:
                # split string in list with string for every backup record element
                backup_record_split = re.split("  +", backup_record)
                # Remove empty items
                for i in backup_record_split:
                    if i == '':
                        backup_record_split.remove(i)
                if len(header_split) != len(backup_record_split):
                    print(warning.format(
                        header=header, body=body,
                        header_split=header_split, body_split=backup_record_split))
                    exit(1)
                new_dict = dict(zip(header_split, backup_record_split))
                backup_list.append(new_dict)
            return backup_list
        else:
            # cut out empty lines and lines started with #
            # and other garbage then reconstruct it as dictionary
            # print show_splitted
            sanitized_show = [item for item in show_splitted if item]
            sanitized_show = [item for item in sanitized_show if not item.startswith('#')]
            # print sanitized_show
            for line in sanitized_show:
                name, var = line.partition(" = ")[::2]
                var = var.strip('"')
                var = var.strip("'")
                specific_record[name.strip()] = var
            return specific_record

    def validate_pb(self, backup_dir, instance=None, backup_id=None, options=[]):

        cmd_list = [
            "validate",
            "-B", backup_dir
        ]
        if instance:
            cmd_list += ["--instance={0}".format(instance)]
        if backup_id:
            cmd_list += ["-i", backup_id]

        return self.run_pb(cmd_list + options)

    def delete_pb(self, backup_dir, instance, backup_id=None, options=[]):
        cmd_list = [
            "delete",
            "-B", backup_dir
        ]

        cmd_list += ["--instance={0}".format(instance)]
        if backup_id:
            cmd_list += ["-i", backup_id]

        return self.run_pb(cmd_list + options)

    def delete_expired(self, backup_dir, instance, options=[]):
        cmd_list = [
            "delete", "--expired",
            "-B", backup_dir,
            "--instance={0}".format(instance)
        ]
        return self.run_pb(cmd_list + options)

    def show_config(self, backup_dir, instance):
        out_dict = {}
        cmd_list = [
            "show-config",
            "-B", backup_dir,
            "--instance={0}".format(instance)
        ]
        res = self.run_pb(cmd_list).splitlines()
        for line in res:
            if not line.startswith('#'):
                name, var = line.partition(" = ")[::2]
                out_dict[name] = var
        return out_dict


    def get_recovery_conf(self, node):
        out_dict = {}
        with open(os.path.join(node.data_dir, "recovery.conf"), "r") as recovery_conf:
            for line in recovery_conf:
                try:
                    key, value = line.split("=")
                except:
                    continue
                out_dict[key.strip()] = value.strip(" '").replace("'\n", "")
        return out_dict

    def set_archiving(self, backup_dir, instance, node, replica=False):

        if replica:
            archive_mode = 'always'
            node.append_conf('postgresql.auto.conf', 'hot_standby = on')
        else:
            archive_mode = 'on'

        node.append_conf(
                "postgresql.auto.conf",
                "wal_level = archive"
                )
        node.append_conf(
                "postgresql.auto.conf",
                "archive_mode = {0}".format(archive_mode)
                )
        if os.name == 'posix':
            node.append_conf(
                    "postgresql.auto.conf",
                    "archive_command = '{0} archive-push -B {1} --instance={2} --wal-file-path %p --wal-file-name %f'".format(
                        self.probackup_path, backup_dir, instance))
        #elif os.name == 'nt':
        #    node.append_conf(
        #            "postgresql.auto.conf",
        #            "archive_command = 'copy %p {0}\\%f'".format(archive_dir)
        #            )

    def set_replica(self, master, replica, replica_name='replica', synchronous=False):
        replica.append_conf('postgresql.auto.conf', 'port = {0}'.format(replica.port))
        replica.append_conf('postgresql.auto.conf', 'hot_standby = on')
        replica.append_conf('recovery.conf', "standby_mode = 'on'")
        replica.append_conf('recovery.conf',
            "primary_conninfo = 'user={0} port={1} application_name={2} sslmode=prefer sslcompression=1'".format(
                self.user, master.port, replica_name))
        if synchronous:
            master.append_conf('postgresql.auto.conf', "synchronous_standby_names='{0}'".format(replica_name))
            master.append_conf('postgresql.auto.conf', "synchronous_commit='remote_apply'")
            master.reload()

    def wrong_wal_clean(self, node, wal_size):
        wals_dir = os.path.join(self.backup_dir(node), "wal")
        wals = [f for f in os.listdir(wals_dir) if os.path.isfile(os.path.join(wals_dir, f))]
        wals.sort()
        file_path = os.path.join(wals_dir, wals[-1])
        if os.path.getsize(file_path) != wal_size:
            os.remove(file_path)

    def guc_wal_segment_size(self, node):
        var = node.execute("postgres", "select setting from pg_settings where name = 'wal_segment_size'")
        return int(var[0][0]) * self.guc_wal_block_size(node)

    def guc_wal_block_size(self, node):
        var = node.execute("postgres", "select setting from pg_settings where name = 'wal_block_size'")
        return int(var[0][0])

    def get_pgpro_edition(self, node):
        if node.execute("postgres", "select exists(select 1 from pg_proc where proname = 'pgpro_edition')")[0][0]:
            var = node.execute("postgres", "select pgpro_edition()")
            return str(var[0][0])
        else:
            return False

    def get_username(self):
        """ Returns current user name """
        return pwd.getpwuid(os.getuid())[0]

    def del_test_dir(self, module_name, fname):
        """ Del testdir and optimistically try to del module dir"""
        try:
            clean_all()
        except:
            pass

        shutil.rmtree(os.path.join(self.tmp_path, module_name, fname),
            ignore_errors=True)
        try:
            os.rmdir(os.path.join(self.tmp_path, module_name))
        except:
            pass

    def pgdata_content(self, directory):
        """ return dict with directory content. TAKE IT AFTER CHECKPOINT or BACKUP"""
        dirs_to_ignore = ['pg_xlog', 'pg_wal', 'pg_log', 'pg_stat_tmp', 'pg_subtrans', 'pg_notify']
        files_to_ignore = ['postmaster.pid', 'postmaster.opts', 'pg_internal.init', 'postgresql.auto.conf']
        suffixes_to_ignore = ('_ptrack', 'ptrack_control', 'pg_control', 'ptrack_init')
        directory_dict = {}
        directory_dict['pgdata'] = directory
        directory_dict['files'] = {}
        for root, dirs, files in os.walk(directory, followlinks=True):
            dirs[:] = [d for d in dirs if d not in dirs_to_ignore]
            for file in files:
                if file in files_to_ignore or file.endswith(suffixes_to_ignore):
                    continue
                file = os.path.join(root,file)
                file_relpath = os.path.relpath(file, directory)
                directory_dict['files'][file_relpath] = hashlib.md5(open(file, 'rb').read()).hexdigest()
        return directory_dict

    def compare_pgdata(self, original_pgdata, restored_pgdata):
        """ return dict with directory content. DO IT BEFORE RECOVERY"""
        fail = False
        error_message = ''
        for file in original_pgdata['files']:
            if file in restored_pgdata['files']:
                if original_pgdata['files'][file] != restored_pgdata['files'][file]:
                    error_message += '\nChecksumm mismatch.\n File_old: {0}\n Checksumm_old: {1}\n File_new: {2}\n Checksumm_mew: {3}\n'.format(
                        os.path.join(original_pgdata['pgdata'], file),
                        original_pgdata['files'][file],
                        os.path.join(restored_pgdata['pgdata'], file),
                        restored_pgdata['files'][file])
                    fail = True
            else:
                error_message += '\nFile dissappearance. File: {0}/{1}'.format(restored_pgdata['pgdata'], file)
                fail = True
        self.assertFalse(fail, error_message)
