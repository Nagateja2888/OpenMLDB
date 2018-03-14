# -*- coding: utf-8 -*-
from testcasebase import TestCaseBase
import time
from libs.test_loader import load
import libs.utils as utils
from libs.deco import *


class TestMakeSnapshotNsClient(TestCaseBase):

    @multi_dimension(False)
    def test_makesnapshot_normal_success(self):
        """

        :return:
        """
        self.clear_ns_table(self.ns_leader)
        old_last_op_id = max(self.showopstatus(self.ns_leader).keys()) if self.showopstatus(self.ns_leader) != {} else 1
        name = 't{}'.format(int(time.time() * 1000000 % 10000000000))
        metadata_path = '{}/metadata.txt'.format(self.testpath)

        pid_group = '"0-2"'
        m = utils.gen_table_metadata(
            '"{}"'.format(name), None, 144000, 8,
            ('table_partition', '"{}"'.format(self.leader), pid_group, 'true'),
            ('table_partition', '"{}"'.format(self.slave1), pid_group, 'false'),
            ('table_partition', '"{}"'.format(self.slave2), pid_group, 'false'))
        utils.gen_table_metadata_file(m, metadata_path)
        rs = self.run_client(self.ns_leader, 'create ' + metadata_path, 'ns_client')
        self.assertTrue('Create table ok' in rs)

        table_info = self.showtable(self.ns_leader)
        tid = table_info.keys()[0][1]
        pid = table_info.keys()[0][2]

        self.put(self.leader, tid, pid, 'testkey0', self.now(), 'testvalue0')

        rs3 = self.makesnapshot(self.ns_leader, name, pid, 'ns_client')
        self.assertTrue('MakeSnapshot ok' in rs3)
        time.sleep(2)

        mf = self.get_manifest(self.leaderpath, tid, pid)
        self.assertEqual(mf['offset'], '1')
        self.assertTrue(mf['name'])
        self.assertEqual(mf['count'], '1')
        last_op_id = max(self.showopstatus(self.ns_leader).keys())
        self.assertFalse(old_last_op_id == last_op_id)
        last_opstatus = self.showopstatus(self.ns_leader)[last_op_id]
        self.assertTrue('kAddReplicaOP', last_opstatus)
        self.clear_ns_table(self.ns_leader)


    def test_makesnapshot_name_notexist(self):
        """

        :return:
        """
        name = 't{}'.format(int(time.time() * 1000000 % 10000000000))
        metadata_path = '{}/metadata.txt'.format(self.testpath)
        m = utils.gen_table_metadata(
            '"{}"'.format(name), None, 144000, 8,
            ('table_partition', '"{}"'.format(self.leader), '"0-3"', 'true'),
            ('table_partition', '"{}"'.format(self.slave1), '"0-3"', 'false'),
            ('table_partition', '"{}"'.format(self.slave2), '"0-3"', 'false'))
        utils.gen_table_metadata_file(m, metadata_path)
        rs = self.run_client(self.ns_leader, 'create ' + metadata_path, 'ns_client')
        self.assertTrue('Create table ok' in rs)

        rs3 = self.makesnapshot(self.ns_leader, name + 'aaa', 2, 'ns_client')
        self.assertTrue('Fail to makesnapshot. error msg:get table info failed' in rs3)


    def test_makesnapshot_pid_notexist(self):
        """

        :return:
        """
        name = 't{}'.format(int(time.time() * 1000000 % 10000000000))
        metadata_path = '{}/metadata.txt'.format(self.testpath)
        m = utils.gen_table_metadata(
            '"{}"'.format(name), None, 144000, 8,
            ('table_partition', '"{}"'.format(self.leader), '"0-3"', 'true'),
            ('table_partition', '"{}"'.format(self.slave1), '"0-3"', 'false'),
            ('table_partition', '"{}"'.format(self.slave2), '"0-3"', 'false'))
        utils.gen_table_metadata_file(m, metadata_path)
        rs = self.run_client(self.ns_leader, 'create ' + metadata_path, 'ns_client')
        self.assertTrue('Create table ok' in rs)

        rs3 = self.makesnapshot(self.ns_leader, name, 4, 'ns_client')
        self.assertTrue('Fail to makesnapshot. error msg:get leader failed' in rs3)


    def test_changeleader_and_makesnapshot(self):
        """
        changeleader后，可以makesnapshot，未changeleader的无法makesnapshot
        :return:
        """
        self.start_client(self.leader)
        metadata_path = '{}/metadata.txt'.format(self.testpath)
        name = 'tname{}'.format(time.time())
        m = utils.gen_table_metadata(
            '"{}"'.format(name), None, 144000, 2,
            ('table_partition', '"{}"'.format(self.leader), '"0-2"', 'true'),
            ('table_partition', '"{}"'.format(self.slave1), '"0-2"', 'false'),
            ('column_desc', '"merchant"', '"string"', 'true'),
            ('column_desc', '"amt"', '"double"', 'false'),
            ('column_desc', '"card"', '"string"', 'true'),
        )
        utils.gen_table_metadata_file(m, metadata_path)
        rs0 = self.ns_create(self.ns_leader, metadata_path)
        self.assertTrue('Create table ok' in rs0)

        rs1 = self.showtable(self.ns_leader)
        tid = rs1.keys()[0][1]

        self.confset(self.ns_leader, 'auto_failover', 'false')
        self.confset(self.ns_leader, 'auto_recover_table', 'false')
        self.put(self.leader, tid, 0, 'testkey0', self.now(), 'testvalue0')

        self.stop_client(self.leader)
        time.sleep(10)

        self.changeleader(self.ns_leader, name, 0)

        rs2 = self.showtable(self.ns_leader)
        rs3 = self.makesnapshot(self.ns_leader, name, 0, 'ns_client')
        rs4 = self.makesnapshot(self.ns_leader, name, 1, 'ns_client')
        self.start_client(self.leader)

        self.assertEqual(rs2[(name, tid, '0', self.leader)], ['leader', '2', '144000', 'no'])
        self.assertEqual(rs2[(name, tid, '1', self.leader)], ['leader', '2', '144000', 'no'])
        self.assertEqual(rs2[(name, tid, '2', self.leader)], ['leader', '2', '144000', 'no'])
        self.assertEqual(rs2[(name, tid, '0', self.slave1)], ['leader', '2', '144000', 'yes'])
        self.assertEqual(rs2[(name, tid, '1', self.slave1)], ['follower', '2', '144000', 'yes'])
        self.assertEqual(rs2[(name, tid, '2', self.slave1)], ['follower', '2', '144000', 'yes'])

        self.assertEqual('MakeSnapshot ok' in rs3, True)
        self.assertEqual('Fail to makesnapshot. error msg:get leader failed' in rs4, True)
        mf = self.get_manifest(self.slave1path, tid, 0)
        self.assertEqual(mf['offset'], '1')
        self.assertTrue(mf['name'])
        self.assertEqual(mf['count'], '1')


if __name__ == "__main__":
    load(TestMakeSnapshotNsClient)
