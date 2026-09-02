# Insert a per-frame diagnostic into KeypointVioEstimator::measure(). The stock build gives no
# visibility at all: on a failing dataset you get one "SO3::exp failed" and nothing else. This
# prints, for every frame, how many optical-flow observations arrived per camera and how many of
# them connected to existing landmarks -- which distinguishes "front end produced nothing" from
# "front end fine, estimator diverged".
import re, sys
p = '/basalt/src/vi_estimator/keypoint_vio.cpp'
s = open(p).read()
anchor = '  marginalize(num_points_connected);'
assert anchor in s, "anchor not found"
dbg = '''  {
    size_t o0 = opt_flow_meas->observations.size() > 0 ? opt_flow_meas->observations[0].size() : 0;
    size_t o1 = opt_flow_meas->observations.size() > 1 ? opt_flow_meas->observations[1].size() : 0;
    int conn = 0;
    for (const auto& kv : num_points_connected) conn += kv.second;
    std::cout << "DBG t_ns=" << opt_flow_meas->t_ns << " obs_cam0=" << o0
              << " obs_cam1=" << o1 << " connected=" << conn
              << " frame_states=" << frame_states.size()
              << " frame_poses=" << frame_poses.size() << std::endl;
  }
'''
s = s.replace(anchor, dbg + anchor, 1)
if '#include <iostream>' not in s:
    s = s.replace('#include', '#include <iostream>\n#include', 1)
open(p, 'w').write(s)
print("instrumented keypoint_vio.cpp")
