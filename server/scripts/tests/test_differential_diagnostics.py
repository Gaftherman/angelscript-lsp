from contextlib import redirect_stdout
from io import StringIO
import json
from pathlib import Path
from runpy import run_path
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import Mock, patch


validator = run_path(str(Path(__file__).parents[1] / "differential-validate.py"))
read_error_diagnostics = validator["read_error_diagnostics"]
main = validator["main"]
CODE = "as-warn-undeclared-identifier"


def diagnostic(severity, start=(1, 2), end=(1, 3), code=CODE):
    return {
        "severity": severity,
        "message": "example",
        "code": code,
        "range": {
            "start": {"line": start[0], "character": start[1]},
            "end": {"line": end[0], "character": end[1]},
        },
    }


class DiagnosticReportTest(unittest.TestCase):
    def test_preserves_error_records_in_report_order(self):
        first, second = diagnostic(1), diagnostic(1)
        second["message"] = "another error"
        client = Mock()
        client.tool.return_value = [diagnostic(2), first, diagnostic(4), second]

        self.assertEqual(read_error_diagnostics(client, "peer.as"), [first, second])
        client.tool.assert_called_once_with("diagnostics", path="peer.as")

    def test_reports_malformed_payloads_with_source(self):
        client = Mock()
        reversed_range = diagnostic(2)
        reversed_range["range"]["end"]["character"] = 1
        cases = [
            ({"state": "unavailable"}, "expected an array"),
            (["unavailable"], "item 1: expected an object"),
            ([{"message": "missing severity"}], "severity"),
            ([diagnostic(True)], "invalid severity"),
            ([dict(diagnostic(1), code=[])], "invalid code"),
            ([{"severity": 2, "message": "missing range"}], "range"),
            ([reversed_range], "invalid range"),
        ]
        for payload, detail in cases:
            with self.subTest(payload=payload):
                client.tool.return_value = payload
                with self.assertRaisesRegex(ValueError, rf"diagnostics for peer\.as.*{detail}"):
                    read_error_diagnostics(client, "peer.as")

    def test_preserves_diagnostics_transport_reason(self):
        client = Mock()
        client.tool.side_effect = RuntimeError("MCP transport closed")

        with self.assertRaisesRegex(ValueError, r"diagnostics for main\.as: MCP transport closed"):
            read_error_diagnostics(client, "main.as")


class DifferentialComparisonTest(unittest.TestCase):
    def run_case(
        self, compiler, server, source="  value;\n", *, show_unpaired=False, category="category", expect="reject"
    ):
        with TemporaryDirectory() as work:
            root = Path(work)
            case = root / category / "case"
            case.mkdir(parents=True)
            (case / "main.as").write_text(f"// @expect: {expect}\n{source}", encoding="utf-8")
            (case / "peer.as").write_text(f"\n{source}", encoding="utf-8")
            log = root / "report.json"
            client = Mock()
            client.tool.side_effect = lambda tool, **args: server.get(args["path"], []) if tool == "diagnostics" else {}
            client_factory = Mock(return_value=client)
            output = StringIO()

            def compile_case(args, **kwargs):
                if isinstance(compiler, Exception):
                    raise compiler
                if isinstance(compiler, subprocess.CompletedProcess):
                    return compiler
                messages = []
                for file, records in compiler.items():
                    for record in records:
                        messages.append(
                            {
                                "type": "ERROR",
                                "section": "default arg" if file is None else str(kwargs["cwd"] / file),
                                "row": record.get("row", 2),
                                "col": record.get("col", 3),
                                "message": "different compiler wording",
                                "code": record.get("code"),
                                "range": record.get("range"),
                            }
                        )
                return subprocess.CompletedProcess(
                    args, int(bool(messages)), json.dumps({"hasError": bool(messages), "messages": messages}), ""
                )

            argv = ["differential-validate.py", "--cases", work, "--log", str(log)]
            if show_unpaired:
                argv.append("--show-unpaired")
            with (
                patch.object(sys, "argv", argv),
                patch.object(subprocess, "run", side_effect=compile_case),
                patch.dict(main.__globals__, {"McpClient": client_factory}),
                redirect_stdout(output),
            ):
                status = main()
            report = json.loads(log.read_text(encoding="utf-8"))
            self.output = output.getvalue()
            self.assertEqual(report["summary"]["cases"], 1)
            for name, count in report["summary"].items():
                self.assertIn(f"{name}={count}", self.output.split())
            return status, report["summary"], report["findings"], client_factory

    def test_fully_matching_case_ignores_message_text(self):
        reports = {"main.as": [diagnostic(1)], "peer.as": [diagnostic(1, (4, 29), (4, 40))]}
        status, summary, findings, _ = self.run_case(reports, reports)
        self.assertEqual(status, 0)
        self.assertEqual(summary["matched"], 2)
        self.assertEqual(findings, [])

    def test_wrong_range_checks_all_four_coordinates(self):
        for start, end in [((0, 2), (1, 3)), ((1, 1), (1, 3)), ((1, 2), (2, 3)), ((1, 2), (1, 4))]:
            with self.subTest(start=start, end=end):
                status, summary, findings, _ = self.run_case(
                    {"main.as": [diagnostic(1)]}, {"main.as": [diagnostic(1, start, end)]}
                )
                self.assertEqual(status, 1)
                self.assertEqual(summary["matched"], 0)
                self.assertEqual(summary["range_file_mismatch"], 1)
                self.assertEqual(findings[0]["kind"], "RANGE_FILE_MISMATCH")

    def test_wrong_file_is_a_discrepancy(self):
        for oracle_file in ("peer.as", "another.as"):
            with self.subTest(file=oracle_file):
                status, summary, findings, _ = self.run_case(
                    {oracle_file: [diagnostic(1)]}, {"main.as": [diagnostic(1)]}
                )
                self.assertEqual(status, 1)
                self.assertEqual(summary["matched"], 0)
                self.assertEqual(summary["range_file_mismatch"], 1)
                self.assertIn(oracle_file, findings[0]["detail"])

    def test_same_code_at_multiple_ranges_matches_one_for_one(self):
        exact = diagnostic(1)
        compiler = {"main.as": [exact, diagnostic(1, (4, 2), (4, 3))]}
        server = {"main.as": [diagnostic(1, (4, 2), (4, 4)), exact, exact]}
        status, summary, _, _ = self.run_case(compiler, server)
        self.assertEqual(status, 1)
        self.assertEqual(summary["matched"], 1)
        self.assertEqual(summary["range_file_mismatch"], 1)
        self.assertEqual(summary["server_only"], 1)

    def test_same_file_and_full_range_match_with_different_codes(self):
        status, summary, findings, _ = self.run_case(
            {"main.as": [diagnostic(1)]}, {"main.as": [diagnostic(1, code="different-code")]}
        )
        self.assertEqual(status, 0)
        self.assertEqual(summary["matched"], 1)
        self.assertEqual(findings, [])

    def test_equal_range_with_different_files_and_codes_does_not_match(self):
        status, summary, findings, _ = self.run_case(
            {"peer.as": [diagnostic(1)]}, {"main.as": [diagnostic(1, code="different-code")]}
        )
        self.assertEqual(status, 1)
        self.assertEqual(summary["matched"], 0)
        self.assertEqual(summary["oracle_only"], 1)
        self.assertEqual(
            [finding["kind"] for finding in findings], ["ORACLE_ONLY", "VERDICT_MISMATCH", "SERVER_EXTRA_FILE"]
        )

    def test_oracle_only_file_fails_the_verdict(self):
        status, summary, findings, _ = self.run_case({"main.as": [{"code": None, "range": None}]}, {"main.as": []})
        self.assertEqual(status, 1)
        self.assertEqual(summary["discrepancies"], 1)
        self.assertEqual(summary["verdict_mismatch"], 1)
        self.assertEqual(summary["position_unpaired"], 0)
        self.assertEqual(summary["oracle_failures"], 0)
        self.assertEqual(findings[0]["kind"], "VERDICT_MISMATCH")
        self.assertIn("main.as: oracle reports errors", findings[0]["detail"])

    def test_server_only_file_is_informational_after_position_match(self):
        status, summary, findings, _ = self.run_case(
            {"main.as": [{}]}, {"main.as": [diagnostic(1)], "peer.as": [diagnostic(1)]}
        )
        self.assertEqual(status, 0)
        self.assertEqual(summary["matched"], 1)
        self.assertEqual(summary["discrepancies"], 0)
        self.assertEqual(summary["verdict_mismatch"], 0)
        self.assertEqual(summary["server_extra_files"], 1)
        self.assertEqual(summary["position_unpaired"], 0)
        self.assertEqual(findings[0]["kind"], "SERVER_EXTRA_FILE")
        self.assertIn("peer.as: server reports errors", findings[0]["detail"])

    def test_known_gap_findings_are_listed_and_do_not_fail(self):
        status, summary, findings, _ = self.run_case({"missing.as": [diagnostic(1)]}, {}, category="_known_gaps")
        self.assertEqual(status, 0)
        self.assertEqual(summary["known_gaps"], 2)
        self.assertEqual(summary["discrepancies"], 0)
        self.assertEqual(summary["oracle_only"], 0)
        self.assertEqual(summary["verdict_mismatch"], 0)
        self.assertEqual(
            [(finding["kind"], finding["classification"]) for finding in findings],
            [("KNOWN_GAP_ORACLE_ONLY", "known_gap"), ("KNOWN_GAP_VERDICT_MISMATCH", "known_gap")],
        )
        self.assertIn("KNOWN_GAP_ORACLE_ONLY", self.output)
        self.assertIn("KNOWN_GAP_VERDICT_MISMATCH", self.output)

    def test_multiple_server_errors_in_an_extra_file_count_once(self):
        error = diagnostic(1)
        status, summary, findings, _ = self.run_case(
            {"peer.as": [error]}, {"main.as": [error, error], "peer.as": [error]}
        )
        self.assertEqual(status, 0)
        self.assertEqual(summary["matched"], 1)
        self.assertEqual(summary["server_extra_files"], 1)
        self.assertEqual(summary["discrepancies"], 0)
        self.assertEqual(
            [(finding["kind"], finding["classification"]) for finding in findings],
            [("SERVER_EXTRA_FILE", "informational")],
        )

    def test_server_error_fails_when_oracle_accepts_the_case(self):
        status, summary, findings, _ = self.run_case({}, {"peer.as": [diagnostic(1)]}, expect="accept")
        self.assertEqual(status, 1)
        self.assertEqual(summary["verdict_mismatch"], 1)
        self.assertEqual(summary["discrepancies"], 1)
        self.assertEqual(summary["server_extra_files"], 0)
        self.assertEqual(findings[0]["kind"], "VERDICT_MISMATCH")

    def test_oracle_only_file_fails_even_when_another_file_matches(self):
        error = diagnostic(1)
        status, summary, findings, _ = self.run_case({"main.as": [error], "peer.as": [{}]}, {"main.as": [error]})
        self.assertEqual(status, 1)
        self.assertEqual(summary["matched"], 1)
        self.assertEqual(summary["verdict_mismatch"], 1)
        self.assertEqual(summary["discrepancies"], 1)
        self.assertIn("peer.as: oracle reports errors", findings[0]["detail"])

    def test_identity_less_error_pairs_with_syntax_error_at_same_start(self):
        status, summary, findings, _ = self.run_case(
            {"main.as": [{}]}, {"main.as": [diagnostic(1, end=(1, 7), code="as-syntax-error")]}
        )
        self.assertEqual(status, 0)
        self.assertEqual(summary["matched"], 1)
        self.assertEqual(findings, [])

    def test_unpaired_positions_in_a_shared_error_file_are_informational(self):
        status, summary, findings, _ = self.run_case({"main.as": [{"col": 4}]}, {"main.as": [diagnostic(1)]})
        self.assertEqual(status, 0)
        self.assertEqual(summary["matched"], 0)
        self.assertEqual(summary["discrepancies"], 0)
        self.assertEqual(summary["position_unpaired"], 2)
        self.assertEqual([finding["kind"] for finding in findings], ["SERVER_ONLY", "ORACLE_ONLY"])
        self.assertTrue(all(finding["classification"] == "informational" for finding in findings))

    def test_show_unpaired_toggles_stdout_details_only(self):
        compiler, server = {"main.as": [{"col": 4}]}, {"main.as": [diagnostic(1)]}
        status, summary, findings, _ = self.run_case(compiler, server)
        self.assertEqual(len(self.output.splitlines()), 1)
        shown_status, shown_summary, shown_findings, _ = self.run_case(compiler, server, show_unpaired=True)
        self.assertEqual((shown_status, shown_summary, shown_findings), (status, summary, findings))
        self.assertIn("  SERVER_ONLY", self.output)
        self.assertIn("  ORACLE_ONLY", self.output)

    def test_unmatched_oracle_identity_fails_even_when_positions_pair(self):
        status, summary, findings, _ = self.run_case(
            {"main.as": [diagnostic(1), {}]}, {"main.as": [diagnostic(1, end=(1, 4), code="different-code")]}
        )
        self.assertEqual(status, 1)
        self.assertEqual(summary["matched"], 1)
        self.assertEqual(summary["discrepancies"], 1)
        self.assertEqual(summary["position_unpaired"], 0)
        self.assertEqual(
            [(finding["kind"], finding["classification"]) for finding in findings], [("ORACLE_ONLY", "failing")]
        )

    def test_identity_less_error_does_not_pair_in_another_file(self):
        status, summary, findings, _ = self.run_case({"peer.as": [{}]}, {"main.as": [diagnostic(1)]})
        self.assertEqual(status, 1)
        self.assertEqual(summary["matched"], 0)
        self.assertEqual(summary["discrepancies"], 1)
        self.assertEqual(summary["server_extra_files"], 1)
        self.assertEqual(summary["position_unpaired"], 0)
        self.assertEqual([finding["kind"] for finding in findings], ["VERDICT_MISMATCH", "SERVER_EXTRA_FILE"])

    def test_synthetic_section_falls_back_to_case_verdict(self):
        for server, expected in (({}, 1), ({"peer.as": [diagnostic(1)]}, 0)):
            with self.subTest(server=server):
                status, summary, _, _ = self.run_case({None: [{}]}, server)
                self.assertEqual(status, expected)
                self.assertEqual(summary["verdict_mismatch"], expected)
                self.assertEqual(summary["position_unpaired"], 0)

    def test_synthetic_section_does_not_override_attributed_verdict(self):
        status, summary, _, _ = self.run_case({None: [{}], "main.as": [{}]}, {"peer.as": [diagnostic(1)]})
        self.assertEqual(status, 1)
        self.assertEqual(summary["verdict_mismatch"], 1)
        self.assertEqual(summary["server_extra_files"], 1)
        self.assertEqual(summary["position_unpaired"], 0)

    def test_identity_less_error_converts_utf8_bytes_to_utf16_units(self):
        status, summary, findings, _ = self.run_case(
            {"peer.as": [{"col": 7}]}, {"peer.as": [diagnostic(1, (1, 3), (1, 4))]}, source="é😀value;\n"
        )
        self.assertEqual(status, 0)
        self.assertEqual(summary["matched"], 1)
        self.assertEqual(findings, [])

    def test_position_matches_consume_each_diagnostic_once(self):
        extra = diagnostic(1, (1, 3), (1, 4))
        status, summary, findings, _ = self.run_case(
            {"main.as": [{}, {}, {"col": 4}]}, {"main.as": [diagnostic(1), extra, extra]}
        )
        self.assertEqual(status, 0)
        self.assertEqual(summary["matched"], 2)
        self.assertEqual(summary["position_unpaired"], 2)
        self.assertEqual(summary["server_only"], 1)
        self.assertEqual(summary["oracle_only"], 1)
        self.assertEqual([finding["kind"] for finding in findings], ["SERVER_ONLY", "ORACLE_ONLY"])
        self.assertIn("main.as:2:3:", findings[1]["detail"])

    def test_exact_identity_is_reserved_before_position_matching(self):
        exact = diagnostic(1)
        status, summary, findings, _ = self.run_case({"main.as": [{}, exact]}, {"main.as": [exact]})
        self.assertEqual(status, 0)
        self.assertEqual(summary["matched"], 1)
        self.assertEqual(summary["position_unpaired"], 1)
        self.assertEqual([finding["kind"] for finding in findings], ["ORACLE_ONLY"])
        self.assertIn("main.as:2:3:", findings[0]["detail"])

    def test_full_range_is_reserved_before_position_matching(self):
        status, summary, findings, _ = self.run_case(
            {"main.as": [{}, diagnostic(1)]}, {"main.as": [diagnostic(1, code="different-code")]}
        )
        self.assertEqual(status, 0)
        self.assertEqual(summary["matched"], 1)
        self.assertEqual(summary["position_unpaired"], 1)
        self.assertEqual(
            [(finding["kind"], finding["classification"]) for finding in findings], [("ORACLE_ONLY", "informational")]
        )

    def test_oracle_failures_are_neither_discrepancies_nor_contradictions(self):
        for failure in (
            subprocess.CompletedProcess([], -11, "", "crash"),
            subprocess.TimeoutExpired([], 60),
            subprocess.CompletedProcess([], 1, "non-JSON output", ""),
        ):
            with self.subTest(failure=failure):
                status, summary, findings, client = self.run_case(failure, {"main.as": [diagnostic(1)]})
                self.assertEqual(status, 1)
                self.assertEqual(summary["oracle_failures"], 1)
                self.assertEqual(summary["oracle_contradictions"], 0)
                self.assertEqual(summary["discrepancies"], 0)
                self.assertEqual(summary["server_only"], 0)
                self.assertEqual([finding["kind"] for finding in findings], ["ORACLE_ERROR"])
                client.assert_not_called()

    def test_oracle_contradiction_has_its_own_count(self):
        status, summary, findings, client = self.run_case({}, {})
        self.assertEqual(status, 1)
        self.assertEqual(summary["oracle_contradictions"], 1)
        self.assertEqual(summary["oracle_failures"], 0)
        self.assertEqual(summary["discrepancies"], 0)
        self.assertEqual([finding["kind"] for finding in findings], ["ORACLE_CONTRADICTS_CASE"])
        client.assert_not_called()


if __name__ == "__main__":
    unittest.main()
