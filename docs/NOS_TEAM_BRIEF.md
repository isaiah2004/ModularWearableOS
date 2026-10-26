# NOS team brief (read fully before writing code)

You are one of several teams building NOS, a Nothing-inspired watch OS, in
`A:\makerspace\smartwatch2.0` on branch `nothing-os`. Never switch branches, never
commit, never run `git checkout/reset/stash` — the lead commits.

## Read first
1. `docs/NOS_DESIGN.md` — the spec: visual rules, motion rules, navigation map,
   performance budget, conventions, polish checklist. Follow it exactly.
2. `docs/research/NOTHING_DESIGN_REFERENCE.md` — the visual language with sources
   (section 4 has face layouts, 3 widget language, 7 the signature moves).
3. Headers: `nos/nos.h` (screens, input, transitions), `nos/ui.h` (tokens and
   components), `nos/dot.h` (dot-matrix type/icons/rings), `nos/motion.h`
   (springs, scrolling, curves), `nos/svc.h` (data services), `nos/hal_ext.h`
   (strap modules), `nos/prefs.h`, `nos/apps/nos_apps.h`, `nos/faces/face.h`.
   Skim `nos/ui.c` and `nos/nos.c` to see how components and the engine behave.
4. `docs/NOS_SIM.md` (written by the tools team; may appear a bit after you start —
   check again later).

## Ownership
- Edit ONLY the files your prompt assigns. Many teams compile the same tree at the
  same time; your files must compile at every moment (make small edits; compile
  single files with gcc `-c` before big saves). If someone else's file breaks the
  build, wait a few minutes and retry; don't fix their file.
- Need a change in a shared file (nos.h, ui.h/.c, dot.h/.c, svc.h, motion.h, nos.c,
  apps registry)? Don't edit it. Work around it locally (a static helper in your
  file) and list the request in your final report.
- Data comes from `svc.h` (a services team is implementing it; until then the stub
  returns zeros) and `hal_ext.h` (the simulator returns synthetic module data).
  Design both empty and populated states.

## Build and look
- Compile check: `gcc -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -Icore -Inos -c FILE -o build/tmp_<team>.o`
  (gcc: `C:\Users\isaia\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe`;
  add `-DNOS_FONT_SHIM` if `nos/fonts/nos_fonts.h` doesn't exist yet).
- Simulator: `sim/nos_build.ps1` -> `build/nos_sim.exe`. To avoid clobbering others,
  build your own copy: `sim/nos_build.ps1 -OutputName nos_sim_<team>.exe` if the script
  supports it (else copy the exe after building). Scripted runs:
  `build/nos_sim_<team>.exe --script build/nos/<team>/x.txt --out build/nos/<team>/x/`
  with commands like `open heart`, `wait 600`, `tap x y`, `drag ...`, `shot name`,
  `film name frames`, `gif name frames` (see docs/NOS_SIM.md). Put all your outputs
  under `build/nos/<team>/`.
- LOOK at your screenshots/films with the Read tool. You are the designer: judge
  them against the polish checklist like an Apple design review. Fix, re-shoot,
  repeat. Minimum three full review passes per screen; keep going while anything
  can be better. Check `frames.csv` from the cost model: p95 est_frame_ms < 30 in
  motion, zero late frames idle. Run the damage check on your screens.

## Quality bar
- Every screen: designed populated, empty, loading and module-absent states.
- Entry: a subtle build-in (dot build or ≤2-frame stagger), ≤ 400 ms.
- Every touch target ≥ 88 px, with a pressed state and `nos_feedback`.
- No placeholder text, no "TODO" visible, no debug output.
- Exact pixel alignment: centre things on UI_CX/UI_CY; check edges against the
  bezel with `ui_row_span`.

## Final report (your last message)
Files written; per screen: states covered, screenshot paths, cost-model p95/max;
API change requests for shared files; known gaps. Be concise.
