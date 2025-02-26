# Copyright 2021 4Paradigm
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

#! /bin/sh
#
# package.sh
#

WORKDIR=`pwd`
set -e
VERSION=${1:-snapshot}
OS=${2:-linux}
package=openmldb-${VERSION}-${OS}
echo "package name: ${package}"

rm -rf ${package} || :
mkdir ${package} || :
cp -r release/conf ${package}/conf
cp -r release/bin ${package}/bin
IP=127.0.0.1

sed -i"" -e "s/--endpoint=.*/--endpoint=${IP}:6527/g" ${package}/conf/nameserver.flags
sed -i"" -e "s/--zk_cluster=.*/--zk_cluster=${IP}:2181/g" ${package}/conf/nameserver.flags
sed -i"" -e "s/--zk_root_path=.*/--zk_root_path=\/openmldb/g" ${package}/conf/nameserver.flags

sed -i"" -e "s/--endpoint=.*/--endpoint=${IP}:9527/g" ${package}/conf/tablet.flags
sed -i"" -e "s/#--zk_cluster=.*/--zk_cluster=${IP}:2181/g" ${package}/conf/tablet.flags
sed -i"" -e "s/#--zk_root_path=.*/--zk_root_path=\/openmldb/g" ${package}/conf/tablet.flags

sed -i"" -e "s/#--zk_cluster=.*/--zk_cluster=${IP}:2181/g" ${package}/conf/apiserver.flags
sed -i"" -e "s/#--zk_root_path=.*/--zk_root_path=\/openmldb/g" ${package}/conf/apiserver.flags

cp -r tools ${package}/tools
ls -l build/bin/
cp -r build/bin/openmldb ${package}/bin/openmldb
test -e build/bin/openmldb_mac &&  cp -r build/bin/openmldb_mac ${package}/bin/openmldb_mac_cli
cd ${package}/bin
cd ../..
tar -cvzf ${package}.tar.gz ${package}
echo "package at ./${package}"
