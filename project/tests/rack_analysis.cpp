#include "encoder.h"
#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <vector>

static int placement_group(const std::string &code, int k, int r, int z, int bid)
{
  if (code == "LotusLRC")
    return ECProject::get_lotuslrc_block_id_to_group_id(k, r, z).at(bid);
  return ECProject::get_azurelrc_block_id_to_group_id(k, r, z).at(bid);
}

static void analyze(const std::string &code, int k, int r, int z, int n, int cluster_num)
{
  std::cout << "\n=== " << code << " k=" << k << " r=" << r << " z=" << z << " n=" << n
            << " ===" << std::endl;
  const int stripe = 0, cluster = 0;
  std::vector<int> failed;
  for (int bid = 0; bid < n; ++bid) {
    const int gid = placement_group(code, k, r, z, bid);
    if ((stripe + gid) % cluster_num == cluster)
      failed.push_back(bid);
  }
  std::sort(failed.begin(), failed.end());
  std::cout << "Failed on cluster 0:";
  for (size_t i = 0; i < failed.size(); ++i)
    std::cout << (i ? ", " : " ") << failed[i];
  std::cout << std::endl;

  std::unordered_map<int, int> lg_count;
  for (int bid : failed) {
    const int lg = ECProject::get_block_id_to_local_group_id(code, k, r, z, bid);
    const int pg = placement_group(code, k, r, z, bid);
    lg_count[lg]++;
    std::cout << "  block " << bid << " placement_group=" << pg << " local_group=" << lg
              << std::endl;
  }
  std::cout << "Distinct local groups: " << lg_count.size() << std::endl;

  const auto phases = ECProject::plan_multi_block_recovery(code, k, r, z, failed);
  std::cout << "Plan phases: " << phases.size() << std::endl;
  for (size_t i = 0; i < phases.size(); ++i) {
    const auto &p = phases[i];
    std::cout << "  phase " << (i + 1) << ": "
              << (p.kind == ECProject::RecoveryPhaseKind::GlobalMulti ? "GlobalMulti"
                                                                      : "SingleBlock");
    std::cout << " recover=[";
    if (p.recover_ids.empty())
      std::cout << "all";
    else
      for (size_t j = 0; j < p.recover_ids.size(); ++j)
        std::cout << (j ? "," : "") << p.recover_ids[j];
    std::cout << "] context=[";
    for (size_t j = 0; j < p.all_failed.size(); ++j)
      std::cout << (j ? "," : "") << p.all_failed[j];
    std::cout << "]" << std::endl;
  }
}

int main()
{
  analyze("LotusLRC", 72, 3, 8, 83, 21);
  analyze("AzureLRC", 72, 4, 4, 80, 21);
  return 0;
}
