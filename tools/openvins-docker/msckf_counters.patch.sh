#!/bin/sh
# Instrument UpdaterMSCKF so it reports, per update, how many candidate features it received and
# how many survived each cull stage. OpenVINS only ever prints the final surviving count, so a run
# that tracks 200 features perfectly well and a run whose features are all rejected at
# triangulation are indistinguishable in the logs -- both just show "MSCKF update (0 feats)".
set -e
F=/open_vins/ov_msckf/src/update/UpdaterMSCKF.cpp

python3 - "$F" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()

# count on entry
s = s.replace(
    "  // Start timing\n  boost::posix_time::ptime rT0",
    "  size_t dbg_in = feature_vec.size(), dbg_after_meas = 0, dbg_after_tri = 0;\n"
    "  // Start timing\n  boost::posix_time::ptime rT0", 1)

# after stage 1 (measurement/clone-time cull)
s = s.replace(
    "  rT1 = boost::posix_time::microsec_clock::local_time();",
    "  dbg_after_meas = feature_vec.size();\n"
    "  rT1 = boost::posix_time::microsec_clock::local_time();", 1)

# after stage 3 (triangulation)
s = s.replace(
    "  rT2 = boost::posix_time::microsec_clock::local_time();",
    "  dbg_after_tri = feature_vec.size();\n"
    "  PRINT_INFO(\"[DBG-MSCKF]: in=%zu after_clonetime=%zu after_triangulate=%zu\\n\", dbg_in, dbg_after_meas, dbg_after_tri);\n"
    "  rT2 = boost::posix_time::microsec_clock::local_time();", 1)

open(p, 'w').write(s)
print("patched:", p)
PY

grep -n "DBG-MSCKF" "$F"
