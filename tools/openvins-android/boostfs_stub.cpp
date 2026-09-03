// Stubs for the three boost::filesystem symbols OpenVINS links against, for the Android benchmark
// build (step X, notes/18).
//
// boost::filesystem is the only Boost component that is not header-only, and OpenVINS reaches it
// from exactly one place: VioManager's constructor, which removes and recreates the timing-log
// directory when `record_timing_information` is set. That option is false in every config we run,
// so building Boost.Filesystem for arm64 would be work spent on a code path the benchmark never
// executes.
//
// These abort rather than returning a plausible value, for the same reason as the dynamic-init
// stub: silently returning "false"/"0" would let a run proceed with timing output quietly disabled
// or a path silently wrong, and a benchmark that lies is worse than one that stops.
#include <boost/filesystem.hpp>

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace {
[[noreturn]] void bail(const char *fn) {
  fprintf(stderr,
          "[ov_bench] FATAL: %s called, but Boost.Filesystem is stubbed in this Android build.\n"
          "           It is only reachable via record_timing_information -- leave that false.\n", fn);
  abort();
}
} // namespace

namespace boost {
namespace filesystem {
namespace detail {

// status() is NOT stubbed out: it is genuinely reached (YamlParser checks the config file exists,
// and VioManagerOptions checks mask paths), so it gets a real stat()-backed implementation. Only
// the write-side operations, which nothing in our configuration reaches, abort.
BOOST_FILESYSTEM_DECL file_status status(path const &p, system::error_code *ec) {
  struct ::stat st;
  if (ec) ec->clear();
  if (::stat(p.c_str(), &st) != 0) return file_status(file_not_found, no_perms);
  file_type t = S_ISDIR(st.st_mode)  ? directory_file
              : S_ISREG(st.st_mode)  ? regular_file
              : S_ISLNK(st.st_mode)  ? symlink_file
                                     : type_unknown;
  return file_status(t, static_cast<perms>(st.st_mode & 07777));
}

BOOST_FILESYSTEM_DECL bool remove(path const &, system::error_code *) { bail("remove"); }
BOOST_FILESYSTEM_DECL bool create_directories(path const &, system::error_code *) { bail("create_directories"); }

BOOST_FILESYSTEM_DECL path::string_type::size_type
path_algorithms::find_parent_path_size(path const &) { bail("find_parent_path_size"); }

} // namespace detail
} // namespace filesystem
} // namespace boost
