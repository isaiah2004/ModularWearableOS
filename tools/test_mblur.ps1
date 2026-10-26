$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force "$root/build/tests" | Out-Null
& gcc -std=c99 -O2 -Wall -Wextra -Werror "-I$root/core" "-I$root/nos" `
    "$root/tests/mblur_test.c" "$root/nos/mblur.c" -o "$root/build/tests/mblur_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Blur kernel test compilation failed' }
& "$root/build/tests/mblur_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Blur kernel test failed' }
& gcc -std=c99 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-format-truncation `
    -flto -fwhole-program -ffunction-sections -fdata-sections "-I$root/core" "-I$root/nos" "-I$root/nos/fonts" `
    "$root/tests/mblur_gate_test.c" '-Wl,--gc-sections' -lm -o "$root/build/tests/mblur_gate_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Blur gate test compilation failed' }
& "$root/build/tests/mblur_gate_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Blur gate test failed' }
& gcc -std=c99 -O2 -Wall -Wextra -Werror "-I$root/core" "-I$root/nos" `
    "$root/tests/pblur_test.c" "$root/nos/mblur.c" "$root/core/sprite.c" -o "$root/build/tests/pblur_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Page blur test compilation failed' }
& "$root/build/tests/pblur_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Page blur test failed' }
foreach ($allocation in 1, 2, 3) {
    & "$root/build/tests/pblur_test.exe" $allocation
    if ($LASTEXITCODE -ne 0) { throw 'Page blur allocation fallback failed' }
}
& gcc -std=c99 -O2 -Wall -Wextra -Werror -flto -fwhole-program `
    "-I$root/core" "-I$root/nos" "-I$root/nos/fonts" `
    "$root/tests/kblur_slice_test.c" -o "$root/build/tests/kblur_slice_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Notification blur slice compilation failed' }
& "$root/build/tests/kblur_slice_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Notification blur slice test failed' }
& gcc -std=c99 -O2 -Wall -Wextra -Werror -flto -fwhole-program "-I$root/core" "-I$root/nos" `
    "$root/tests/psram_test.c" "$root/nos/psram.c" -o "$root/build/tests/psram_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'PSRAM allocator test compilation failed' }
& "$root/build/tests/psram_test.exe"
if ($LASTEXITCODE -ne 0) { throw 'PSRAM allocator test failed' }
