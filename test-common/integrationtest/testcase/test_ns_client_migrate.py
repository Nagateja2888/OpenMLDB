# -*- coding: utf-8 -*-
from testcasebase import TestCaseBase
import time
import os
from libs.test_loader import load
import libs.utils as utils
from libs.logger import infoLogger
from libs.deco import multi_dimension
import libs.ddt as ddt
import libs.conf as conf


def get_base_attr(attr):
    TestCaseBase.setUpClass()
    return TestCaseBase.__getattribute__(TestCaseBase, attr)


@ddt.ddt
class TestNameserverMigrate(TestCaseBase):

    def createtable_put(self, tname, data_count):
        self.confset(self.ns_leader, 'auto_failover', 'true')
        self.confset(self.ns_leader, 'auto_recover_table', 'true')
        metadata_path = '{}/metadata.txt'.format(self.testpath)
        m = utils.gen_table_metadata(
            '"{}"'.format(tname), '"kAbsoluteTime"', 144000, 8,
            ('table_partition', '"{}"'.format(self.leader), '"0-10"', 'true'),
            ('table_partition', '"{}"'.format(self.slave1), '"3-8"', 'false'),
            ('table_partition', '"{}"'.format(self.slave2), '"0-3"', 'false'),
            ('column_desc', '"k1"', '"string"', 'true'),
            ('column_desc', '"k2"', '"string"', 'false'),
            ('column_desc', '"k3"', '"string"', 'false'))
        utils.gen_table_metadata_file(m, metadata_path)
        rs = self.ns_create(self.ns_leader, metadata_path)
        self.assertEqual('Create table ok' in rs, True)
        table_info = self.showtable(self.ns_leader)
        self.tid = int(table_info.keys()[0][1])
        self.pid = 4
        self.put_large_datas(data_count, 7)

    @ddt.data(
        ('4-6', [4, 5, 6]),
        ('4,6', [4, 6]),
        ('4', [4])
    )
    @ddt.unpack
    def test_ns_client_migrate_normal(self, pid_group, pid_range):
        """
        正常情况下迁移成功
        :param pid_group:
        :param pid_range:
        :return:
        """
        tname = str(time.time())
        self.createtable_put(tname, 500)
        time.sleep(2)
        rs1 = self.get_table_status(self.slave1, self.tid, self.pid)
        rs2 = self.migrate(self.ns_leader, self.slave1, tname, pid_group, self.slave2)
        time.sleep(2)
        rs3 = self.showtable(self.ns_leader)
        rs4 = self.get_table_status(self.slave2, self.tid, self.pid)
        self.assertEqual('partition migrate ok' in rs2, True)
        for i in pid_range:
            self.assertEqual((tname, str(self.tid), str(i), self.slave1) in rs3, False)
            self.assertEqual((tname, str(self.tid), str(i), self.slave2) in rs3, True)
        self.assertEqual(rs1[0], rs4[0])

    def test_ns_client_migrate_endpoint_offline(self):
        """
        节点离线，迁移失败
        """
        tname = str(time.time())
        self.createtable_put(tname, 1)
        self.stop_client(self.slave1)
        time.sleep(10)
        rs1 = self.migrate(self.ns_leader, self.slave1, tname, '4-6', self.slave2)
        rs2 = self.migrate(self.ns_leader, self.slave2, tname, '0-2', self.slave1)
        self.start_client(self.slave1)
        time.sleep(10)
        self.assertEqual('' in rs1, True)
        self.assertEqual('' in rs2, True)

    @ddt.data(
        (get_base_attr('slave1'), time.time(), '4-6', get_base_attr('slave1'),
         'src_endpoint is same as des_endpoint'),
        (get_base_attr('leader'), time.time(), '4-6', get_base_attr('slave1'),
         'cannot migrate leader'),
        ('src_notexists', time.time(), '4-6', get_base_attr('slave1'),
         'src_endpoint is not exist or not healthy'),
        (get_base_attr('slave1'), time.time(), '4-6', 'des_notexists',
         'des_endpoint is not exist or not healthy'),
        (get_base_attr('slave1'), 'table_not_exists', '4-6', get_base_attr('slave2'),
         'table is not exist'),
        (get_base_attr('slave1'), time.time(), '20', get_base_attr('slave2'),
         'leader endpoint is empty'),
        (get_base_attr('slave1'), time.time(), 'pid', get_base_attr('slave2'),
         'format error'),
        (get_base_attr('slave1'), time.time(), '', get_base_attr('slave2'),
         'Bad format.'),
        (get_base_attr('slave1'), time.time(), '8-9', get_base_attr('slave2'),
         'failed to migrate partition'),
        (get_base_attr('slave1'), time.time(), '3-4', get_base_attr('slave2'),
         'is already in des_endpoint'),
        (get_base_attr('slave1'), time.time(), '6-4', get_base_attr('slave2'),
         'has not valid pid'),
    )
    @ddt.unpack
    def test_ns_client_migrate_args_invalid(self, src, tname, pid_group, des, exp_msg):
        """
        参数异常时返回失败
        :param src:
        :param tname:
        :param pid_group:
        :param des:
        :param exp_msg:
        :return:
        """
        self.createtable_put(tname, 1)
        if tname != 'table_not_exists':
            rs2 = self.migrate(self.ns_leader, src, tname, pid_group, des)
        else:
            rs2 = self.migrate(self.ns_leader, src, 'table_not_exists_', pid_group, des)
        self.assertEqual(exp_msg in rs2, True)

    def test_ns_client_migrate_failover_and_recover(self):
        """
        迁移时发生故障切换，故障切换成功，迁移失败
        原leader故障恢复成follower之后，可以被迁移成功
        :return:
        """
        tname = str(time.time())
        self.createtable_put(tname, 100)
        time.sleep(2)
        rs0 = self.get_table_status(self.leader, self.tid, self.pid)  # get offset leader
        self.stop_client(self.leader)
        time.sleep(2)
        rs1 = self.migrate(self.ns_leader, self.slave1, tname, '4-6', self.slave2)
        time.sleep(8)
        rs2 = self.showtable(self.ns_leader)

        self.start_client(self.leader)  # recover table
        time.sleep(10)
        self.showtable(self.ns_leader)
        rs6 = self.get_table_status(self.slave1, self.tid, self.pid)  # get offset slave1
        rs3 = self.migrate(self.ns_leader, self.leader, tname, '4-6', self.slave2)
        time.sleep(2)
        rs4 = self.showtable(self.ns_leader)
        rs5 = self.get_table_status(self.slave2, self.tid, self.pid)  # get offset slave2

        self.assertEqual('partition migrate ok' in rs1, True)
        self.assertEqual('partition migrate ok' in rs3, True)
        for i in range(4, 7):
            self.assertEqual((tname, str(self.tid), str(i), self.slave1) in rs2, True)
            self.assertEqual((tname, str(self.tid), str(i), self.slave2) in rs2, False)
            self.assertEqual((tname, str(self.tid), str(i), self.leader) in rs4, False)
            self.assertEqual((tname, str(self.tid), str(i), self.slave2) in rs4, True)
        self.assertEqual(rs0[0], rs5[0])
        self.assertEqual(rs0[0], rs6[0])

    def test_ns_client_migrate_no_leader(self):
        """
        无主状态下迁移失败
        :return:
        """
        tname = str(time.time())
        self.confset(self.ns_leader, 'auto_failover', 'false')
        self.createtable_put(tname, 500)
        self.stop_client(self.leader)
        time.sleep(2)
        rs1 = self.migrate(self.ns_leader, self.slave1, tname, "4-6", self.slave2)
        time.sleep(2)
        rs2 = self.showtable(self.ns_leader)

        self.start_client(self.leader)
        time.sleep(10)
        self.showtable(self.ns_leader)
        self.confset(self.ns_leader, 'auto_failover', 'true')
        self.assertEqual('partition migrate ok' in rs1, True)
        for i in range(4, 7):
            self.assertEqual((tname, str(self.tid), str(i), self.slave1) in rs2, True)
            self.assertEqual((tname, str(self.tid), str(i), self.slave2) in rs2, False)


if __name__ == "__main__":
    load(TestNameserverMigrate)
