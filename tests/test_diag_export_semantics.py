"""Real DLGF/DLG1 binary fixtures through the decoder and exported report.

Only synthetic numeric metadata is used. Private fixtures/results are retained
under build/alpha05/checks/diag-export; no device or serial connection is made.
"""
import contextlib
import csv
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/alpha05/checks/diag-export'
SPEC = importlib.util.spec_from_file_location('diag_export_semantics', ROOT / 'tools/diag_export.py')
EXPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORT)


def signed(value):
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


def record(boot, sequence, code, a=0, b=0, c=0, uptime=None, flags=0):
    # Independent wire packing, including CRC, rather than decoder-created dicts.
    prefix = struct.pack('<IHHIIIIHHiii', 0x31474C44, 1, 44, boot, sequence,
                         sequence * 10 if uptime is None else uptime, 0, code, flags,
                         signed(a), signed(b), signed(c))
    return prefix + struct.pack('<I', zlib.crc32(prefix))


def binary(generation, records):
    prefix = struct.pack('<7I', 0x46474C44, 1, 32, generation, 1, 0, 0)
    return prefix + struct.pack('<I', zlib.crc32(prefix)) + b''.join(records)


class ExportSemantics(unittest.TestCase):
    def setUp(self):
        OUT.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(prefix=self._testMethodName + '-', dir=OUT))

    def file(self, name, generation, records):
        path = self.directory / ('diag-' + name + '.bin')
        path.write_bytes(binary(generation, records))
        return path

    def report(self):
        value = EXPORT.load([self.directory])
        (self.directory / 'report.json').write_text(json.dumps(value, indent=2) + '\n')
        return value

    def test_old_wire_and_report_parity(self):
        rows = [record(1, 1, 1, 1, 0, 0, uptime=0),
                record(1, 2, 0x101, 0, 2, 0, uptime=1000),
                record(1, 3, 0x100, 80, 3700, 7, uptime=2000),
                record(1, 4, 0x102, 1, 80, 0, uptime=2000),
                record(1, 5, 0x101, 2, 3, 1, uptime=4000),
                record(1, 6, 0x100, 78, 3680, 7, uptime=5000),
                record(1, 7, 0x202, EXPORT.name_id('diag.marker'), 0, 0, uptime=5000),
                record(1, 8, 0x777, -3, -4, -5, uptime=5000)]
        path = self.file('old', 1, rows)
        self.assertEqual(len(path.read_bytes()), 32 + 44 * len(rows))
        header, decoded, errors = EXPORT.decode_file(path)
        self.assertTrue(header['valid_header']); self.assertEqual(errors, [])
        self.assertEqual(decoded[-1]['event'], 'UNKNOWN_1911')
        self.assertEqual([decoded[-1][key] for key in ('a', 'b', 'c')], [-3, -4, -5])
        report = self.report(); session = report['sessions'][0]
        self.assertEqual(session['power_ms'], {'active': 1000, 'aod': 0, 'display_off': 3000,
                                             'sensor_idle': 1000, 'unknown': 0})
        self.assertEqual(session['battery_saver_ms'], 3000)
        self.assertEqual(session['battery'], {'valid_battery_samples': 2, 'first_percent': 80,
                                             'last_percent': 78, 'percent_change': -2, 'sample_span_ms': 3000})
        self.assertEqual(report['events'][-2]['name'], 'diag.marker')
        self.assertEqual(report['schema']['record_bytes'], 44)
        self.assertEqual(report['schema']['file_header_bytes'], 32)
        self.assertEqual(report['transactions'], [])
        self.assertEqual(report['allocations'], [])
        self.assertFalse(report['errors'])

    def test_boot_scoped_unsigned_ids_order_context_and_retained_replay(self):
        request = 0x80000009
        start = record(9, 1, 0x210, request, (10 << 16) | 4, 7)
        done = record(9, 4, 0x211, request, 1, 1)
        self.file('early-physical', 1, [done, record(10, 2, 0x211, request, -1, -5)])
        self.file('late-physical', 2, [start, record(9, 2, 0x212, request, EXPORT.name_id('home'), 17),
                                      record(9, 3, 0x211, request, 0, 42),
                                      record(9, 4, 0x211, request, 1, 1, flags=1),
                                      record(10, 1, 0x210, request, (8 << 16) | 11, 123)])
        report = self.report(); transactions = {(t['boot'], t['request_id']): t for t in report['transactions']}
        self.assertEqual(report['duplicates_removed'], 1); self.assertFalse(report['errors'])
        self.assertEqual(len(transactions), 2)
        first, second = transactions[9, request], transactions[10, request]
        self.assertEqual((first['state'], first['acknowledged']), ('success', True))
        self.assertEqual(first['request']['domain_name'], 'gnss')
        self.assertEqual(first['request']['action_name'], 'start')
        self.assertEqual(first['terminal_result']['detail'], 1)  # Collection armed, no fix/save inference.
        self.assertEqual(first['contexts'][0]['source_known_name'], 'home')
        self.assertEqual([r['result'] for r in first['results']], [0, 1])
        self.assertEqual((second['state'], second['terminal_result']['detail']), ('failed', -5))
        replay = next(e for e in report['events'] if (e['boot'], e['sequence']) == (9, 4))
        self.assertTrue(replay['retained']); self.assertEqual(replay['copies'], 2)
        self.assertEqual(replay['a'], signed(request))

    def test_missing_request_terminal_unknown_and_signed_failure(self):
        self.file('missing', 1, [record(1, 1, 0x210, 11, (9 << 16) | 10, 1),
            record(1, 2, 0x210, 12, (9 << 16) | 5, 1), record(1, 3, 0x211, 12, 0, 14),
            record(1, 4, 0x211, 13, 1, 8), record(1, 5, 0x212, 14, 99, 2),
            record(1, 6, 0x210, 15, (8 << 16) | 11, 2), record(1, 7, 0x211, 15, 7, 1),
            record(1, 8, 0x210, 16, (8 << 16) | 11, 2), record(1, 9, 0x211, 16, -999, -21),
            record(1, 10, 0x210, 17, (1 << 16) | 1, 3), record(1, 11, 0x211, 17, 2, 0)])
        report = self.report(); transactions = {t['request_id']: t for t in report['transactions']}
        self.assertEqual([transactions[i]['state'] for i in range(11, 18)],
                         ['unresolved', 'pending', 'orphan', 'orphan', 'unknown_result', 'failed', 'cancelled'])
        self.assertFalse(any(transactions[i]['acknowledged'] for i in range(11, 16)))
        self.assertTrue(transactions[13]['terminal_observed'])
        self.assertTrue(transactions[16]['acknowledged'])
        self.assertEqual(transactions[16]['terminal_result']['result'], -999)
        self.assertEqual(report['diagnostic_summary']['issue_counts']['missing_action_terminal'], 3)

    def test_conflicting_and_repeated_terminals_and_reused_request(self):
        rows = []
        def emit(code, a, b, c): rows.append(record(1, len(rows) + 1, code, a, b, c))
        emit(0x210, 21, (1 << 16) | 1, 99); emit(0x210, 21, (1 << 16) | 1, 99); emit(0x211, 21, 1, 1)
        emit(0x210, 22, (1 << 16) | 1, 99); emit(0x211, 22, 1, 1); emit(0x211, 22, -1, -5)
        emit(0x210, 23, (1 << 16) | 1, 99); emit(0x211, 23, 1, 1); emit(0x211, 23, 0, 2)
        emit(0x210, 24, (1 << 16) | 1, 99); emit(0x211, 24, 1, 1); emit(0x211, 24, 1, 1)
        emit(0x211, 25, 1, 1); emit(0x210, 25, (1 << 16) | 1, 99)
        emit(0x210, 0, (1 << 16) | 1, 99); emit(0x211, 0, 1, 1)
        self.file('conflict', 1, rows); report = self.report()
        tx = {t['request_id']: t for t in report['transactions']}
        self.assertTrue(all(tx[i]['state'] == 'conflict' and not tx[i]['acknowledged'] for i in (0, 21, 22, 23)))
        self.assertEqual((tx[24]['state'], tx[24]['result_count']), ('success', 2))
        self.assertFalse(tx[25]['acknowledged']); self.assertEqual(tx[25]['state'], 'conflict')
        self.assertEqual(report['diagnostic_summary']['issue_counts']['terminal_before_request'], 1)

    def test_conflicting_replay_cannot_choose_success(self):
        self.file('first', 1, [record(7, 1, 0x210, 31, (9 << 16) | 10, 1), record(7, 2, 0x211, 31, 1, 1)])
        self.file('second', 2, [record(7, 2, 0x211, 31, -1, -5)])
        report = self.report(); tx = report['transactions'][0]
        self.assertEqual(tx['state'], 'conflict'); self.assertFalse(tx['acknowledged'])
        self.assertEqual(report['errors'][0]['kind'], 'duplicate_id_conflict')
        terminal = next(e for e in report['events'] if e['sequence'] == 2)
        self.assertEqual(terminal['b'], 1)  # Existing first-copy raw selection remains explicit.
        self.assertEqual(terminal['conflicting_copies'][0]['b'], -1)

    def test_conflicting_replay_variant_bound_remains_uncertain(self):
        self.file('first', 1, [record(7, 1, 0x210, 31, (9 << 16) | 10, 1), record(7, 2, 0x211, 31, 1, 1),
                               record(7, 3, 0x210, 32, (9 << 16) | 10, 2), record(7, 4, 0x211, 32, 1, 1),
                               record(8, 1, 0x210, 31, (9 << 16) | 10, 1), record(8, 2, 0x211, 31, 1, 1)])
        self.file('second', 2, [record(7, 2, 0x211, 31, -1, -5)])
        self.file('third', 3, [record(7, 2, 0x211, 32, 0, 2)])
        with patch.dict(EXPORT.ANALYSIS_LIMITS, duplicate_variants_per_record=1): report = self.report()
        tx = {(t['boot'], t['request_id']): t for t in report['transactions']}
        self.assertFalse(tx[7, 31]['acknowledged'] or tx[7, 32]['acknowledged'])
        self.assertTrue(tx[8, 31]['acknowledged'])
        event = next(e for e in report['events'] if (e['boot'], e['sequence']) == (7, 2))
        self.assertEqual(len(event['conflicting_copies']), 1)
        self.assertEqual(event['omitted_conflicting_copies'], 1)

    def test_allocator_explicit_ids_and_ordered_failure_candidate(self):
        self.file('allocation', 1, [record(1, 1, 0x108, 0x80001000, 0x80000000, 0x40001234),
            record(1, 2, 0x105, 12, 8, 10), record(1, 3, 0x109, 0xFFFFFFFF, 0x3FCCABCD, 1 | (7 << 8)),
            record(1, 4, 0x10A, 0xFFFFFFFF, 3, 0xFFFFFFF0), record(1, 5, 0x107, 128, 64, 96),
            record(2, 1, 0x108, 88, 3, 0), record(2, 2, 0x109, 0xFFFFFFFF, 77, 0),
            record(2, 3, 0x10A, 0xFFFFFFFF, 1, 23)])
        report = self.report(); self.assertEqual(len(report['allocations']), 2)
        first, second = report['allocations']
        self.assertEqual(first['capture_sequence'], 0xFFFFFFFF)
        self.assertEqual(first['status'], 'correlated_candidate')
        self.assertEqual(first['failure_correlation'], 'ordered_candidate')
        self.assertEqual(first['failure_sequence_gap'], 2)
        self.assertEqual(first['failure']['size_bytes'], 0x80001000)
        self.assertEqual(first['context']['core'], 1)
        self.assertTrue(first['context']['caller_known'] and first['context']['isr'] and first['context']['metrics_cached'])
        self.assertEqual(first['heap']['new_failure_count'], 3)
        self.assertEqual(first['heap']['acquisition_uptime_ms'], 0xFFFFFFF0)
        self.assertEqual(second['boot'], 2); self.assertEqual(second['failure']['size_bytes'], 88)
        self.assertEqual(report['events'][4]['dma_heap'], {'free_bytes': 128, 'minimum_bytes': 64, 'largest_block': 96})
        self.assertNotIn('dma_heap', first)  # Later DMA metrics are not the hook's exact headroom.

    def test_allocator_partial_lost_tags_window_and_conflicting_capture(self):
        self.file('allocation-missing', 1, [record(3, 1, 0x108, 30, 1, 99),
            record(3, 40, 0x109, 41, 10, 0), record(3, 41, 0x10A, 41, 2, 9),
            record(3, 42, 0x10A, 42, 1, 10), record(3, 43, 0x109, 42, 9, 0),
            record(3, 44, 0x109, 42, 8, 0), record(3, 45, 0x108, 70, 1, 99)])
        report = self.report(); groups = report['allocations']
        self.assertEqual(len(groups), 4)
        untagged = [g for g in groups if g['capture_sequence'] is None]
        self.assertEqual(len(untagged), 2)
        tagged = {g['capture_sequence']: g for g in groups if g['capture_sequence']}
        self.assertEqual(tagged[41]['missing'], ['failure'])
        self.assertEqual(tagged[42]['status'], 'conflict')
        self.assertEqual(tagged[42]['context_count'], 2)

    def test_allocator_bounds_and_metadata_arriving_before_context(self):
        rows = [record(1, 1, 0x108, 10, 1, 99), record(1, 2, 0x10A, 1, 1, 5), record(1, 3, 0x109, 1, 6, 0),
                record(1, 4, 0x108, 20, 1, 99), record(1, 5, 0x109, 2, 6, 0), record(1, 6, 0x10A, 2, 1, 6),
                record(1, 7, 0x108, 30, 1, 99), record(1, 8, 0x109, 3, 6, 0), record(1, 9, 0x10A, 3, 1, 7)]
        self.file('bounded-allocation', 1, rows)
        report = self.report(); self.assertEqual(len(report['allocations']), 3)
        first = report['allocations'][0]; self.assertEqual(first['failure']['size_bytes'], 10)
        self.assertEqual(first['heap']['sequence'], 2); self.assertEqual(first['context']['sequence'], 3)
        with patch.dict(EXPORT.ANALYSIS_LIMITS, allocations=2): report = self.report()
        self.assertLessEqual(len(report['allocations']), 2)
        self.assertEqual(len(report['events']), len(rows))
        self.assertGreater(report['diagnostic_summary']['allocations']['omitted_events'], 0)

    def test_retained_fault_summary_crash_display_raw_values(self):
        self.file('faults', 1, [record(2, 1, 5, 0, 3, 0x108), record(2, 2, 5, 0, 7, 0x108, flags=1),
            record(2, 3, 0x10B, 0xDEADBEEF, 32768, 0x3FCC1234),
            record(2, 4, 0x10C, 0x80001234, 0x3FCDAAAA, 28),
            record(2, 5, 0x10E, 5, -7, 0x0201), record(2, 6, 0x10E, 0x100, 31, 20),
            record(2, 7, 0x10E, 99, 9, 0)])
        report = self.report(); events = report['events']
        self.assertEqual([e['fault_summary']['occurrences'] for e in events[:2]], [3, 7])
        self.assertEqual(events[0]['fault_summary']['original_event'], 'ALLOC_FAILURE')
        self.assertNotIn('original_boot', events[0]['fault_summary'])
        self.assertEqual(events[2]['crash_artifact']['elf_hash_token'], 0xDEADBEEF)
        self.assertEqual(events[2]['a'], signed(0xDEADBEEF))
        self.assertEqual(events[3]['crash_pc']['pc_hex'], '80001234')
        self.assertEqual(events[4]['display'], {'kind': 'fault', 'phase': 5, 'phase_name': 'panel_command',
                                              'error': -7, 'packed_credits': 0x0201, 'dma_credits': 1, 'stripe_credits': 2})
        self.assertEqual(events[5]['display'], {'kind': 'command_ack', 'channel': 0, 'channel_name': 'brightness',
                                              'requested': 31, 'applied': 20})
        self.assertEqual(events[6]['display']['phase_name'], 'unknown')

    def test_gnss_rejection_aggregate_is_numeric_and_age_unknown(self):
        self.file('gnss', 1, [record(1, 1, 0x321, (10 << 16) | 0x806D, 0x7FFFFFFF, 0x7FFFFFFF),
                              record(1, 2, 0x321, (10 << 16) | 16, 3, 2500),
                              record(1, 3, 0x321, (12 << 16) | 16, 2, 4)])
        events = self.report()['events']; value = events[0]['gnss_quality']
        self.assertEqual(value['rejected_count'], 0x7FFFFFFF)
        self.assertEqual(value['unknown_reason_bits'], 0x8000)
        self.assertEqual(value['reasons'], ['unavailable', 'stale', 'invalid_range_or_nonfinite', 'outlier', 'unstable_reacquisition'])
        self.assertTrue(value['age_unknown']); self.assertIsNone(value['age_ms'])
        self.assertEqual(events[1]['gnss_quality']['age_ms'], 2500)
        self.assertNotIn('gnss_quality', events[2])
        self.assertFalse(any(key in value for key in ('lat', 'lon', 'location', 'body', 'name')))

    def test_shutdown_policy_variants_never_count_as_saver_or_power_transitions(self):
        self.file('shutdown', 1, [record(1, 1, 1, 1, uptime=0), record(1, 2, 0x102, 1, 60, 0, uptime=1000),
            record(1, 3, 0x102, 0x100, 2, 0, uptime=2000), record(1, 4, 0x102, 0, 60, 0, uptime=4000),
            record(1, 5, 0x102, 0x101, 2, 2, uptime=5000), record(1, 6, 0x102, 0x102, 2, 1, uptime=6000),
            record(1, 7, 0x100, 60, 3700, 7, uptime=8000)])
        report = self.report(); session = report['sessions'][0]
        self.assertEqual(session['battery_saver_ms'], 3000)
        self.assertEqual(session['power_ms']['unknown'], 8000)
        self.assertEqual(sum(session['power_ms'][name] for name in ('active', 'aod', 'display_off', 'sensor_idle')), 0)
        self.assertEqual(report['events'][2]['power_policy']['kind'], 'shutdown_intent')
        self.assertEqual(report['events'][4]['power_policy']['kind'], 'shutdown_parked_commit')
        self.assertEqual(report['events'][5]['power_policy'], {'kind': 'developer_recovery_restart', 'shutdown_mode': 2, 'shutdown_phase': 1})

    def test_bounded_details_keep_terminal_and_raw_evidence(self):
        rows = [record(1, 1, 0x210, 1, (9 << 16) | 10, 1)]
        for sequence in range(2, 24):
            rows.append(record(1, sequence, 0x212 if sequence % 2 else 0x211, 1, 0, sequence))
        rows.extend([record(1, 24, 0x211, 1, 1, 77), record(1, 25, 0x211, 2, 1, 1),
                     record(1, 26, 0x211, 3, 1, 1)])
        self.file('bounded', 1, rows)
        with patch.dict(EXPORT.ANALYSIS_LIMITS, transactions=2, details_per_transaction=3, issues=1):
            report = self.report()
        self.assertEqual(len(report['events']), len(rows))
        tx = report['transactions'][0]
        self.assertEqual(len(tx['results']), 3); self.assertEqual(len(tx['contexts']), 3)
        self.assertGreater(tx['omitted_results'], 0); self.assertGreater(tx['omitted_contexts'], 0)
        self.assertEqual(tx['terminal_result']['detail'], 77); self.assertTrue(tx['acknowledged'])
        self.assertEqual(report['diagnostic_summary']['transactions']['omitted_events'], 1)
        self.assertEqual(len(report['semantic_issues']), 1)
        self.assertGreater(report['diagnostic_summary']['omitted_issues'], 0)

    def test_crc_tail_cli_json_csv_and_no_semantic_strict_failure(self):
        path = self.file('cli', 1, [record(1, 1, 0x211, 77, -1, -5)])
        output, csv_path = self.directory / 'trace.json', self.directory / 'trace.csv'
        argv = [str(path), '--json', str(output), '--csv', str(csv_path), '--strict']
        with contextlib.redirect_stdout(io.StringIO()) as stdout:
            self.assertEqual(EXPORT.main(argv), 0)  # Missing request is evidence loss, not corrupt bytes.
        parsed = json.loads(output.read_text()); self.assertEqual(parsed['transactions'][0]['state'], 'orphan')
        self.assertIn('diagnostic_summary', json.loads(stdout.getvalue()))
        with csv_path.open(newline='') as csv_file:
            rows = list(csv.DictReader(csv_file))
        self.assertEqual(list(rows[0]), ['boot', 'sequence', 'uptime_ms', 'epoch', 'event', 'a', 'b', 'c',
                                      'name', 'retained', 'copies', 'file', 'offset'])
        self.assertEqual(rows[0]['event'], 'ACTION_RESULT'); self.assertEqual(rows[0]['b'], '-1')
        damaged = bytearray(path.read_bytes()); damaged[32 + 20] ^= 1; damaged.extend(b'bad'); path.write_bytes(damaged)
        report = self.report(); self.assertEqual([e['kind'] for e in report['errors']], ['record_crc_mismatch', 'truncated_tail'])
        self.assertEqual(report['transactions'], [])
        with contextlib.redirect_stdout(io.StringIO()): self.assertEqual(EXPORT.main(argv), 2)

    @unittest.skipUnless(shutil.which('gcc'), 'GCC required for actual producer wire fixture')
    def test_actual_c_semantic_producer_binary(self):
        executable = self.directory / 'producer.exe'
        env = os.environ | {key: str(self.directory) for key in ('TEMP', 'TMP', 'TMPDIR')}
        command = ['gcc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                   '-I' + str(ROOT / 'core'), '-I' + str(ROOT / 'nos'),
                   str(ROOT / 'tests/diag_log_test.c'), '-o', str(executable)]
        compiled = subprocess.run(command, capture_output=True, text=True, env=env, timeout=60)
        (self.directory / 'producer-compile.log').write_text(compiled.stdout + compiled.stderr)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        result = subprocess.run([str(executable), 'semantic', str(self.directory / 'trace')],
                                capture_output=True, text=True, env=env, timeout=30)
        (self.directory / 'producer.log').write_text(result.stdout + result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = self.report(); self.assertFalse(report['errors'])
        tx = report['transactions'][0]
        self.assertEqual(tx['request']['domain_name'], 'notification')
        self.assertEqual(tx['request']['action_name'], 'dismiss')
        self.assertEqual(tx['request']['target'], 721)
        self.assertEqual(tx['contexts'][0]['value'], 17)
        self.assertEqual((tx['state'], tx['terminal_result']['detail']), ('success', 721))


if __name__ == '__main__':
    unittest.main()
