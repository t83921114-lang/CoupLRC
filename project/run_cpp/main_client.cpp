#include "client.h"
#include "toolbox.h"
#include <fstream>
#include <sys/time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "config.h"
#include <iomanip>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <random>
#include "encoder.h"
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>

namespace {

// Same as CoordinatorImpl::*_stripe_placement: map2cluster = (stripe_id + map2group) % cluster_num
int block_id_to_group(const std::string &code_type, int k, int r, int z, int block_id)
{
    if (code_type == "LotusLRC")
    {
        auto m = ECProject::get_lotuslrc_block_id_to_group_id(k, r, z);
        return m.at(block_id);
    }
    if (code_type == "AzureLRC")
    {
        auto m = ECProject::get_azurelrc_block_id_to_group_id(k, r, z);
        return m.at(block_id);
    }
    if (code_type == "OptimalLRC")
    {
        auto m = ECProject::get_optimal_lrc_block_id_to_group_id(k, r, z);
        return m.at(block_id);
    }
    if (code_type == "UniformLRC")
    {
        auto m = ECProject::get_uniform_lrc_block_id_to_group_id(k, r, z);
        return m.at(block_id);
    }
    if (code_type == "UniLRC")
    {
        int local_data_num = k / z;
        int local_global_parity_num = r / z;
        if (block_id < k)
            return block_id / local_data_num;
        if (block_id < k + r)
            return (block_id - k) / local_global_parity_num;
        return (block_id - k - r);
    }
    throw std::runtime_error("unsupported code_type for layout: " + code_type);
}

std::vector<int> blocks_on_cluster(const std::string &code_type, int k, int r, int z, int n,
                                   int stripe_id, int cluster_id, int cluster_num)
{
    std::vector<int> blocks;
    blocks.reserve(static_cast<size_t>(n));
    for (int bid = 0; bid < n; ++bid)
    {
        int gid = block_id_to_group(code_type, k, r, z, bid);
        if ((stripe_id + gid) % cluster_num == cluster_id)
            blocks.push_back(bid);
    }
    std::sort(blocks.begin(), blocks.end());
    return blocks;
}

void print_block_ids(const char *label, const std::vector<int> &ids)
{
    std::cout << label;
    for (size_t i = 0; i < ids.size(); ++i)
        std::cout << (i ? ", " : " ") << ids[i];
    std::cout << std::endl;
}

// Throughput = recovered_data_mb / elapsed_seconds
void print_throughput_summary(const char *test_name,
                              const std::vector<std::chrono::duration<double>> &time_spans,
                              double recovered_mb)
{
    if (time_spans.empty())
    {
        std::cout << test_name << ": no successful samples" << std::endl;
        return;
    }
    std::vector<double> throughputs;
    throughputs.reserve(time_spans.size());
    for (const auto &t : time_spans)
    {
        if (t.count() <= 0)
            continue;
        throughputs.push_back(recovered_mb / t.count());
    }
    if (throughputs.empty())
    {
        std::cout << test_name << ": no valid timing samples" << std::endl;
        return;
    }
    const double avg = std::accumulate(throughputs.begin(), throughputs.end(), 0.0) /
                       static_cast<double>(throughputs.size());
    const double max_tp = *std::max_element(throughputs.begin(), throughputs.end());
    const double min_tp = *std::min_element(throughputs.begin(), throughputs.end());
    std::cout << "Average throughput: " << avg << " MB/s" << std::endl;
    std::cout << "Max throughput: " << max_tp << " MB/s" << std::endl;
    std::cout << "Min throughput: " << min_tp << " MB/s" << std::endl;
}

// Single-rack repair split: how many of the N failed blocks (all in one local group)
// go through the global (cross-group) batch, leaving the rest for cheap local-group
// recovery. The number left for local recovery equals the number of local-parity
// blocks per local group:
//   - LotusLRC: 2 local parities/local group => leave 2, global-recover N-2.
//   - AzureLRC/OptimalLRC/UniformLRC: 1 local parity/local group => leave 1,
//     global-recover N-1.
// Returns the subset (a prefix of failed_on_rack) to recover via the global batch.
int local_recovery_block_num(const std::string &code_type)
{
    return (code_type == "LotusLRC") ? 2 : 1;
}

// Local parity block id(s) of a local group: LotusLRC has 2 local parities per local group
// (k+r+2*lg, k+r+2*lg+1), the other LRCs have 1 (k+r+lg).
std::vector<int> local_parity_ids_of_group(const std::string &code_type, int k, int r, int z, int lg)
{
    (void)z;
    if (code_type == "LotusLRC")
        return {k + r + 2 * lg, k + r + 2 * lg + 1};
    return {k + r + lg};
}

std::vector<int> multi_recovery_batch(const std::string &code_type, int r,
                                      const std::vector<int> &failed_on_rack)
{
    (void)r;
    const size_t local_num = static_cast<size_t>(local_recovery_block_num(code_type));
    if (failed_on_rack.size() <= local_num)
        return {}; // everything can be handled by local-group recovery
    return std::vector<int>(failed_on_rack.begin(),
                            failed_on_rack.end() - static_cast<std::ptrdiff_t>(local_num));
}

} // namespace

int main(int argc, char **argv)
{
    char buff[256];
    getcwd(buff, 256);
    std::string exe_path(argv[0]);
    std::string exe_dir;
    size_t last_slash = exe_path.rfind('/');
    if (last_slash != std::string::npos) {
        exe_dir = exe_path.substr(0, last_slash);
        if (exe_dir.empty() || exe_dir[0] != '/')
            exe_dir = std::string(buff) + "/" + exe_dir;
    } else {
        exe_dir = buff;
    }
    std::string sys_config_path = exe_dir + "/../../config/parameterConfiguration.xml";
    //std::string sys_config_path = "/home/GuanTian/lql/UniLRC/project/config/parameterConfiguration.xml";
    std::cout << "Current working directory: " << sys_config_path << std::endl;

    const ECProject::Config *config = ECProject::Config::getInstance(sys_config_path);
    std::string client_ip = "10.10.1.1";
    int client_port = 44444;
    ECProject::Client client(client_ip, client_port, config->CoordinatorIP + ":" + std::to_string(config->CoordinatorPort), sys_config_path);
    std::cout << client.sayHelloToCoordinatorByGrpc("Client ID: " + client_ip + ":" + std::to_string(client_port)) << std::endl;

    std::vector<int> parameters = client.get_parameters();
    int k = parameters[0];
    int r = parameters[1];
    int z = parameters[2];
    std::string code_type;
    if(parameters[4] == 0){
        code_type = "AzureLRC";
    }
    else if(parameters[4] == 1){
        code_type = "OptimalLRC";
    }
    else if(parameters[4] == 2){
        code_type = "UniformLRC";
    }
    else if(parameters[4] == 3){
        code_type = "UniLRC";
    }
    else if(parameters[4] == 4){
        code_type = "LotusLRC";
    }
    else{
        std::cout << "Code type error" << std::endl;
        return -1;
    }
    double block_size = static_cast<double> (parameters[3]) / 1024 / 1024; //MB
    int n = k + r + z;
    
    int stripe_num = 1;
    size_t total_write_size = static_cast<size_t>(stripe_num * block_size * k); // MB
    std::cout << "Starting set stripe operation" << std::endl;
    std::chrono::high_resolution_clock::time_point set_start = std::chrono::high_resolution_clock::now();
    for(int i = 0; i < stripe_num; i++){
        client.set();
    }
    std::chrono::high_resolution_clock::time_point set_end = std::chrono::high_resolution_clock::now();
    std::cout << "Set stripe operation finished" << std::endl;
    std::cout << "Conducting experiments, please wait..." << std::endl;
    std::chrono::duration<double> set_time = std::chrono::duration_cast<std::chrono::duration<double>>(set_end - set_start);
    std::cout << "write throughput: " << (static_cast<double>(total_write_size) / set_time.count()) << " MB/s" << std::endl;
    std::mt19937 rng(std::random_device{}());
    sleep(5);

    std::uniform_int_distribution<int> dist_500(0, k*stripe_num - 500);
    std::uniform_real_distribution<double> dist_double(0.0, 1.0);
    
    
    //for read test
    
    // std::cout << "Normal read test start" << std::endl;
    // std::vector<std::chrono::duration<double>> read_time_spans;
    // for(int i = 0; i < 5; i++){
    //     size_t data_size;
    //     int id = i;
    //     std::string key = std::to_string(id);
    //     std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
    //     std::shared_ptr<char[]> data = client.get(key, data_size);
    //     if(!data){
    //         std::cout << "Get operation failed" << std::endl;
    //         continue;
    //     }
    //     std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
    //     std::chrono::duration<double> time_span = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    //     read_time_spans.push_back(time_span);
    //     //std::cout << "get time: " << time_span.count() << std::endl;
    // }
    // std::chrono::duration<double> read_total_time_span = std::accumulate(read_time_spans.begin(), read_time_spans.end(), std::chrono::duration<double>(0));
    // std::cout << "Total time: " << read_total_time_span.count() << std::endl;
    // std::cout << "Average time: " << read_total_time_span.count() / read_time_spans.size() << std::endl;
    // std::cout << "Throughput (stripes/s): " << read_time_spans.size() / read_total_time_span.count() << std::endl;
    // std::cout << "Speed: " << static_cast<size_t>(block_size) * k / (read_total_time_span.count() / read_time_spans.size()) << " MB/s" << std::endl;
    // std::chrono::duration<double> read_max_time_span = *std::max_element(read_time_spans.begin(), read_time_spans.end());
    // std::chrono::duration<double> read_min_time_span = *std::min_element(read_time_spans.begin(), read_time_spans.end());
    // std::cout << "Max speed: " << static_cast<size_t>(block_size) * k / read_min_time_span.count() << " MB/s" << std::endl;
    // std::cout << "Min speed: " << static_cast<size_t>(block_size) * k / read_max_time_span.count() << " MB/s" << std::endl;
    // std::cout << "Normal read test end" << std::endl;
    // std::cout << std::endl;

    
    // //for degraded read test
    
    // std::vector<std::chrono::duration<double>> degraded_read_time_spans;
    // std::cout << "Degraded read test start" << std::endl;
    // for(int i = 0; i < k; i++){
    //     size_t data_size;
    //     int id = i;
    //     std::string key = std::to_string(id);
    //     std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
    //     std::shared_ptr<char[]> data = client.get_degraded_read_block(0, i);
    //     if(!data){
    //         std::cout << "Degraded read operation failed" << std::endl;
    //         continue;
    //     }
    //     std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
    //     std::chrono::duration<double> time_span = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    //     degraded_read_time_spans.push_back(time_span);
    //     //std::cout << "get time: " << time_span.count() << std::endl;
    // }
    // std::chrono::duration<double> degraded_read_total_time_span = std::accumulate(degraded_read_time_spans.begin(), degraded_read_time_spans.end(), std::chrono::duration<double>(0));
    // std::cout << "Average time: " << degraded_read_total_time_span.count() / degraded_read_time_spans.size() << std::endl;
    // std::chrono::duration<double> degraded_read_max_time_span = *std::max_element(degraded_read_time_spans.begin(), degraded_read_time_spans.end());
    // std::chrono::duration<double> degraded_read_min_time_span = *std::min_element(degraded_read_time_spans.begin(), degraded_read_time_spans.end());
    // std::cout << "Max time: "<< degraded_read_max_time_span.count() << std::endl;
    // std::cout << "Min time: "<< degraded_read_min_time_span.count() << std::endl;
    // std::cout << "Throughput (blocks/s): " << degraded_read_time_spans.size() / degraded_read_total_time_span.count() << std::endl;
    // std::cout << "Speed: " << static_cast<size_t>(block_size) / (degraded_read_total_time_span.count() / degraded_read_time_spans.size()) << " MB/s" << std::endl;
    // std::cout << "Max speed: " << static_cast<size_t>(block_size) / degraded_read_min_time_span.count() << " MB/s" << std::endl;
    // std::cout << "Min speed: " << static_cast<size_t>(block_size) / degraded_read_max_time_span.count() << " MB/s" << std::endl;
    // std::cout << "Degraded read test end" << std::endl;
    // std::cout << std::endl;

    /*
    //for single block recovery
    
    std::cout << "Single block recovery test start" << std::endl;
    std::vector<std::chrono::duration<double>> block_recovery_time_spans;
    for(int i = 0; i < n; i++){
        std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
        client.recovery(0, i);
        std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> time_span = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
        block_recovery_time_spans.push_back(time_span);
        //std::cout << "single block repair time: " << time_span.count() << std::endl;
    }
    std::chrono::duration<double> block_recovery_total_time_span = std::accumulate(block_recovery_time_spans.begin(), block_recovery_time_spans.end(), std::chrono::duration<double>(0));
    std::chrono::duration<double> block_recovery_max_time_span = *std::max_element(block_recovery_time_spans.begin(), block_recovery_time_spans.end());
    std::chrono::duration<double> block_recovery_min_time_span = *std::min_element(block_recovery_time_spans.begin(), block_recovery_time_spans.end());
    //std::cout << "Total time: " << total_time_span.count() << std::endl;
    std::cout << "Average time: " << block_recovery_total_time_span.count() / block_recovery_time_spans.size() << std::endl;
    std::cout << "Max time: "<< block_recovery_max_time_span.count() << std::endl;
    std::cout << "Min time: "<< block_recovery_min_time_span.count() << std::endl;
    std::cout << "Single block recovery test end" << std::endl;
    std::cout << std::endl;
    */
    //client.recovery(0, 0);
    //client.multi_block_recovery(0, {0, 1});
    //sleep(5);

/*
    // for one block recovery
    {
        const double recovered_mb = block_size;
        std::vector<std::chrono::duration<double>> one_block_recovery_time_spans;
        std::cout << "One block recovery test start" << std::endl;
        for (int i = 0; i < 10; i++)
        {
            std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
            client.recovery(0, 0);
            std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> time_span =
                std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
            one_block_recovery_time_spans.push_back(time_span);
            if (time_span.count() > 0)
                std::cout << "[" << i << "th] One block recovery throughput: "
                          << (recovered_mb / time_span.count()) << " MB/s" << std::endl;
        }
        print_throughput_summary("One block recovery", one_block_recovery_time_spans, recovered_mb);
        std::cout << "One block recovery test end" << std::endl;
        std::cout << std::endl;
    }
*/
/*
    // for two block recovery (test blocks 0 and 1)
    {
        const double recovered_mb = 2.0 * block_size;
        std::vector<std::chrono::duration<double>> multi_block_recovery_time_spans;
        std::cout << "Two block recovery test start (blocks 0, 1)" << std::endl;
        for (int i = 0; i < 10; i++)
        {
            std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
            client.multi_block_recovery(0, {0, 1});
            std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> time_span =
                std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
            multi_block_recovery_time_spans.push_back(time_span);
            if (time_span.count() > 0)
                std::cout << "[" << i << "th] Two block recovery throughput: "
                          << (recovered_mb / time_span.count()) << " MB/s" << std::endl;
        }
        print_throughput_summary("Two block recovery", multi_block_recovery_time_spans, recovered_mb);
        std::cout << "Two block recovery test end" << std::endl;
        std::cout << std::endl;
    }
*/
/*
// Multi block recovery: first cluster (rack) fails under current layout + placement
    {
        const int test_stripe_id = 0;
        const int failed_cluster_id = 0;
        std::vector<int> first_rack_failed;
        try
        {
            first_rack_failed = blocks_on_cluster(
                code_type, k, r, z, n, test_stripe_id, failed_cluster_id, config->ClusterNum);
        }
        catch (const std::exception &e)
        {
            std::cout << "Layout lookup failed: " << e.what() << std::endl;
            return -1;
        }
        if (first_rack_failed.empty())
        {
            std::cout << "No blocks on cluster " << failed_cluster_id
                      << " for stripe " << test_stripe_id << ", skip one-rack test" << std::endl;
        }
        else
        {
        std::vector<int> multi_recover_ids = multi_recovery_batch(code_type, r, first_rack_failed);
        // Leftover blocks (not in the global batch) are recovered by local-group repair.
        std::vector<int> local_recover_ids;
        for (int bid : first_rack_failed)
        {
            if (std::find(multi_recover_ids.begin(), multi_recover_ids.end(), bid) ==
                multi_recover_ids.end())
                local_recover_ids.push_back(bid);
        }
        // Lotus offloads 2 same-local-group blocks to a single-round 2-parity local
        // recovery; the others offload 1 block per single-block local recovery.
        const bool lotus_two_block_local =
            (code_type == "LotusLRC" && local_recover_ids.size() == 2);

        std::cout << "Multi block recovery test start (one rack, cluster "
                  << failed_cluster_id << ", stripe " << test_stripe_id << ")" << std::endl;
        print_block_ids("  Failed blocks on rack (layout+placement):", first_rack_failed);
        print_block_ids("  Blocks via globalRecovery batch:", multi_recover_ids);
        if (lotus_two_block_local)
            print_block_ids("  Blocks via Lotus same-group 2-parity local recovery:", local_recover_ids);
        else
            print_block_ids("  Blocks via recovery() one-by-one (single-block local):", local_recover_ids);

        const double recovered_mb =
            static_cast<double>(first_rack_failed.size()) * block_size;

        std::vector<std::chrono::duration<double>> multi_block_recovery_one_rack_time_spans;
        for (int i = 0; i < 10; i++)
        {
            std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
            // Step 1: global batch recovers the bulk (N-2 for Lotus, N-1 for others).
            if (!multi_recover_ids.empty())
                client.multi_block_recovery(test_stripe_id, first_rack_failed, multi_recover_ids);
            // Step 2: local-group recovery of the leftover block(s).
            if (lotus_two_block_local)
                client.multi_block_recovery(test_stripe_id, local_recover_ids, {});
            else
                for (int bid : local_recover_ids)
                    client.recovery(test_stripe_id, bid);
            std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> time_span =
                std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
            multi_block_recovery_one_rack_time_spans.push_back(time_span);
            if (time_span.count() > 0)
                std::cout << "[" << i << "th] One rack recovery throughput: "
                          << (recovered_mb / time_span.count()) << " MB/s" << std::endl;
        }
        print_throughput_summary("One rack recovery", multi_block_recovery_one_rack_time_spans,
                                 recovered_mb);
        std::cout << "One rack recovery test end" << std::endl;
        std::cout << std::endl;
        }
    }
*/

/*
    // Maintenance-robust normal read: one rack (cluster) is under maintenance / failed, which
    // takes out several blocks of the stripe. We read the whole stripe's data blocks by first
    // reconstructing the failed rack's DATA blocks in memory (no disk write-back) using the
    // single-rack N-1/N-2 split (cross-group global batch + in-memory local fill), then reading
    // the surviving data blocks directly.
    {
        const int test_stripe_id = 0;
        const int failed_cluster_id = 0;
        std::vector<int> first_rack_failed;
        try
        {
            first_rack_failed = blocks_on_cluster(
                code_type, k, r, z, n, test_stripe_id, failed_cluster_id, config->ClusterNum);
        }
        catch (const std::exception &e)
        {
            std::cout << "Layout lookup failed: " << e.what() << std::endl;
            return -1;
        }
        // Only data blocks (block_id < k) are reconstructed/read.
        std::vector<int> failed_data;
        for (int bid : first_rack_failed)
            if (bid < k)
                failed_data.push_back(bid);

        if (failed_data.empty())
        {
            std::cout << "No data blocks on cluster " << failed_cluster_id << " for stripe "
                      << test_stripe_id << ", skip maintenance-robust read test" << std::endl;
        }
        else
        {
            // Same N-1/N-2 split as single-rack repair, restricted to one local group: of the N
            // blocks of the group that fell on the failed rack (data + co-located local parities),
            // N-x go through the cross-group global batch and x stay as the leftover for local fill,
            // where x = the group's local-parity count (LotusLRC: 2, the other LRCs: 1).
            //
            // Maintenance read only needs DATA, so we only reconstruct the data blocks among both
            // sides. The x leftover slots are conceptually filled FIRST by the group's failed local
            // parities (the read never has to solve those), then by data blocks; hence the number of
            // DATA blocks actually local-filled is x - (failed local parities) = the surviving local
            // parities. Co-locating a local parity on the failed rack therefore just shrinks the
            // data leftover (down to 0 => everything goes through the global batch). We compute the
            // leftover directly as that many trailing data blocks of the group.
            auto parity_survives = [&](int pid) {
                int gid = block_id_to_group(code_type, k, r, z, pid);
                int cl = (test_stripe_id + gid) % config->ClusterNum;
                return cl != failed_cluster_id;
            };
            // Choose the local group of the last failed data block; size the leftover to that
            // group's surviving local parities.
            int g_last = ECProject::get_block_id_to_local_group_id(code_type, k, r, z, failed_data.back());
            std::vector<int> grp_failed_data;
            for (int bid : failed_data)
                if (ECProject::get_block_id_to_local_group_id(code_type, k, r, z, bid) == g_last)
                    grp_failed_data.push_back(bid);
            int surviving_parities = 0;
            for (int pid : local_parity_ids_of_group(code_type, k, r, z, g_last))
                if (parity_survives(pid))
                    surviving_parities++;
            int leftover_cnt = std::min<int>(surviving_parities, static_cast<int>(grp_failed_data.size()));
            std::unordered_set<int> leftover_set(
                grp_failed_data.end() - leftover_cnt, grp_failed_data.end());
            std::vector<int> global_batch;
            std::vector<int> local_fill;
            for (int bid : failed_data)
            {
                if (leftover_set.count(bid))
                    local_fill.push_back(bid);
                else
                    global_batch.push_back(bid);
            }

            std::cout << "Maintenance-robust normal read test start (one rack, cluster "
                      << failed_cluster_id << ", stripe " << test_stripe_id << ", code "
                      << code_type << ")" << std::endl;
            print_block_ids("  Failed blocks on rack (layout+placement):", first_rack_failed);
            print_block_ids("  Failed DATA blocks to reconstruct:", failed_data);
            print_block_ids("  Phase-1 DATA blocks via cross-group global batch:", global_batch);
            print_block_ids("  Phase-2 DATA blocks via in-memory local fill:", local_fill);

            // Whole-stripe read throughput: k data blocks delivered to the client.
            const double recovered_mb = static_cast<double>(k) * block_size;
            std::vector<std::chrono::duration<double>> maintenance_read_time_spans;
            for (int i = 0; i < 10; i++)
            {
                size_t data_size = 0;
                std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
                std::shared_ptr<char[]> data =
                    client.maintenance_read(test_stripe_id, failed_data, global_batch, data_size);
                std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
                if (!data)
                {
                    std::cout << "Maintenance-robust read operation failed" << std::endl;
                    continue;
                }
                std::chrono::duration<double> time_span =
                    std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
                maintenance_read_time_spans.push_back(time_span);
                if (time_span.count() > 0)
                    std::cout << "[" << i << "th] Maintenance-robust read throughput: "
                              << (recovered_mb / time_span.count()) << " MB/s" << std::endl;
            }
            print_throughput_summary("Maintenance-robust normal read",
                                     maintenance_read_time_spans, recovered_mb);
            std::cout << "Maintenance-robust normal read test end" << std::endl;
            std::cout << std::endl;
        }
    }
*/

    // const int total_nodes = config->ClusterNum * config->DatanodeNumPerCluster;

    //Full node repair: rand over [0, ClusterNum*DatanodeNumPerCluster) (replaces legacy 19*30)
    // std::cout << "Full node repair test start" << std::endl;
    // const int node_num = 5;
    // std::vector<int> node_ids;
    // if (total_nodes <= 0)
    // {
    //     std::cout << "Invalid ClusterNum/DatanodeNumPerCluster, skip full node repair test" << std::endl;
    // }
    // else
    // {
    //     while (node_ids.size() < static_cast<size_t>(node_num))
    //     {
    //         int random_id = rand() % total_nodes;
    //         if (std::find(node_ids.begin(), node_ids.end(), random_id) == node_ids.end())
    //             node_ids.push_back(random_id);
    //     }
    //     std::cout << "node_id range [0, " << (total_nodes - 1) << "], sampled nodes:";
    //     print_block_ids("", node_ids);

    //     std::vector<double> full_node_recovery_speeds;
    //     for (int i = 0; i < node_num; i++)
    //     {
    //         std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
    //         int block_num = client.recovery_full_node(node_ids[i]);
    //         std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
    //         std::chrono::duration<double> time_span =
    //             std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    //         if (block_num <= 0)
    //         {
    //             std::cout << "  node " << node_ids[i] << ": skip (no blocks on node or RPC failed)"
    //                       << std::endl;
    //             continue;
    //         }
    //         if (time_span.count() <= 0)
    //             continue;
    //         double total_size = static_cast<double>(block_num) * block_size;
    //         double speed = total_size / time_span.count();
    //         full_node_recovery_speeds.push_back(speed);
    //         std::cout << "  node " << node_ids[i] << ": " << block_num << " blocks, "
    //                   << speed << " MB/s" << std::endl;
    //     }
    //     if (full_node_recovery_speeds.empty())
    //     {
    //         std::cout << "No successful full-node recovery samples (all nodes empty?)" << std::endl;
    //     }
    //     else
    //     {
    //         std::cout << "Average speed: "
    //                   << std::accumulate(full_node_recovery_speeds.begin(),
    //                                      full_node_recovery_speeds.end(), 0.0) /
    //                          full_node_recovery_speeds.size()
    //                   << " MB/s" << std::endl;
    //         std::cout << "Max speed: "
    //                   << *std::max_element(full_node_recovery_speeds.begin(),
    //                                        full_node_recovery_speeds.end())
    //                   << " MB/s" << std::endl;
    //         std::cout << "Min speed: "
    //                   << *std::min_element(full_node_recovery_speeds.begin(),
    //                                        full_node_recovery_speeds.end())
    //                   << " MB/s" << std::endl;
    //     }
    //     std::cout << "Full node repair test end" << std::endl;
    //     std::cout << std::endl;
    // }

    // Two-node repair: per stripe, 0 blocks skip / 1 block single / 2 blocks dual recovery
    // std::cout << "Two node repair test start" << std::endl;
    // const int pair_num = 5;
    // if (total_nodes <= 1)
    // {
    //     std::cout << "Need at least 2 nodes, skip two-node repair test" << std::endl;
    // }
    // else
    // {
    //     std::vector<std::pair<int, int>> node_pairs;
    //     while (node_pairs.size() < static_cast<size_t>(pair_num))
    //     {
    //         int n0 = rand() % total_nodes;
    //         int n1 = rand() % total_nodes;
    //         while (n1 == n0)
    //             n1 = rand() % total_nodes;
    //         if (n0 > n1)
    //             std::swap(n0, n1);
    //         const std::pair<int, int> p{n0, n1};
    //         if (std::find(node_pairs.begin(), node_pairs.end(), p) == node_pairs.end())
    //             node_pairs.push_back(p);
    //     }
    //     std::cout << "node_id range [0, " << (total_nodes - 1) << "], sampled pairs:";
    //     for (size_t i = 0; i < node_pairs.size(); i++)
    //         std::cout << " (" << node_pairs[i].first << "," << node_pairs[i].second << ")";
    //     std::cout << std::endl;

    //     std::vector<double> two_node_recovery_speeds;
    //     for (int i = 0; i < pair_num; i++)
    //     {
    //         const int n0 = node_pairs[i].first;
    //         const int n1 = node_pairs[i].second;
    //         std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
    //         int block_num = client.recovery_two_nodes(n0, n1);
    //         std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
    //         std::chrono::duration<double> time_span =
    //             std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    //         if (block_num <= 0)
    //         {
    //             std::cout << "  nodes " << n0 << "," << n1
    //                       << ": skip (no blocks on both nodes or RPC failed)" << std::endl;
    //             continue;
    //         }
    //         if (time_span.count() <= 0)
    //             continue;
    //         double total_size = static_cast<double>(block_num) * block_size;
    //         double speed = total_size / time_span.count();
    //         two_node_recovery_speeds.push_back(speed);
    //         std::cout << "  nodes " << n0 << "," << n1 << ": " << block_num << " blocks, "
    //                   << speed << " MB/s" << std::endl;
    //     }
    //     if (two_node_recovery_speeds.empty())
    //     {
    //         std::cout << "No successful two-node recovery samples (all pairs empty?)" << std::endl;
    //     }
    //     else
    //     {
    //         std::cout << "Average speed: "
    //                   << std::accumulate(two_node_recovery_speeds.begin(),
    //                                      two_node_recovery_speeds.end(), 0.0) /
    //                          two_node_recovery_speeds.size()
    //                   << " MB/s" << std::endl;
    //         std::cout << "Max speed: "
    //                   << *std::max_element(two_node_recovery_speeds.begin(),
    //                                        two_node_recovery_speeds.end())
    //                   << " MB/s" << std::endl;
    //         std::cout << "Min speed: "
    //                   << *std::min_element(two_node_recovery_speeds.begin(),
    //                                        two_node_recovery_speeds.end())
    //                   << " MB/s" << std::endl;
    //     }
    //     std::cout << "Two node repair test end" << std::endl;
    //     std::cout << std::endl;
    // }

    //for decode test
    // std::cout << "Decode test start" << std::endl;
    // std::vector<double> decode_time_spans;
    // for(int i = 0; i < n; i++){
    //     double decode_time_span;
    //     client.decode_test(0, i, client_ip, client_port, decode_time_span);
    //     decode_time_spans.push_back(decode_time_span);
    // }
    // std::cout << "Average decode time: " << std::endl;
    // std::cout << std::accumulate(decode_time_spans.begin(), decode_time_spans.end(), 0.0) / decode_time_spans.size() << std::endl;
    // std::cout << "Decode test end" << std::endl;
    // std::cout << std::endl;
    return 0;
}
