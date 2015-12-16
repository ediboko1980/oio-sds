# Copyright (C) 2015 OpenIO, original work as part of
# OpenIO Software Defined Storage
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 3.0 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library.

from mock import MagicMock as Mock

from oio.common.exceptions import InconsistentContent
from oio.content.factory import ContentFactory
from oio.content.dup import DupContent
from tests.utils import BaseTestCase


class TestContentFactory(BaseTestCase):
    def setUp(self):
        super(TestContentFactory, self).setUp()
        self.namespace = self.conf['namespace']
        self.gridconf = {"namespace": self.namespace}
        self.content_factory = ContentFactory(self.gridconf)

    def tearDown(self):
        super(TestContentFactory, self).tearDown()

    def test_extract_datasec(self):
        self.content_factory.ns_info = {
            "data_security": {
                "DUPONETWO": "DUP:distance=1|nb_copy=2",
                "RAIN": "RAIN:k=6|m=2|algo=liber8tion"
            },
            "storage_policy": {
                "RAIN": "NONE:RAIN:NONE",
                "SINGLE": "NONE:NONE:NONE",
                "TWOCOPIES": "NONE:DUPONETWO:NONE"
            }
        }

        ds_type, ds_args = self.content_factory._extract_datasec("RAIN")
        self.assertEqual(ds_type, "RAIN")
        self.assertEqual(ds_args, {
            "k": "6",
            "m": "2",
            "algo": "liber8tion"
        })

        ds_type, ds_args = self.content_factory._extract_datasec("SINGLE")
        self.assertEqual(ds_type, "DUP")
        self.assertEqual(ds_args, {
            "nb_copy": "1",
            "distance": "0"
        })

        ds_type, ds_args = self.content_factory._extract_datasec("TWOCOPIES")
        self.assertEqual(ds_type, "DUP")
        self.assertEqual(ds_args, {
            "nb_copy": "2",
            "distance": "1"
        })

        self.assertRaises(InconsistentContent,
                          self.content_factory._extract_datasec,
                          "UnKnOwN")

    def test_get_rain(self):
        meta = {
            "chunk-method": "plain/rain?algo=liber8tion&k=6&m=2",
            "ctime": 1450176946,
            "deleted": False,
            "hash": "E952A419957A6E405BFC53EC65483F73",
            "hash-method": "md5",
            "id": "3FA2C4A1ED2605005335A276890EC458",
            "length": 658,
            "mime-type": "application/octet-stream",
            "name": "tox.ini",
            "policy": "RAIN",
            "version": 1450176946676289
        }
        chunks = [
            {
                "url": "http://127.0.0.1:6012/A0A0",
                "pos": "0.p0", "size": 512,
                "hash": "E7D4E4AD460971CA2E3141F2102308D4"},
            {
                "url": "http://127.0.0.1:6010/A01",
                "pos": "0.1", "size": 146,
                "hash": "760AB5DA7C51A3654F1CA622687CD6C3"},
            {
                "url": "http://127.0.0.1:6011/A00",
                "pos": "0.0", "size": 512,
                "hash": "B1D08B86B8CAA90A2092CCA0DF9201DB"},
            {
                "url": "http://127.0.0.1:6013/A0A1",
                "pos": "0.p1", "size": 512,
                "hash": "DA9D7F72AEEA5791565724424CE45C16"}
        ]
        self.content_factory.container_client.content_show = Mock(
            return_value=(chunks, meta))
        # TODO
        self.assertRaises(Exception, self.content_factory.get,
                          "xxx_container_id", "xxx_content_id")

    def test_get_dup(self):
        meta = {
            "chunk-method": "plain/bytes",
            "ctime": 1450176946,
            "deleted": False,
            "hash": "E952A419957A6E405BFC53EC65483F73",
            "hash-method": "md5",
            "id": "3FA2C4A1ED2605005335A276890EC458",
            "length": 658,
            "mime-type": "application/octet-stream",
            "name": "tox.ini",
            "policy": "TWOCOPIES",
            "version": 1450176946676289
        }
        chunks = [
            {
                "url": "http://127.0.0.1:6010/A0",
                "pos": "0", "size": 658,
                "hash": "E952A419957A6E405BFC53EC65483F73"},
            {
                "url": "http://127.0.0.1:6011/A1",
                "pos": "0", "size": 658,
                "hash": "E952A419957A6E405BFC53EC65483F73"}
        ]
        self.content_factory.container_client.content_show = Mock(
            return_value=(chunks, meta))
        c = self.content_factory.get("xxx_container_id", "xxx_content_id")
        self.assertEqual(type(c), DupContent)
