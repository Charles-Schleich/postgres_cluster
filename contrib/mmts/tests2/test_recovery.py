#
# Based on Aphyr's test for CockroachDB.
#

import unittest
import time
import subprocess
import datetime
import docker
import warnings

from lib.bank_client import MtmClient
from lib.failure_injector import *

TEST_WARMING_TIME = 5
TEST_DURATION = 10
TEST_RECOVERY_TIME = 30
TEST_SETUP_TIME = 20
TEST_STOP_DELAY = 5

class TestHelper(object):

    def assertIsolation(self, aggs):
        isolated = True
        for conn_id, agg in enumerate(aggs):
            isolated = isolated and agg['sumtotal']['isolation'] == 0
        if not isolated:
            raise AssertionError('Isolation failure')

    def assertCommits(self, aggs):
        commits = True
        for conn_id, agg in enumerate(aggs):
            commits = commits and 'commit' in agg['transfer']['finish']
        if not commits:
            print('No commits during aggregation interval')
            # time.sleep(100000)
            raise AssertionError('No commits during aggregation interval')

    def assertNoCommits(self, aggs):
        commits = True
        for conn_id, agg in enumerate(aggs):
            commits = commits and 'commit' in agg['transfer']['finish']
        if commits:
            raise AssertionError('There are commits during aggregation interval')

    def performFailure(self, failure):

        time.sleep(TEST_WARMING_TIME)
         
        print('Simulate failure at ',datetime.datetime.utcnow())

        failure.start()

        self.client.clean_aggregates()
        time.sleep(TEST_DURATION)
        aggs_failure = self.client.get_aggregates()

        failure.stop()

        print('Eliminate failure at ',datetime.datetime.utcnow())

        self.client.clean_aggregates()
        time.sleep(TEST_RECOVERY_TIME)
        aggs = self.client.get_aggregates()

        return (aggs_failure, aggs)


class RecoveryTest(unittest.TestCase, TestHelper):

    @classmethod
    def setUpClass(cls):
        subprocess.check_call(['docker-compose','up',
            '--force-recreate',
            '--build',
            '-d'])

        # XXX: add normal wait here
        time.sleep(TEST_SETUP_TIME)

        cls.client = MtmClient([
            "dbname=regression user=postgres host=127.0.0.1 port=15432",
            "dbname=regression user=postgres host=127.0.0.1 port=15433",
            "dbname=regression user=postgres host=127.0.0.1 port=15434"
        ], n_accounts=1000)
        cls.client.bgrun()

    @classmethod
    def tearDownClass(cls):
        print('tearDown')
        cls.client.stop()

        time.sleep(TEST_STOP_DELAY)

        if not cls.client.is_data_identic():
            raise AssertionError('Different data on nodes')

        if cls.client.no_prepared_tx() != 0:
            raise AssertionError('There are some uncommitted tx')

        # XXX: check nodes data identity here
        # subprocess.check_call(['docker-compose','down'])

    def setUp(self):
        warnings.simplefilter("ignore", ResourceWarning)
        time.sleep(20)
        print('Start new test at ',datetime.datetime.utcnow())

    def tearDown(self):
        print('Finish test at ',datetime.datetime.utcnow())

    def test_normal_operations(self):
        print('### test_normal_operations ###')

        aggs_failure, aggs = self.performFailure(NoFailure())

        self.assertCommits(aggs_failure)
        self.assertIsolation(aggs_failure)

        self.assertCommits(aggs)
        self.assertIsolation(aggs)


    def test_node_partition(self):
        print('### test_node_partition ###')

        aggs_failure, aggs = self.performFailure(SingleNodePartition('node3'))

        self.assertCommits(aggs_failure[:2])
        self.assertNoCommits(aggs_failure[2:])
        self.assertIsolation(aggs_failure)

        self.assertCommits(aggs)
        self.assertIsolation(aggs)

    # def test_edge_partition(self):
    #     print('### test_edge_partition ###')

    #     aggs_failure, aggs = self.performFailure(EdgePartition('node2', 'node3'))

    #     self.assertTrue( ('commit' in aggs_failure[1]['transfer']['finish']) or ('commit' in aggs_failure[2]['transfer']['finish']) )
    #     self.assertCommits(aggs_failure[0:1]) # first node
    #     self.assertIsolation(aggs_failure)

    #     self.assertCommits(aggs)
    #     self.assertIsolation(aggs)

    def test_node_restart(self):
        print('### test_node_restart ###')

        aggs_failure, aggs = self.performFailure(RestartNode('node3'))

        self.assertCommits(aggs_failure[:2])
        self.assertNoCommits(aggs_failure[2:])
        self.assertIsolation(aggs_failure)

        self.assertCommits(aggs)
        self.assertIsolation(aggs)

    def test_node_crash(self):
        print('### test_node_crash ###')

        aggs_failure, aggs = self.performFailure(CrashRecoverNode('node3'))

        self.assertCommits(aggs_failure[:2])
        self.assertNoCommits(aggs_failure[2:])
        self.assertIsolation(aggs_failure)

        self.assertCommits(aggs)
        self.assertIsolation(aggs)

if __name__ == '__main__':
    unittest.main()

