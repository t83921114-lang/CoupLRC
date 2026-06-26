#ifndef BREAKDOWN_TIMING_H
#define BREAKDOWN_TIMING_H

#include "proxy.grpc.pb.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace ECProject {
namespace breakdown_timing {

inline double max_span_duration(const std::vector<double> &starts,
                                const std::vector<double> &ends)
{
  double max_d = 0.0;
  const size_t n = std::min(starts.size(), ends.size());
  for (size_t i = 0; i < n; i++)
  {
    if (ends[i] > starts[i])
      max_d = std::max(max_d, ends[i] - starts[i]);
  }
  return max_d;
}

inline double grpc_delay_max(const std::vector<double> &notify_times,
                            const std::vector<double> &grpc_start_times)
{
  if (notify_times.empty() || grpc_start_times.empty())
    return 0.0;
  const double max_start = *std::max_element(grpc_start_times.begin(), grpc_start_times.end());
  const double min_notify = *std::min_element(notify_times.begin(), notify_times.end());
  return std::max(0.0, max_start - min_notify);
}

inline double duration_seconds(
    const std::chrono::high_resolution_clock::time_point &start,
    const std::chrono::high_resolution_clock::time_point &end)
{
  return std::chrono::duration<double>(end - start).count();
}

// Store wall-clock durations (start=0, end=duration) for coordinator aggregation.
inline void publish_degraded_read(proxy_proto::DegradedReadReply *reply,
                                  double disk_read_seconds,
                                  double network_seconds,
                                  double decode_seconds)
{
  reply->set_disk_io_start_time(0.0);
  reply->set_disk_io_end_time(disk_read_seconds);
  reply->set_network_start_time(0.0);
  reply->set_network_end_time(network_seconds);
  reply->set_decode_start_time(0.0);
  reply->set_decode_end_time(decode_seconds);
  reply->set_cross_rack_time(0.0);
  reply->set_cross_rack_xor_time(0.0);
  reply->set_data_node_grpc_notify_time(0.0);
  reply->set_data_node_grpc_start_time(0.0);
}

inline void publish_recovery(proxy_proto::RecoveryReply *reply,
                             double disk_read_seconds,
                             double network_seconds,
                             double decode_seconds,
                             double disk_write_seconds)
{
  reply->set_disk_io_start_time(0.0);
  reply->set_disk_io_end_time(disk_read_seconds);
  reply->set_network_start_time(0.0);
  reply->set_network_end_time(network_seconds);
  reply->set_decode_start_time(0.0);
  reply->set_decode_end_time(decode_seconds);
  reply->set_cross_rack_time(0.0);
  reply->set_cross_rack_xor_time(0.0);
  reply->set_dest_data_node_network_time(0.0);
  reply->set_dest_data_node_disk_io_time(disk_write_seconds);
  reply->set_data_node_grpc_notify_time(0.0);
  reply->set_data_node_grpc_start_time(0.0);
}

} // namespace breakdown_timing
} // namespace ECProject

#endif
