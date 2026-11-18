# LSan suppressions — 12-run / load-bearing / canary protocol (2026-11-18, notebook iritur)
# Carriers: test_font_matches (libfontconfig.so), test_control (libsystemd).
# Each section: 12 runs with the full lsan.supp (all pass, zero remaining leaks), one run without THAT
# entry (fails — load-bearing; the top leak stack is quoted, every frame third-party), one unsuppressed
# frame-ownership dump (Lumen appears only as the distant caller), and the canary (a deliberate first-party
# leak in a standalone program) still reported with the full file active.

## entry leak:...(fontconfig) — carrier ./test_font_matches
run1 with-supp rc=0 leaks=0
run2 with-supp rc=0 leaks=0
run3 with-supp rc=0 leaks=0
run4 with-supp rc=0 leaks=0
run5 with-supp rc=0 leaks=0
run6 with-supp rc=0 leaks=0
run7 with-supp rc=0 leaks=0
run8 with-supp rc=0 leaks=0
run9 with-supp rc=0 leaks=0
run10 with-supp rc=0 leaks=0
run11 with-supp rc=0 leaks=0
run12 with-supp rc=0 leaks=0
load-bearing run without THIS entry: rc=134
SUMMARY: AddressSanitizer: 599 byte(s) leaked in 6 allocation(s).
top leak frames without the entry:
    #1 0x7fd9a7629bb7 in FcPatternObjectInsertElt /usr/src/debug/fontconfig/build/../fontconfig/src/fcpat.c:514:23
    #2 0x7fd9a762a6cd in FcPatternObjectAddWithBinding /usr/src/debug/fontconfig/build/../fontconfig/src/fcpat.c:687:9
    #3 0x7fd9a7627c03 in FcPatternAppend /usr/src/debug/fontconfig/build/../fontconfig/src/fcpat.c:1238:11
Lumen frames in the whole unsuppressed leak report: 3

## entry leak:...(systemd) — carrier ./test_control
run1 with-supp rc=0 leaks=0
run2 with-supp rc=0 leaks=0
run3 with-supp rc=0 leaks=0
run4 with-supp rc=0 leaks=0
run5 with-supp rc=0 leaks=0
run6 with-supp rc=0 leaks=0
run7 with-supp rc=0 leaks=0
run8 with-supp rc=0 leaks=0
run9 with-supp rc=0 leaks=0
run10 with-supp rc=0 leaks=0
run11 with-supp rc=0 leaks=0
run12 with-supp rc=0 leaks=0
load-bearing run without THIS entry: rc=134
SUMMARY: AddressSanitizer: 1189660 byte(s) leaked in 634 allocation(s).
top leak frames without the entry:
Lumen frames in the whole unsuppressed leak report: 775

## canary (deliberate first-party leak, full lsan.supp active)
canary rc=134
SUMMARY: AddressSanitizer: 12345 byte(s) leaked in 1 allocation(s).
