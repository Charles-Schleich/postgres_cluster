import os
import unittest
from .helpers.ptrack_helpers import ProbackupTest


class OptionTest(ProbackupTest, unittest.TestCase):

    def __init__(self, *args, **kwargs):
        super(OptionTest, self).__init__(*args, **kwargs)
        self.module_name = 'show'

    # @unittest.skip("skip")
    # @unittest.expectedFailure
    def test_show_1(self):
        """Status DONE and OK"""
        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, self.module_name, fname, 'backup')
        node = self.make_simple_node(base_dir="{0}/{1}/node".format(self.module_name, fname),
            initdb_params=['--data-checksums'],
            pg_options={'wal_level': 'replica'}
            )

        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.start()

        self.assertEqual(
            self.backup_node(backup_dir, 'node', node, options=["--log-level=panic"]),
            None
        )
        self.assertIn("OK", self.show_pb(backup_dir, 'node', as_text=True))

        # Clean after yourself
        self.del_test_dir(self.module_name, fname)

    # @unittest.skip("skip")
    def test_corrupt_2(self):
        """Status CORRUPT"""
        fname = self.id().split('.')[3]
        backup_dir = os.path.join(self.tmp_path, self.module_name, fname, 'backup')
        node = self.make_simple_node(base_dir="{0}/{1}/node".format(self.module_name, fname),
            initdb_params=['--data-checksums'],
            pg_options={'wal_level': 'replica'}
            )

        self.init_pb(backup_dir)
        self.add_instance(backup_dir, 'node', node)
        self.set_archiving(backup_dir, 'node', node)
        node.start()

        backup_id = self.backup_node(backup_dir, 'node', node)

        # delete file which belong to backup
        file = os.path.join(backup_dir, "backups", "node", backup_id, "database", "postgresql.conf")
        os.remove(file)

        self.validate_pb(backup_dir, 'node', backup_id)
        self.assertIn("CORRUPT", self.show_pb(backup_dir, as_text=True))

        # Clean after yourself
        self.del_test_dir(self.module_name, fname)
