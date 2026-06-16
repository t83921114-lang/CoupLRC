/**
 * Exp#7: encoding microbenchmark.
 *
 * 计完整 encode_*() 端到端耗时，包含每次的 parity memset、矩阵生成、
 * ec_init_tables、内核、内存分配/释放。
 *
 * 吞吐口径：data_mb = k * block_size / 1024^2，单位 MiB/s（仅数据块字节）。
 */
#include "config.h"
#include "encoder.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <numeric>
#include <string>
#include <unistd.h>
#include <vector>

static void run_encode(const std::string &code_type, int k, int r, int z,
                       unsigned char **data_ptrs, unsigned char **parity_ptrs,
                       int block_size)
{
  if (code_type == "UniLRC")
  {
    ECProject::encode_unilrc(k, r, z, data_ptrs, parity_ptrs, block_size);
  }
  else if (code_type == "OptimalLRC")
  {
    ECProject::encode_optimal_lrc(k, r, z, data_ptrs, parity_ptrs, block_size);
  }
  else if (code_type == "UniformLRC")
  {
    ECProject::encode_uniform_lrc(k, r, z, data_ptrs, parity_ptrs, block_size);
  }
  else if (code_type == "AzureLRC")
  {
    ECProject::encode_azure_lrc(k, r, z, data_ptrs, parity_ptrs, block_size);
  }
  else if (code_type == "LotusLRC")
  {
    ECProject::encode_lotuslrc(k, r, z, data_ptrs, parity_ptrs, block_size);
  }
  else
  {
    std::cerr << "Unknown CodeType: " << code_type << std::endl;
    std::exit(1);
  }
}

static std::string default_config_path(const char *argv0)
{
  char cwd[256];
  if (getcwd(cwd, sizeof(cwd)) == nullptr)
  {
    return "project/config/parameterConfiguration.xml";
  }
  std::string exe_path(argv0);
  size_t last_slash = exe_path.rfind('/');
  std::string exe_dir = (last_slash != std::string::npos) ? exe_path.substr(0, last_slash) : std::string(cwd);
  if (exe_dir.empty() || exe_dir[0] != '/')
  {
    exe_dir = std::string(cwd) + "/" + exe_dir;
  }
  return exe_dir + "/../../config/parameterConfiguration.xml";
}

int main(int argc, char **argv)
{
  std::string config_path = (argc > 1) ? argv[1] : default_config_path(argv[0]);
  int warmup_iters = (argc > 2) ? std::stoi(argv[2]) : 20;
  int measure_iters = (argc > 3) ? std::stoi(argv[3]) : 100;

  if (warmup_iters < 0 || measure_iters <= 0)
  {
    std::cerr << "Usage: " << argv[0]
              << " [config.xml] [warmup_iters=20] [measure_iters=100]" << std::endl;
    return 1;
  }

  std::cout << "Config path: " << config_path << std::endl;
  const ECProject::Config *cfg = ECProject::Config::getInstance(config_path);

  int k = cfg->k;
  int r = cfg->r;
  int z = cfg->z;
  int block_size = static_cast<int>(cfg->BlockSize);
  int block_num = k + r + z;
  const std::string code_type = cfg->CodeType;

  std::vector<unsigned char *> blocks(block_num, nullptr);
  for (int i = 0; i < block_num; ++i)
  {
    blocks[i] = static_cast<unsigned char *>(std::aligned_alloc(32, block_size));
    if (blocks[i] == nullptr)
    {
      std::cerr << "Failed to allocate block buffer (need ~"
                << (static_cast<long long>(block_num) * block_size / 1024 / 1024)
                << " MB total)" << std::endl;
      return 1;
    }
  }

  unsigned char **data_ptrs = blocks.data();
  unsigned char **parity_ptrs = blocks.data() + k;
  for (int i = 0; i < k; ++i)
  {
    std::memset(data_ptrs[i], 0xAA, block_size);
  }

  std::cout << "Warm-up: " << warmup_iters << " iteration(s)" << std::endl;
  for (int i = 0; i < warmup_iters; ++i)
  {
    run_encode(code_type, k, r, z, data_ptrs, parity_ptrs, block_size);
  }

  std::vector<double> times;
  times.reserve(static_cast<size_t>(measure_iters));
  std::cout << "Measuring: " << measure_iters << " iteration(s)" << std::endl;
  for (int i = 0; i < measure_iters; ++i)
  {
    auto t0 = std::chrono::high_resolution_clock::now();
    run_encode(code_type, k, r, z, data_ptrs, parity_ptrs, block_size);
    auto t1 = std::chrono::high_resolution_clock::now();
    times.push_back(std::chrono::duration<double>(t1 - t0).count());
  }

  std::sort(times.begin(), times.end());
  double sum = std::accumulate(times.begin(), times.end(), 0.0);
  double avg = sum / measure_iters;
  double median = times[static_cast<size_t>(measure_iters / 2)];
  double min_t = times.front();
  double max_t = times.back();
  double data_mb = static_cast<double>(k) * block_size / 1024.0 / 1024.0;

  std::cout << "\n=== Encoding benchmark result ===" << std::endl;
  std::cout << "CodeType: " << code_type << std::endl;
  std::cout << "k=" << k << " r=" << r << " z=" << z << " BlockSize=" << block_size << std::endl;
  std::cout << "throughput_scope: data blocks (k=" << k << " blocks)" << std::endl;
  std::cout << "encode_time_avg_s: " << avg << std::endl;
  std::cout << "encode_time_median_s: " << median << std::endl;
  std::cout << "encode_time_min_s: " << min_t << std::endl;
  std::cout << "encode_time_max_s: " << max_t << std::endl;
  std::cout << "encode_throughput_avg_MBps: " << (data_mb / avg) << std::endl;
  std::cout << "encode_throughput_median_MBps: " << (data_mb / median) << std::endl;

  for (unsigned char *p : blocks)
  {
    std::free(p);
  }
  return 0;
}
