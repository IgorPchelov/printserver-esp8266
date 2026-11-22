#!/bin/sh
# Host unit tests (no hardware needed)
#   test_pwg   - PWG-raster -> PCL converter (PwgToPcl.h)
#   test_queue - spool queue logic (PrintQueue.cpp) on a REAL littlefs
#                in RAM (host_littlefs/ shim, littlefs v2.9 vendored)
#   test_scroll- OLED ping-pong scroll trajectory (StatusDisplay.cpp linked
#                against the host_syntax stubs)
#   syntax     - g++ -fsyntax-only of every firmware module via stubs
#                (host_syntax/), so .ino edits are compile-checked too
cd "$(dirname "$0")"
rc=0

echo "== PWG decoder tests =="
g++ -std=c++17 -w -I. -Iarduino_shim -o test_pwg test_pwg.cpp && ./test_pwg || rc=1

echo "== PrintQueue tests (real littlefs in RAM) =="
g++ -std=c++17 -w -I. -Ihost_littlefs -o test_queue \
    test_queue.cpp ../PrintQueue.cpp host_littlefs/glue.cpp \
    -x c host_littlefs/littlefs/lfs.c \
    -x c host_littlefs/littlefs/lfs_util.c -x none && ./test_queue 2>&1 \
    | grep -v "littlefs/lfs.c:.*error:" || rc=1

echo "== OLED scroll trajectory tests =="
g++ -std=c++17 -w -I. -I.. -Ihost_syntax/stubs -o test_scroll \
    test_scroll.cpp ../StatusDisplay.cpp ../ParallelPort.cpp \
    ../NetService.cpp host_syntax/stubs/stub_globals.cpp && \
    ./test_scroll || rc=1

echo "== firmware syntax check =="
for f in printserver.ino ParallelPort.cpp PrintQueue.cpp NetService.cpp \
         StatusDisplay.cpp StatusLed.cpp; do
  g++ -std=c++17 -fsyntax-only -I host_syntax/stubs -x c++ "../$f" || rc=1
done
echo "syntax check done (no output above = clean)"

exit $rc
