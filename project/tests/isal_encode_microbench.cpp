/**
 * Layered ISA-L encoding microbenchmark (Scheme B).
 * Level 0: full encode_* (incl. heap alloc per call)
 * Level 1: production hot path (memset + matrix + init_tables + encode, no alloc)
 * Level 2: ec_init_tables + ec_encode_data_avx2
 * Level 3: ec_encode_data_avx2 only (processor encoding kernel)
 */
#include "config.h"
#include "encoder.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{

enum class BenchmarkLevel
{
  L0_FullWithAlloc = 0,
  L1_Production = 1,
  L2_InitAndEncode = 2,
  L3_KernelOnly = 3,
};

struct Options
{
  std::string config_path;
  int warmup_iters = 20;
  int measure_iters = 100;
  int batch = 1;
  int level = 3;
  bool all_levels = false;
  bool zero_parity_in_loop = true;
  int block_size_override = 0;
};

struct TimingStats
{
  double min_s = 0.0;
  double max_s = 0.0;
  double avg_s = 0.0;
  double median_s = 0.0;
  double p95_s = 0.0;
};

struct BenchContext
{
  const ECProject::Config *cfg = nullptr;
  int k = 0;
  int r = 0;
  int z = 0;
  int rows = 0;
  int block_size = 0;
  unsigned char **data_ptrs = nullptr;
  unsigned char **parity_ptrs = nullptr;
  unsigned char *encode_matrix = nullptr;
  unsigned char *g_tbls = nullptr;
  bool zero_parity_in_loop = true;
  std::vector<unsigned char *> blocks;
};

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

static void print_usage(const char *argv0)
{
  std::cerr
      << "Usage: " << argv0 << " [config.xml] [options]\n"
      << "Options:\n"
      << "  --level=N           Benchmark level 0-3 (default: 3)\n"
      << "  --all-levels        Run levels 0 through 3\n"
      << "  --warmup=N          Warm-up iterations (default: 20)\n"
      << "  --iters=N           Measure iterations (default: 100)\n"
      << "  --batch=N           Encodes per timed sample (default: 1)\n"
      << "  --block-size=N      Override BlockSize from config\n"
      << "  --zero-parity-in-loop   Level 1: memset parity inside timed loop (default)\n"
      << "  --zero-parity-outside   Level 1: memset parity outside timed loop\n"
      << "\nLevels:\n"
      << "  0  full encode_* incl. new/delete each call\n"
      << "  1  memset + gen_matrix + init_tables + encode (no alloc)\n"
      << "  2  ec_init_tables + ec_encode_data_avx2\n"
      << "  3  ec_encode_data_avx2 only\n";
}

static bool parse_int_arg(const std::string &arg, const std::string &prefix, int &out)
{
  if (arg.rfind(prefix, 0) != 0)
  {
    return false;
  }
  out = std::stoi(arg.substr(prefix.size()));
  return true;
}

static bool parse_options(int argc, char **argv, Options &opt)
{
  opt.config_path = default_config_path(argv[0]);
  std::vector<std::string> positional;

  for (int i = 1; i < argc; ++i)
  {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h")
    {
      print_usage(argv[0]);
      return false;
    }
    if (arg == "--all-levels")
    {
      opt.all_levels = true;
      continue;
    }
    if (arg == "--zero-parity-in-loop")
    {
      opt.zero_parity_in_loop = true;
      continue;
    }
    if (arg == "--zero-parity-outside")
    {
      opt.zero_parity_in_loop = false;
      continue;
    }

    int value = 0;
    if (parse_int_arg(arg, "--level=", value))
    {
      opt.level = value;
      continue;
    }
    if (parse_int_arg(arg, "--warmup=", value))
    {
      opt.warmup_iters = value;
      continue;
    }
    if (parse_int_arg(arg, "--iters=", value))
    {
      opt.measure_iters = value;
      continue;
    }
    if (parse_int_arg(arg, "--batch=", value))
    {
      opt.batch = value;
      continue;
    }
    if (parse_int_arg(arg, "--block-size=", value))
    {
      opt.block_size_override = value;
      continue;
    }

    if (!arg.empty() && arg[0] == '-')
    {
      std::cerr << "Unknown option: " << arg << std::endl;
      return false;
    }
    positional.push_back(arg);
  }

  if (!positional.empty())
  {
    opt.config_path = positional[0];
  }

  if (opt.warmup_iters < 0 || opt.measure_iters <= 0 || opt.batch <= 0)
  {
    std::cerr << "Invalid warmup/iters/batch values." << std::endl;
    return false;
  }
  if (!opt.all_levels && (opt.level < 0 || opt.level > 3))
  {
    std::cerr << "Level must be 0-3." << std::endl;
    return false;
  }
  return true;
}

static std::string read_cpu_model()
{
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::string line;
  while (std::getline(cpuinfo, line))
  {
    if (line.rfind("model name", 0) == 0)
    {
      size_t colon = line.find(':');
      if (colon != std::string::npos && colon + 1 < line.size())
      {
        std::string model = line.substr(colon + 1);
        size_t start = model.find_first_not_of(" \t");
        if (start != std::string::npos)
        {
          model = model.substr(start);
        }
        return model;
      }
    }
  }
  return "unknown";
}

static void gen_encode_matrix(const ECProject::Config *cfg, unsigned char *encode_matrix, int k, int r, int z)
{
  const std::string &code_type = cfg->CodeType;
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

static void run_encode_full(const ECProject::Config *cfg,
                            unsigned char **data_ptrs,
                            unsigned char **parity_ptrs,
                            int block_size)
{
  int k = cfg->k;
  int r = cfg->r;
  int z = cfg->z;
  const std::string &code_type = cfg->CodeType;

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

static void zero_parity_blocks(BenchContext &ctx)
{
  for (int i = 0; i < ctx.rows; ++i)
  {
    std::memset(ctx.parity_ptrs[i], 0, static_cast<size_t>(ctx.block_size));
  }
}

static void run_level0(BenchContext &ctx)
{
  run_encode_full(ctx.cfg, ctx.data_ptrs, ctx.parity_ptrs, ctx.block_size);
}

static void run_level1(BenchContext &ctx)
{
  if (ctx.zero_parity_in_loop)
  {
    zero_parity_blocks(ctx);
  }
  gen_encode_matrix(ctx.cfg, ctx.encode_matrix, ctx.k, ctx.r, ctx.z);
  ECProject::ec_init_tables(ctx.k, ctx.rows, ctx.encode_matrix + ctx.k * ctx.k, ctx.g_tbls);
  ECProject::ec_encode_data_avx2(ctx.block_size, ctx.k, ctx.rows, ctx.g_tbls, ctx.data_ptrs, ctx.parity_ptrs);
}

static void run_level2(BenchContext &ctx)
{
  zero_parity_blocks(ctx);
  ECProject::ec_init_tables(ctx.k, ctx.rows, ctx.encode_matrix + ctx.k * ctx.k, ctx.g_tbls);
  ECProject::ec_encode_data_avx2(ctx.block_size, ctx.k, ctx.rows, ctx.g_tbls, ctx.data_ptrs, ctx.parity_ptrs);
}

static void run_level3(BenchContext &ctx)
{
  zero_parity_blocks(ctx);
  ECProject::ec_encode_data_avx2(ctx.block_size, ctx.k, ctx.rows, ctx.g_tbls, ctx.data_ptrs, ctx.parity_ptrs);
}

static void run_level(BenchContext &ctx, BenchmarkLevel level)
{
  switch (level)
  {
  case BenchmarkLevel::L0_FullWithAlloc:
    run_level0(ctx);
    break;
  case BenchmarkLevel::L1_Production:
    run_level1(ctx);
    break;
  case BenchmarkLevel::L2_InitAndEncode:
    run_level2(ctx);
    break;
  case BenchmarkLevel::L3_KernelOnly:
    run_level3(ctx);
    break;
  }
}

static const char *level_name(BenchmarkLevel level)
{
  switch (level)
  {
  case BenchmarkLevel::L0_FullWithAlloc:
    return "L0 full encode_* (with alloc)";
  case BenchmarkLevel::L1_Production:
    return "L1 production hot path";
  case BenchmarkLevel::L2_InitAndEncode:
    return "L2 init_tables + encode";
  case BenchmarkLevel::L3_KernelOnly:
    return "L3 kernel only (ec_encode_data_avx2)";
  }
  return "unknown";
}

static TimingStats compute_stats(std::vector<double> times)
{
  TimingStats stats;
  if (times.empty())
  {
    return stats;
  }

  std::sort(times.begin(), times.end());
  stats.min_s = times.front();
  stats.max_s = times.back();
  stats.avg_s = std::accumulate(times.begin(), times.end(), 0.0) / static_cast<double>(times.size());
  stats.median_s = times[times.size() / 2];
  size_t p95_index = static_cast<size_t>(static_cast<double>(times.size()) * 0.95);
  if (p95_index >= times.size())
  {
    p95_index = times.size() - 1;
  }
  stats.p95_s = times[p95_index];
  return stats;
}

static TimingStats benchmark_level(BenchContext &ctx, BenchmarkLevel level, int warmup_iters, int measure_iters, int batch)
{
  for (int i = 0; i < warmup_iters; ++i)
  {
    for (int b = 0; b < batch; ++b)
    {
      run_level(ctx, level);
    }
  }

  std::vector<double> sample_seconds;
  sample_seconds.reserve(static_cast<size_t>(measure_iters));
  for (int i = 0; i < measure_iters; ++i)
  {
    auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < batch; ++b)
    {
      run_level(ctx, level);
    }
    auto t1 = std::chrono::steady_clock::now();
    sample_seconds.push_back(std::chrono::duration<double>(t1 - t0).count());
  }

  return compute_stats(sample_seconds);
}

static void print_level_result(BenchmarkLevel level,
                               const TimingStats &stats,
                               int batch,
                               double data_mb_per_encode,
                               double stripe_mb_per_encode)
{
  const double batch_factor = static_cast<double>(batch);
  const double data_mb_per_sample = data_mb_per_encode * batch_factor;
  const double stripe_mb_per_sample = stripe_mb_per_encode * batch_factor;

  auto print_throughput = [&](const char *label, double seconds)
  {
    std::cout << "  " << label << "_time_s: " << seconds << std::endl;
    std::cout << "  " << label << "_data_MBps: " << (data_mb_per_sample / seconds) << std::endl;
    std::cout << "  " << label << "_stripe_MBps: " << (stripe_mb_per_sample / seconds) << std::endl;
  };

  std::cout << "\n--- " << level_name(level) << " ---" << std::endl;
  print_throughput("min", stats.min_s);
  print_throughput("median", stats.median_s);
  print_throughput("avg", stats.avg_s);
  print_throughput("p95", stats.p95_s);
  print_throughput("max", stats.max_s);
}

static BenchContext setup_context(const ECProject::Config *cfg, int block_size, bool zero_parity_in_loop)
{
  BenchContext ctx;
  ctx.cfg = cfg;
  ctx.k = cfg->k;
  ctx.r = cfg->r;
  ctx.z = cfg->z;
  ctx.rows = cfg->r + cfg->z;
  ctx.block_size = block_size;
  ctx.zero_parity_in_loop = zero_parity_in_loop;

  const int block_num = ctx.k + ctx.rows;
  ctx.blocks.assign(static_cast<size_t>(block_num), nullptr);
  for (int i = 0; i < block_num; ++i)
  {
    ctx.blocks[static_cast<size_t>(i)] =
        static_cast<unsigned char *>(std::aligned_alloc(32, static_cast<size_t>(block_size)));
    if (ctx.blocks[static_cast<size_t>(i)] == nullptr)
    {
      std::cerr << "Failed to allocate block buffers." << std::endl;
      std::exit(1);
    }
  }

  ctx.data_ptrs = ctx.blocks.data();
  ctx.parity_ptrs = ctx.blocks.data() + ctx.k;
  for (int i = 0; i < ctx.k; ++i)
  {
    std::memset(ctx.data_ptrs[i], 0xAA, static_cast<size_t>(block_size));
  }

  const int m = ctx.k + ctx.r;
  ctx.encode_matrix = new unsigned char[static_cast<size_t>((m + ctx.z) * ctx.k)];
  ctx.g_tbls = new unsigned char[static_cast<size_t>(ctx.k * ctx.rows * 32)];
  gen_encode_matrix(cfg, ctx.encode_matrix, ctx.k, ctx.r, ctx.z);
  ECProject::ec_init_tables(ctx.k, ctx.rows, ctx.encode_matrix + ctx.k * ctx.k, ctx.g_tbls);
  return ctx;
}

static void cleanup_context(BenchContext &ctx)
{
  for (unsigned char *p : ctx.blocks)
  {
    std::free(p);
  }
  ctx.blocks.clear();
  delete[] ctx.encode_matrix;
  delete[] ctx.g_tbls;
  ctx.encode_matrix = nullptr;
  ctx.g_tbls = nullptr;
}

} // namespace

int main(int argc, char **argv)
{
  Options opt;
  if (!parse_options(argc, argv, opt))
  {
    return 1;
  }

  std::cout << "=== ISA-L Encoding Microbenchmark ===" << std::endl;
  std::cout << "Config path: " << opt.config_path << std::endl;
  std::cout << "CPU: " << read_cpu_model() << std::endl;

  const ECProject::Config *cfg = ECProject::Config::getInstance(opt.config_path);
  const int block_size = (opt.block_size_override > 0) ? opt.block_size_override : static_cast<int>(cfg->BlockSize);

  std::cout << "CodeType: " << cfg->CodeType << std::endl;
  std::cout << "k=" << cfg->k << " r=" << cfg->r << " z=" << cfg->z
            << " rows=" << (cfg->r + cfg->z) << " BlockSize=" << block_size
            << " (" << (static_cast<double>(block_size) / 1024.0 / 1024.0) << " MiB)" << std::endl;
  std::cout << "Warmup: " << opt.warmup_iters << "  Measure: " << opt.measure_iters
            << "  Batch: " << opt.batch << std::endl;
  if (!opt.all_levels)
  {
    std::cout << "Level: " << opt.level << std::endl;
  }
  std::cout << "Level 1 zero parity: "
            << (opt.zero_parity_in_loop ? "inside timed loop" : "outside timed loop") << std::endl;

  BenchContext ctx = setup_context(cfg, block_size, opt.zero_parity_in_loop);

  const double data_mb_per_encode =
      static_cast<double>(ctx.k) * static_cast<double>(block_size) / 1024.0 / 1024.0;
  const double stripe_mb_per_encode =
      static_cast<double>(ctx.k + ctx.rows) * static_cast<double>(block_size) / 1024.0 / 1024.0;

  std::vector<BenchmarkLevel> levels;
  if (opt.all_levels)
  {
    levels = {BenchmarkLevel::L0_FullWithAlloc,
              BenchmarkLevel::L1_Production,
              BenchmarkLevel::L2_InitAndEncode,
              BenchmarkLevel::L3_KernelOnly};
  }
  else
  {
    levels = {static_cast<BenchmarkLevel>(opt.level)};
  }

  for (BenchmarkLevel level : levels)
  {
    TimingStats stats = benchmark_level(ctx, level, opt.warmup_iters, opt.measure_iters, opt.batch);
    print_level_result(level, stats, opt.batch, data_mb_per_encode, stripe_mb_per_encode);
  }

  cleanup_context(ctx);
  return 0;
}
