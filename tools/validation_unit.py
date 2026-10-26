"""Run exactly one named unittest source and emit structured counts/skips."""
import argparse
import importlib.util
import json
from pathlib import Path
import sys
import unittest

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('file', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(root))
    spec = importlib.util.spec_from_file_location('validation_' + args.file.stem, args.file)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    suite = unittest.defaultTestLoader.loadTestsFromModule(module)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    passed = result.wasSuccessful() and result.testsRun > 0
    args.report.write_text(json.dumps({'status': 'passed' if passed else 'failed',
        'tests_run': result.testsRun, 'failures': len(result.failures), 'errors': len(result.errors),
        'skips': [{'test': str(test), 'reason': reason} for test, reason in result.skipped]}, indent=2) + '\n', encoding='utf-8')
    return 0 if passed else 1

if __name__ == '__main__': raise SystemExit(main())
