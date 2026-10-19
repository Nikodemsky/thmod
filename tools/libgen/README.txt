thmod: import libraries for the game's DLLs

lib/storm.lib    - import library for Strm2.dll (Storm 1.x, exports by ORDINAL only)
lib/smackw32.lib - import library for SmackW32.DLL (linked but unused by TH4.exe)

Both contain only the DLL name plus function names/ordinals - no Blizzard/RAD code.
The DLLs themselves are NOT in the repo; copy Strm2.dll, Standard.sn2 and
SmackW32.DLL from your TH4 game folder into lib/ (they are git-ignored).

storm.def is generated from the ordinal comments in src/storm.h:
    python gen_storm_def.py ../../src/storm.h > storm.def

Regenerate the .lib files (either tool works):
    lib.exe /def:storm.def /machine:x86 /out:../../lib/storm.lib          (VS Developer Prompt)
    llvm-dlltool -m i386 -d storm.def -l ../../lib/storm.lib               (LLVM)
    (same for smackw32.def -> smackw32.lib)

If you add a new Storm function to src/storm.h, keep the "// NNN" ordinal comment
and regenerate - otherwise the linker reports an unresolved _eSXxx@N symbol.
