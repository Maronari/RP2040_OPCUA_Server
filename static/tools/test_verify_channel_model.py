#!/usr/bin/env python3
"""No-network regression tests for fail-closed v0.4.2 channel verification."""

from __future__ import annotations

import argparse
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from verify_channel_model import (  # noqa: E402
    channel_value_type_expectations,
    distinct_scalar_value,
    parse_channel_value_type,
    validate_requested_evidence,
)


class VerifyChannelModelTests(unittest.TestCase):
    def test_exact_type_parser_accepts_supported_type(self) -> None:
        self.assertEqual(
            parse_channel_value_type("channel.counter=UInt64"),
            ("channel.counter", "UInt64"),
        )

    def test_exact_type_parser_rejects_incomplete_and_unknown_values(self) -> None:
        for value in ("channel.counter", "=UInt64", "channel.counter=Byte"):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                parse_channel_value_type(value)

    def test_conflicting_type_expectations_fail_closed(self) -> None:
        with self.assertRaises(ValueError):
            channel_value_type_expectations([
                ("channel.counter", "UInt32"),
                ("channel.counter", "UInt64"),
            ])

    def test_requested_write_evidence_requires_targets_and_credentials(self) -> None:
        args = argparse.Namespace(
            verify_wrong_type_write=True,
            verify_unauthorized_writable_write=False,
            expect_writable_channel=[],
            expect_value_type=[],
            require_mixed_value_types=False,
            second_username=None,
        )
        with self.assertRaises(ValueError):
            validate_requested_evidence(args)

        args.verify_wrong_type_write = False
        args.verify_unauthorized_writable_write = True
        args.expect_writable_channel = ["channel.loopback"]
        args.expect_value_type = [("channel.loopback", "UInt32")]
        with self.assertRaises(ValueError):
            validate_requested_evidence(args)

    def test_mixed_evidence_requires_exact_distinct_type_expectations(self) -> None:
        args = argparse.Namespace(
            verify_wrong_type_write=False,
            verify_unauthorized_writable_write=False,
            expect_writable_channel=[],
            expect_value_type=[("channel.a", "Float")],
            require_mixed_value_types=True,
            second_username=None,
        )
        with self.assertRaises(ValueError):
            validate_requested_evidence(args)

        args.expect_value_type.append(("channel.b", "Boolean"))
        validate_requested_evidence(args)

    def test_writable_target_requires_exact_type_expectation(self) -> None:
        args = argparse.Namespace(
            verify_wrong_type_write=False,
            verify_unauthorized_writable_write=False,
            expect_writable_channel=["channel.loopback"],
            expect_value_type=[],
            require_mixed_value_types=False,
            second_username=None,
        )
        with self.assertRaises(ValueError):
            validate_requested_evidence(args)

    def test_distinct_values_preserve_builtin_domain(self) -> None:
        cases = (
            (False, "Boolean", True),
            ("abc", "String", "abcx"),
            (1.5, "Float", 0.0),
            (0.0, "Double", 1.0),
            (2**31 - 1, "Int32", 2**31 - 2),
            (0, "UInt32", 1),
            (2**63 - 1, "Int64", 2**63 - 2),
            (2**64 - 1, "UInt64", 2**64 - 2),
        )
        for original, variant_type, expected in cases:
            with self.subTest(variant_type=variant_type):
                changed = distinct_scalar_value(original, variant_type)
                self.assertEqual(changed, expected)
                self.assertNotEqual(changed, original)


if __name__ == "__main__":
    unittest.main()
