"""Exact launcher curve reuse, duplicate work and same-frame invalidation."""
from pathlib import Path
import hashlib
import json
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from render_private_build import AVAILABLE, link, run, workspace


def probe(source, out, candidate):
    # Instrument private actual-source copies only. The direct reference uses
    # the unchanged authored curve for every lookup, as before publication reuse.
    preamble = '''
static unsigned curve_sqrt_calls,curve_trail_calls;
static int curve_reference_direct;
static float curve_sqrt(float value){curve_sqrt_calls++;return sqrtf(value);}
#define sqrtf curve_sqrt
'''
    source = source.replace('#define ROW_P', preamble + '\n#define ROW_P', 1)
    needle = 'static int launcher_trail_x(int cy,int level) {'
    assert source.count(needle) == 1
    source = source.replace(needle, needle + '\n    curve_trail_calls++;', 1)
    if candidate:
        needle = 'static void geometry_trail_get(int row,float xr,int cy,int low,int high,int *xa,int *xb)\n{'
        assert source.count(needle) == 1
        source = source.replace(needle, needle + '''
    if(curve_reference_direct){*xa=launcher_trail_x(cy,low);*xb=launcher_trail_x(cy,high);return;}
''', 1)
    include = (ROOT / 'tests/launcher_curve_reuse_test.inc').as_posix()
    source = '#include <assert.h>\n#define CURVE_REUSE_CANDIDATE ' + str(int(candidate)) + '\n' + source
    source += '\n#include "' + include + '"\n'
    path = out / 'probe.c'
    path.write_text(source)
    binary = out / 'probe.exe'
    link(path, binary, exclude={'nos/apps/sys_launcher.c'})
    result = run(binary, out, timeout=120)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    records = [json.loads(line[6:]) for line in result.stdout.splitlines() if line.startswith('CURVE ')]
    assert len(records) == 157, len(records)
    return records, result.stdout


@unittest.skipUnless(AVAILABLE, 'Windows and gcc required')
class LauncherCurveReuseTests(unittest.TestCase):
    def test_exact_curve_work_and_same_frame_changes(self):
        source = ROOT / 'nos/apps/sys_launcher.c'
        before = hashlib.sha256(source.read_bytes()).hexdigest()
        out = workspace('launcher-curve-reuse')
        rows, log = probe(source.read_text(), out, True)
        self.assertIn('8 within-frame invalidations', log)
        self.assertEqual(rows[0]['old_sqrt'], 152)
        self.assertEqual(rows[0]['draw_sqrt'], 0)
        self.assertIn('WORK preparation_sqrt=20', log)
        self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), before)
        (out / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')


if __name__ == '__main__':
    unittest.main()
