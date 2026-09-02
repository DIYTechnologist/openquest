# Print the estimated state each frame, so a gradual blow-up (bad time alignment / IMU scale) can
# be told apart from a sudden singular update.
p = '/basalt/src/vi_estimator/keypoint_vio.cpp'
s = open(p).read()
anchor = '  marginalize(num_points_connected);'
add = '''  if (!frame_states.empty()) {
    const auto& st = frame_states.rbegin()->second.getState();
    std::cout << "   STATE p=" << st.T_w_i.translation().norm()
              << " v=" << st.vel_w_i.norm()
              << " bg=" << st.bias_gyro.norm()
              << " ba=" << st.bias_accel.norm() << std::endl;
  }
'''
assert anchor in s
s = s.replace(anchor, add + anchor, 1)
open(p, 'w').write(s)
print("state instrumentation added")
