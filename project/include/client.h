#ifndef CLIENT_H
#define CLIENT_H

#ifdef BAZEL_BUILD
#include "src/proto/coordinator.grpc.pb.h"
#else
#include "coordinator.grpc.pb.h"
#endif

#include "meta_definition.h"
#include <grpcpp/grpcpp.h>
#include <asio.hpp>
#include "config.h"
#include "toolbox.h"
#include <vector>
#include <set>
namespace ECProject
{
  class Client
  {
  public:
    Client(std::string ClientIP, int ClientPort, std::string CoordinatorIpPort) : m_coordinatorIpPort(CoordinatorIpPort),
                                                                                  m_clientIPForGet(ClientIP),
                                                                                  m_clientPortForGet(ClientPort),
                                                                                  acceptor(io_context, asio::ip::tcp::endpoint(asio::ip::address::from_string(ClientIP.c_str()), m_clientPortForGet))
    {
      auto channel = grpc::CreateChannel(m_coordinatorIpPort, grpc::InsecureChannelCredentials());
      m_coordinator_ptr = coordinator_proto::coordinatorService::NewStub(channel);
      m_clientID = ClientIP + ":" + std::to_string(ClientPort);
    }

    Client(std::string ClientIP, int ClientPort, std::string CoordinatorIpPort, std::string config_path) : m_coordinatorIpPort(CoordinatorIpPort),
                                                                                                           m_clientIPForGet(ClientIP),
                                                                                                           m_clientPortForGet(ClientPort),
                                                                                                           acceptor(io_context, asio::ip::tcp::endpoint(asio::ip::address::from_string(ClientIP.c_str()), m_clientPortForGet))
    {
      auto channel = grpc::CreateChannel(m_coordinatorIpPort, grpc::InsecureChannelCredentials());
      m_coordinator_ptr = coordinator_proto::coordinatorService::NewStub(channel);
      m_clientID = ClientIP + ":" + std::to_string(ClientPort);
      m_sys_config = ECProject::Config::getInstance(config_path);
      m_toolbox = ECProject::ToolBox::getInstance();
      m_pre_allocated_buffer = new char[static_cast<size_t> (m_sys_config->BlockSize) * static_cast<size_t> (m_sys_config->n)];
      memset(m_pre_allocated_buffer, 0xaa, (m_sys_config->BlockSize) * static_cast<size_t> (m_sys_config->n));
      if (m_sys_config->AppendMode == "CACHED_MODE")
      {
        m_cached_buffer = new char *[m_sys_config->r + m_sys_config->z];
        for (int i = 0; i < m_sys_config->r + m_sys_config->z; i++)
        {
          m_cached_buffer[i] = new char[m_sys_config->BlockSize];
          memset(m_cached_buffer[i], 0, m_sys_config->BlockSize);
        }
      }
    }

    ~Client()
    {
      delete[] m_pre_allocated_buffer;
      if (m_sys_config->AppendMode == "CACHED_MODE")
      {
        for (int i = 0; i < m_sys_config->r + m_sys_config->z; i++)
        {
          delete[] m_cached_buffer[i];
        }
        delete[] m_cached_buffer;
      }
    }

    std::string sayHelloToCoordinatorByGrpc(std::string hello);
    bool append(int append_size);
    bool sub_append(int append_size);
    bool sub_append_in_rep_mode(int append_size);
    bool set();
    bool sub_set(int block_num);
    std::shared_ptr<char[]> get_degraded_read_block(int stripe_id, int failed_block_id);
    std::shared_ptr<char[]> get_degraded_read_block_breakdown(int stripe_id, int failed_block_id, double &total_time, double &disk_io_time, double &network_time, double &encode_time);
    bool recovery_breakdown(int stripe_id, int failed_block_id, double &disk_read_time, double &network_time, double &decode_time, double &disk_write_time);
    bool recovery(int stripe_id, int failed_block_id);
    // all_failed_block_ids: full failed set for decode plan; recovery_block_ids: subset to repair (empty = repair all)
    bool multi_block_recovery(int stripe_id, std::vector<int> all_failed_block_ids,
                              const std::vector<int> &recovery_block_ids = {});
    bool multi_block_recovery_breakdown(int stripe_id, std::vector<int> all_failed_block_ids,
                                        double &disk_read_time, double &network_time,
                                        double &decode_time, double &disk_write_time,
                                        const std::vector<int> &recovery_block_ids = {});
    int recovery_full_node(int node_id);
    int recovery_two_nodes(int node_id_0, int node_id_1);
    bool set(std::string key, std::string value);
    bool SetParameterByGrpc(ECSchema input_ecschema);
    std::shared_ptr<char[]> get(std::string key, size_t &data_size);
    // Maintenance-robust normal read: under single-rack failure, reconstruct the failed rack's
    // data blocks in memory (no disk write-back) using the single-rack N-1/N-2 split, then read
    // the whole stripe's k data blocks. failed_data_block_ids: data blocks (id < k) on the failed
    // rack; global_batch_block_ids: subset reconstructed via the global batch (the rest = local
    // fill). Returns the k data blocks (k * BlockSize) assembled in block-id order.
    std::shared_ptr<char[]> maintenance_read(int stripe_id,
                                             const std::vector<int> &failed_data_block_ids,
                                             const std::vector<int> &global_batch_block_ids,
                                             size_t &data_size);
    std::shared_ptr<char[]> get_blocks(int start_block_id, int end_block_id);
    std::shared_ptr<char[]> get_degraded_read_blocks(int start_block_id, int end_block_id);
    bool get(std::string key, std::string &value);
    bool delete_key(std::string key);
    bool delete_stripe(int stripe_id);
    bool delete_all_stripes();
    int get_append_slice_plans(std::string append_mode, int curr_logical_offset, int append_size, std::vector<std::vector<int>> *node_slice_sizes_per_cluster, std::vector<int> *modified_data_block_nums_per_cluster, std::vector<int> *data_ptr_size_array, int &parity_slice_size, int &parity_slice_offset);
    void split_for_append_data_and_parity(const coordinator_proto::ReplyProxyIPsPorts *reply_proxy_ips_ports, const std::vector<char *> &cluster_slice_data, const std::vector<std::vector<int>> &node_slice_sizes_per_cluster, const std::vector<int> &modified_data_block_nums_per_cluster, std::vector<char *> &data_ptr_array, std::vector<char *> &global_parity_ptr_array, std::vector<char *> &local_parity_ptr_array);
    void split_for_set_data_and_parity(const coordinator_proto::ReplyProxyIPsPorts *reply_proxy_ips_ports, const std::vector<char *> &cluster_slice_data, const std::vector<int> &data_block_num_per_group, const std::vector<int> &global_parity_block_num_per_group, const std::vector<int> &local_parity_block_num_per_group, std::vector<char *> &data_ptr_array, std::vector<char *> &global_parity_ptr_array, std::vector<char *> &local_parity_ptr_array);
    void async_append_to_proxies(char *cluster_slice_data, std::string append_key, int cluster_slice_size, std::string proxy_ip, int proxy_port, int index, bool *if_commit_arr);
    void get_cached_parity_slices(std::vector<char *> &global_parity_ptr_array, std::vector<char *> &local_parity_ptr_array, const int parity_slice_size, const int parity_slice_offset);
    void cache_latest_parity_slices(std::vector<char *> &global_parity_ptr_array, std::vector<char *> &local_parity_ptr_array, const int parity_slice_size, const int parity_slice_offset);
    std::vector<int> get_parameters();
    bool decode_test(int stripe_id, int failed_block_id, std::string client_ip, int client_port, double &decode_time);

  private:
    bool call_global_recovery(int stripe_id, const std::vector<int> &all_failed_block_ids,
                              const std::vector<int> &recovery_block_ids);
    bool call_global_recovery_breakdown(int stripe_id, const std::vector<int> &all_failed_block_ids,
                                          const std::vector<int> &recovery_block_ids,
                                          double &disk_read_time, double &network_time,
                                          double &decode_time, double &disk_write_time);
    // Scheme B: clusters touched by a single-block recovery (sources + dest). Empty = cannot
    // determine (caller should serialize). Used to decide if two recoveries can run in parallel.
    std::set<int> recovery_cluster_set(int stripe_id, int failed_block_id);
    // Relaxed Scheme B: the single write-back (dest) cluster of a recovery. The only real
    // concurrency hazard is two recoveries sharing the same dest proxy (single shared acceptor);
    // shared read-only source clusters are safe. Returns -1 if it cannot be determined.
    int recovery_dest_cluster(int stripe_id, int failed_block_id);
    std::unique_ptr<coordinator_proto::coordinatorService::Stub> m_coordinator_ptr;
    std::string m_coordinatorIpPort;
    std::string m_clientIPForGet;
    int m_clientPortForGet;
    std::string m_clientID;
    asio::io_context io_context;
    asio::ip::tcp::acceptor acceptor;

    int m_append_logical_offset = 0;
    ECProject::Config *m_sys_config;
    ECProject::ToolBox *m_toolbox;
    char *m_pre_allocated_buffer = nullptr;
    char **m_cached_buffer = nullptr;
  };

} // namespace ECProject

#endif // CLIENT_H