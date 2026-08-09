#!/usr/bin/env python3
"""Host regression checks for the bounded live NodeSet exporter."""

import asyncio
import unittest

from asyncua import ua
from asyncua.common.node import Node

from export_nodeset import CachedExportNode, CachedXmlExporter


def data_value(value):
    return ua.DataValue(ua.Variant(value))


class FakeServer:
    async def get_namespace_array(self):
        return ["http://opcfoundation.org/UA/", "urn:test:device"]


class ExportNodeCacheTests(unittest.TestCase):
    def test_missing_inapplicable_attribute_uses_ua_status(self):
        async def exercise():
            source = Node(object(), ua.NodeId("Device", 1))
            attributes = {
                ua.AttributeIds.NodeClass: data_value(ua.NodeClass.Object),
                ua.AttributeIds.BrowseName: data_value(ua.QualifiedName("Device", 1)),
                ua.AttributeIds.DisplayName: data_value(ua.LocalizedText("Device", "en-US")),
                ua.AttributeIds.Description: data_value(ua.LocalizedText("", "en-US")),
                ua.AttributeIds.EventNotifier: data_value(0),
            }
            cached = CachedExportNode(source, attributes, [])

            with self.assertRaises(ua.UaStatusCodeError):
                await cached.read_data_type()

            exporter = CachedXmlExporter(FakeServer(), export_values=False)
            await exporter.build_etree([cached])

        asyncio.run(exercise())


if __name__ == "__main__":
    unittest.main()
