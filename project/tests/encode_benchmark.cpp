/**
 * Exp#7: encoding microbenchmark.
 *
 * 支持两种计时口径，通过下面的 #define 注释切换（二选一）：
 *
 *   BENCH_MODE_KERNEL : 只计 ec_encode_data_avx2 内核耗时（对标 ECWide FAST'21
 *                       Figure 1 / Intel ISA-L erasure_code_perf）。
 *                       编码矩阵生成与 ec_init_tables 在循环外做一次并复用，不计时。
 *
 *   BENCH_MODE_FULL   : 计完整 encode_*() 端到端耗时（原始行为），包含每次的
 *                       parity memset、矩阵生成、ec_init_tables、内核、内存分配/释放。
 *
 * 吞吐口径：stripe_mb = (k + r + z) * block_size / 1024^2，单位 MiB/s（全条带字节）。
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
 
 /* ===================== 测试模式切换：二选一，注释掉不用的那一行 ===================== */
 //#define BENCH_MODE_KERNEL
 #define BENCH_MODE_FULL
 /* ================================================================================== */
 
 #if defined(BENCH_MODE_KERNEL) && defined(BENCH_MODE_FULL)
 #error "只能启用 BENCH_MODE_KERNEL 或 BENCH_MODE_FULL 其中之一"
 #endif
 #if !defined(BENCH_MODE_KERNEL) && !defined(BENCH_MODE_FULL)
 #error "必须启用 BENCH_MODE_KERNEL 或 BENCH_MODE_FULL 其中之一"
 #endif
 
 #if defined(BENCH_MODE_KERNEL)
 static const char *kBenchModeName = "kernel (ec_encode_data_avx2 only, Figure-1 scope)";
 #else
 static const char *kBenchModeName = "full (end-to-end encode_*)";
 #endif
 
 #if defined(BENCH_MODE_FULL)
 /* 完整端到端编码：调用各码型的 encode_*()（含矩阵生成 / init_tables / 内核 / 分配释放） */
 static void run_encode_full(const std::string &code_type, int k, int r, int z,
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
 #endif
 
 #if defined(BENCH_MODE_KERNEL)
 /* 仅 kernel 模式需要：生成对应码型的编码矩阵（循环外一次，不计时） */
 static void gen_encode_matrix(const std::string &code_type, int k, int r, int z,
                               unsigned char *encode_matrix)
 {
   if (code_type == "UniLRC")
   {
     ECProject::gen_unilrc_matrix(encode_matrix, k, r, z);
   }
   else if (code_type == "OptimalLRC")
   {
     ECProject::gen_optimal_lrc_matrix(encode_matrix, k, r, z);
   }
   else if (code_type == "UniformLRC")
   {
     ECProject::gen_uniform_lrc_matrix(encode_matrix, k, r, z);
   }
   else if (code_type == "AzureLRC")
   {
     ECProject::gen_azure_lrc_matrix(encode_matrix, k, r, z);
   }
   else if (code_type == "LotusLRC")
   {
     ECProject::gen_lotuslrc_matrix(encode_matrix, k, r, z);
   }
   else
   {
     std::cerr << "Unknown CodeType: " << code_type << std::endl;
     std::exit(1);
   }
 }
 #endif
 
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
   std::cout << "Bench mode : " << kBenchModeName << std::endl;
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
 
 #if defined(BENCH_MODE_KERNEL)
   /* 循环外准备（不计时）：生成编码矩阵 + 初始化 Galois 乘法表，整个 run 复用一次。 */
   std::vector<unsigned char> encode_matrix(static_cast<size_t>(block_num) * k);
   std::vector<unsigned char> g_tbls(static_cast<size_t>(k) * (r + z) * 32);
   gen_encode_matrix(code_type, k, r, z, encode_matrix.data());
   ECProject::ec_init_tables(k, r + z, &encode_matrix[static_cast<size_t>(k) * k], g_tbls.data());
 
 #define BENCH_ENCODE_ONCE()                                                          \
   ECProject::ec_encode_data_avx2(block_size, k, r + z, g_tbls.data(), data_ptrs, parity_ptrs)
 #else
 #define BENCH_ENCODE_ONCE()                                                          \
   run_encode_full(code_type, k, r, z, data_ptrs, parity_ptrs, block_size)
 #endif
 
   std::cout << "Warm-up: " << warmup_iters << " iteration(s)" << std::endl;
   for (int i = 0; i < warmup_iters; ++i)
   {
     BENCH_ENCODE_ONCE();
   }
 
   std::vector<double> times;
   times.reserve(static_cast<size_t>(measure_iters));
   std::cout << "Measuring: " << measure_iters << " iteration(s)" << std::endl;
   for (int i = 0; i < measure_iters; ++i)
   {
     auto t0 = std::chrono::high_resolution_clock::now();
     BENCH_ENCODE_ONCE();
     auto t1 = std::chrono::high_resolution_clock::now();
     times.push_back(std::chrono::duration<double>(t1 - t0).count());
   }
 
   std::sort(times.begin(), times.end());
   double sum = std::accumulate(times.begin(), times.end(), 0.0);
   double avg = sum / measure_iters;
   double median = times[static_cast<size_t>(measure_iters / 2)];
   double min_t = times.front();
   double max_t = times.back();
  double stripe_mb = static_cast<double>(block_num) * block_size / 1024.0 / 1024.0;

  std::cout << "\n=== Encoding benchmark result ===" << std::endl;
  std::cout << "mode: " << kBenchModeName << std::endl;
  std::cout << "CodeType: " << code_type << std::endl;
  std::cout << "k=" << k << " r=" << r << " z=" << z << " BlockSize=" << block_size << std::endl;
  std::cout << "throughput_scope: full stripe (k+r+z=" << block_num << " blocks)" << std::endl;
  std::cout << "encode_time_avg_s: " << avg << std::endl;
  std::cout << "encode_time_median_s: " << median << std::endl;
  std::cout << "encode_time_min_s: " << min_t << std::endl;
  std::cout << "encode_time_max_s: " << max_t << std::endl;
  std::cout << "encode_throughput_avg_MBps: " << (stripe_mb / avg) << std::endl;
  std::cout << "encode_throughput_median_MBps: " << (stripe_mb / median) << std::endl;
 
   for (unsigned char *p : blocks)
   {
     std::free(p);
   }
   return 0;
 }
 