import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class HeapDiagnosticsTests(unittest.TestCase):
    def test_allocator_failure_and_concurrent_snapshots(self):
        with tempfile.TemporaryDirectory(prefix="nos-heap-") as tmp:
            path = Path(tmp)
            (path / "esp_attr.h").write_text("#define IRAM_ATTR\n#define DRAM_ATTR\n", encoding="utf-8")
            (path / "esp_heap_caps.h").write_text("""
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_DMA 8
#define MALLOC_CAP_INTERNAL 2048
#define MALLOC_CAP_8BIT 4
typedef void (*esp_alloc_failed_hook_t)(size_t,uint32_t,const char *);
int heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t);
size_t heap_caps_get_free_size(uint32_t);
size_t heap_caps_get_largest_free_block(uint32_t);
size_t heap_caps_get_minimum_free_size(uint32_t);
""", encoding="utf-8")
            binary = path / ("heap.exe" if os.name == "nt" else "heap")
            result = subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                                     "-pthread", "-I" + str(path), "tests/nos_heap_diag_test.c",
                                     "-o", str(binary)], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("PASS heap failure capture", result.stdout)


if __name__ == "__main__":
    unittest.main()
