// Stub for OpenVINS' dynamic initialiser, for the Android benchmark build (step X, notes/18).
//
// DynamicInitializer is the only thing in ov_init that pulls in Ceres, and porting Ceres to Android
// would be a substantial job for code we do not execute: our configuration runs static
// initialisation (`init_dyn_use: false`, and `notes/14` fixed the static path's threshold), so
// InertialInitializer only ever reaches the dynamic branch when that flag is set.
//
// The stub aborts rather than returning false. Returning false would silently degrade to "failed to
// initialise" and could be mistaken for a real result; aborting makes it impossible to benchmark a
// configuration this build cannot honestly represent.
#include "dynamic/DynamicInitializer.h"

#include <cstdio>
#include <cstdlib>

namespace ov_init {

bool DynamicInitializer::initialize(double &timestamp, Eigen::MatrixXd &covariance,
                                    std::vector<std::shared_ptr<ov_type::Type>> &order,
                                    std::shared_ptr<ov_type::IMU> &_imu,
                                    std::map<double, std::shared_ptr<ov_type::PoseJPL>> &_clones_IMU,
                                    std::unordered_map<size_t, std::shared_ptr<ov_type::Landmark>> &_features_SLAM) {
  (void)timestamp; (void)covariance; (void)order; (void)_imu; (void)_clones_IMU; (void)_features_SLAM;
  fprintf(stderr,
          "[ov_bench] FATAL: dynamic initialisation was reached, but this Android build excludes it\n"
          "           (Ceres is not ported). Set init_dyn_use: false -- the benchmark is only valid\n"
          "           for the static-init configuration we actually run.\n");
  abort();
}

} // namespace ov_init
