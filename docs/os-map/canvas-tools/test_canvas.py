"""Regression checks: python docs/os-map/canvas-tools/test_canvas.py.

Temporary fixtures stay inside canvas-tools. No canonical input is modified.
"""
import copy
import json
from pathlib import Path
import tempfile
import unittest

import build_canvas as b


class CanvasTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.graph, cls.gaps, cls.available, _, _ = b.load_inputs()

    def test_deterministic_artifacts(self):
        first, _, _ = b.artifacts()
        second, _, _ = b.artifacts()
        self.assertEqual(first, second)

    def test_every_component_context_and_store_is_retained(self):
        canvases = b.build(self.graph, [], False)
        overview, _, schedule, _ = canvases[:4]
        self.assertEqual({n['id'] for n in self.graph['nodes']},
                         {m['source'] for m in overview.meta.values() if 'source' in m})
        self.assertEqual(len(self.graph['schedule']),
                         sum('schedule' in m for m in schedule.meta.values()))
        self.assertEqual({n['id'] for n in self.graph['nodes'] if n['lane'] == 'stores'},
                         {m['source'] for m in schedule.meta.values() if 'source' in m})

    def test_all_flow_steps_and_chain_edges(self):
        flow = b.build(self.graph, [], False)[1]
        self.assertEqual(sum(len(f['steps']) for f in self.graph['flows']),
                         sum('action' in m for m in flow.meta.values()))
        self.assertEqual(sum(max(0, len(f['steps']) - 1) for f in self.graph['flows']), len(flow.edges))

    def test_absent_empty_and_wrapped_inputs(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as directory:
            root = Path(directory)
            (root / 'data').mkdir()
            graph, gaps, available, warnings, _ = b.load_inputs(root)
            self.assertFalse(available)
            self.assertEqual(len(warnings), 2)
            for c in b.build(graph, gaps, available):
                self.assertEqual(b.validate(c.data(), c.parents), [])
            (root / 'data/graph.json').write_text(json.dumps(self.graph), encoding='utf-8')
            for value, expected_available in [('', False), ('[]', True), ('{}', True), ('{"gaps": []}', True)]:
                (root / 'data/gaps.json').write_text(value, encoding='utf-8')
                _, gaps, available, _, _ = b.load_inputs(root)
                self.assertEqual(gaps, [])
                self.assertEqual(available, expected_available)
            (root / 'data/gaps.json').write_text('{broken', encoding='utf-8')
            with self.assertRaises(ValueError):
                b.load_inputs(root)

    def test_realistic_gaps_and_unknown_component_are_visible(self):
        target = self.graph['flows'][0]['steps'][0]['node']
        gaps = [{'id': 'test-gap', 'title': 'Example verified finding', 'severity': 'critical',
                 'area': 'Health', 'nodes': [target, 'unknown-component'],
                 'evidence': [{'file': 'example.c', 'line': 20}], 'user_impact': 'Impact ' * 60,
                 'suggested_fix': 'Preserve the full evidence.', 'votes': {'verified': 3}}]
        canvases = b.build(self.graph, gaps, True)
        for c in canvases:
            self.assertEqual(b.validate(c.data(), c.parents), [])
        self.assertTrue(any('1 gap' in n.get('text', '') for n in canvases[0].nodes))
        highlighted = [n for n in canvases[1].nodes if canvases[1].meta[n['id']].get('source') == target]
        self.assertTrue(highlighted)
        self.assertTrue(all(n.get('color') == '1' for n in highlighted))
        self.assertEqual(len(canvases[3].edges), 2)
        self.assertTrue(any('UNRESOLVED' in n.get('text', '') for n in canvases[3].nodes))
        self.assertTrue(any('Unknown affected component' in s for s in b.audit(self.graph, gaps)))

    def test_validator_rejects_layout_and_reference_corruption(self):
        c = b.Canvas('Fixture')
        c.group('group', 'Area', 0, 0, 1100, 600)
        c.card('a', '**Alpha**', 48, 100, parent='group')
        c.card('b', '**Beta**', 504, 100, parent='group')
        c.edge('a', 'b')
        self.assertEqual(b.validate(c.data(), c.parents), [])
        for mutation, expected in [
            (lambda d: d['nodes'][2].update(id='a'), 'unique'),
            (lambda d: d['edges'][0].update(toNode='missing'), 'Dangling'),
            (lambda d: d['nodes'][2].update(x=60), 'Overlapping'),
            (lambda d: d['nodes'][2].update(x=470), 'gutter'),
            (lambda d: d['nodes'][2].update(y=700), 'outside'),
            (lambda d: d['nodes'][2].update(text='Very long content ' * 300), 'capacity'),
        ]:
            data = copy.deepcopy(c.data())
            mutation(data)
            self.assertTrue(any(expected in error for error in b.validate(data, c.parents)), expected)

    def test_missing_lanes_and_dangling_edges_are_reported(self):
        graph = copy.deepcopy(self.graph)
        graph['nodes'][0]['lane'] = 'not-a-real-lane'
        graph['edges'].append({'from': 'missing', 'to': graph['nodes'][0]['id'], 'label': 'test', 'kind': 'data'})
        issues = b.audit(graph, [])
        self.assertTrue(any('Unknown/missing lane' in s for s in issues))
        self.assertTrue(any('Dangling graph edge' in s for s in issues))
        c = b.build(graph, [], False)[0]
        self.assertEqual(b.validate(c.data(), c.parents), [])
        self.assertIn('lane-unassigned', {n['id'] for n in c.nodes})

    def test_embedded_json_cannot_close_script(self):
        graph = copy.deepcopy(self.graph)
        graph['nodes'][0]['summary'] = '</script><script>alert(1)</script>'
        page = b.render_html(b.build(graph, [], False), graph, [], False, [])
        self.assertNotIn('</script><script>alert(1)', page)
        self.assertIn('\\u003c/script>', page)

    def test_overview_spine_is_real_deduplicated_and_covers_apps(self):
        canvas = b.build(self.graph, self.gaps, self.available)[0]
        canonical = {(e['from'], e['to']) for e in self.graph['edges']}
        visible = {(e['fromNode'], e['toNode']) for e in canvas.edges}
        self.assertTrue(90 <= len(canvas.edges) <= 140)
        self.assertEqual(len(visible), len(canvas.edges))
        self.assertTrue(visible <= canonical)
        lookup = {n['id']: n for n in self.graph['nodes']}
        for a, z in canonical:
            la, lz = lookup[a]['lane'], lookup[z]['lane']
            if ((la in ('apps', 'faces') and lz == 'services') or
                (lz in ('apps', 'faces') and la == 'services') or
                (la == 'services' and lz == 'stores') or
                (la in ('drivers', 'tasks') and lz == 'services')):
                self.assertIn((a, z), visible)
        touched = {i for pair in visible for i in pair}
        for n in self.graph['nodes']:
            if n['lane'] in ('apps', 'faces') and any(n['id'] in p for p in canonical):
                self.assertIn(n['id'], touched)
        for edge in canvas.edges:
            self.assertIn(edge['fromSide'], ('top', 'bottom'))
            self.assertIn(edge['toSide'], ('top', 'bottom'))
        self.assertIn(('svc-activity', 'app-energy'), visible)
        missing = next(e for e in canvas.edges if (e['fromNode'], e['toNode']) == ('svc-workout', 'app-energy'))
        self.assertIn('MISSING', missing['label'])

    def test_focused_canvases_keep_all_incident_edges_and_exact_one_hop(self):
        canvases = b.build(self.graph, self.gaps, self.available)[4:]
        self.assertEqual(tuple(c.title for c in canvases), b.SYSTEMS)
        covered_nodes, covered_edges = set(), set()
        for canvas in canvases:
            members = b.system_members(self.graph, canvas.title)
            expected = [e for e in self.graph['edges'] if e['from'] in members or e['to'] in members]
            actual = [(e['fromNode'], e['toNode'], e['label']) for e in canvas.edges]
            self.assertCountEqual(actual, [(e['from'], e['to'], f"{e['kind']}: {e['label']}") for e in expected])
            included = members | {i for e in expected for i in (e['from'], e['to'])}
            self.assertEqual(included, {m['source'] for m in canvas.meta.values() if 'source' in m})
            for m in canvas.meta.values():
                if 'source' in m:
                    self.assertEqual(m['ghost'], m['source'] not in members)
            self.assertEqual(b.validate(canvas.data(), canvas.parents), [])
            covered_nodes |= members
            covered_edges.update(actual)
        self.assertEqual(covered_nodes, {n['id'] for n in self.graph['nodes']})
        self.assertEqual(covered_edges, {(e['from'], e['to'], f"{e['kind']}: {e['label']}") for e in self.graph['edges']})

    def test_severe_gap_borders_on_every_component_copy_and_context(self):
        target = 'svc-workout'
        gap = dict(id='fixture-high', title='High impact', severity='high', nodes=[target])
        for canvas in b.build(self.graph, [gap], True):
            for n in canvas.nodes:
                m = canvas.meta[n['id']]
                if m.get('source') == target or target in m.get('sources', []):
                    self.assertEqual(n.get('color'), '1', (canvas.title, n['id']))
                    self.assertIn('gap', n['text'])

    def test_gap_renames_preserve_evidence_and_unknowns(self):
        raw = dict(id='fixture', severity='high', nodes=['store-prefs', 'store-sd', 'svc-not-a-node'])
        resolved = b.resolve_gaps([raw], {n['id']: n for n in self.graph['nodes']})[0]
        self.assertEqual(raw['nodes'], ['store-prefs', 'store-sd', 'svc-not-a-node'])
        self.assertEqual(resolved['nodes'], ['store-nos-prefs', 'drv-sd', 'svc-not-a-node'])
        self.assertEqual(resolved['original_nodes'], raw['nodes'])
        self.assertEqual(b.resolve_gaps([resolved], {n['id']: n for n in self.graph['nodes']}), [resolved])

    def test_system_files_index_tabs_and_today_group(self):
        output, canvases, _ = b.artifacts()
        self.assertEqual(len([p for p in output if p.suffix == '.canvas']), 14)
        overview_text = '\n'.join(n.get('text', '') for n in canvases[0].nodes)
        for title in b.SYSTEMS:
            self.assertIn(b.ROOT / 'canvases' / (title + '.canvas'), output)
            self.assertIn(f'[[canvases/{title}.canvas|', overview_text)
        page = output[b.ROOT / 'preview/system_map.html']
        payload = json.loads(page.split('<script id="atlas-data" type="application/json">')[1].split('</script>')[0])
        self.assertEqual(set(payload['canvases']), set(b.TITLES + b.SYSTEMS))
        groups = [n for n in canvases[3].nodes if n['type'] == 'group']
        self.assertEqual(min(groups, key=lambda n: (n['y'], n['x']))['label'], 'The three things the user hit today')


if __name__ == '__main__':
    unittest.main(verbosity=2)
