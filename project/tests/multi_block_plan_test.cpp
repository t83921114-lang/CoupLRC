/**
 * Unit tests for plan_multi_block_recovery (pure logic, no gRPC).
 */
#include "encoder.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

static bool vec_eq(const std::vector<int> &a, const std::vector<int> &b)
{
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i] != b[i])
      return false;
  return true;
}

static const char *kind_str(ECProject::RecoveryPhaseKind k)
{
  return k == ECProject::RecoveryPhaseKind::GlobalMulti ? "GlobalMulti" : "SingleBlock";
}

static void print_phases(const std::vector<ECProject::RecoveryPhase> &phases)
{
  for (size_t i = 0; i < phases.size(); ++i) {
    const auto &p = phases[i];
    std::cout << "    phase " << i << ": " << kind_str(p.kind) << " all_failed=[";
    for (size_t j = 0; j < p.all_failed.size(); ++j)
      std::cout << (j ? "," : "") << p.all_failed[j];
    std::cout << "] recover=[";
    for (size_t j = 0; j < p.recover_ids.size(); ++j)
      std::cout << (j ? "," : "") << p.recover_ids[j];
    std::cout << "]" << std::endl;
  }
}

static bool expect_phases(const char *label,
                          const std::vector<ECProject::RecoveryPhase> &phases,
                          const std::vector<ECProject::RecoveryPhaseKind> &kinds,
                          const std::vector<std::vector<int>> &all_failed_sets,
                          const std::vector<std::vector<int>> &recover_sets)
{
  if (phases.size() != kinds.size()) {
    std::cerr << "FAIL: " << label << " expected " << kinds.size() << " phase(s), got "
              << phases.size() << std::endl;
    print_phases(phases);
    return false;
  }
  for (size_t i = 0; i < phases.size(); ++i) {
    if (phases[i].kind != kinds[i]) {
      std::cerr << "FAIL: " << label << " phase " << i << " kind" << std::endl;
      print_phases(phases);
      return false;
    }
    if (!vec_eq(phases[i].all_failed, all_failed_sets[i])) {
      std::cerr << "FAIL: " << label << " phase " << i << " all_failed" << std::endl;
      print_phases(phases);
      return false;
    }
    if (!vec_eq(phases[i].recover_ids, recover_sets[i])) {
      std::cerr << "FAIL: " << label << " phase " << i << " recover_ids" << std::endl;
      print_phases(phases);
      return false;
    }
  }
  std::cout << "PASS: " << label << std::endl;
  return true;
}

static std::vector<int> pick_same_local_group_data_blocks(const std::string &code_type, int k, int r,
                                                          int z, int count)
{
  std::vector<int> picked;
  for (int bid = 0; bid < k && static_cast<int>(picked.size()) < count; ++bid) {
    try {
      const int lg = ECProject::get_block_id_to_local_group_id(code_type, k, r, z, bid);
      if (picked.empty()) {
        picked.push_back(bid);
      } else if (lg == ECProject::get_block_id_to_local_group_id(code_type, k, r, z, picked[0])) {
        picked.push_back(bid);
      }
    } catch (const std::exception &) {
      continue;
    }
  }
  return picked;
}

static std::vector<int> pick_two_different_local_group_blocks(const std::string &code_type, int k,
                                                              int r, int z)
{
  int a = -1, b = -1;
  int lg_a = -1;
  for (int bid = 0; bid < k; ++bid) {
    try {
      const int lg = ECProject::get_block_id_to_local_group_id(code_type, k, r, z, bid);
      if (a < 0) {
        a = bid;
        lg_a = lg;
        continue;
      }
      if (lg != lg_a) {
        b = bid;
        break;
      }
    } catch (const std::exception &) {
      continue;
    }
  }
  return {a, b};
}

static bool test_same_group_five(int k, int r, int z, const std::string &code)
{
  auto failed = pick_same_local_group_data_blocks(code, k, r, z, 5);
  if (static_cast<int>(failed.size()) < 5) {
    std::cout << "SKIP: " << code << " could not find 5 same-local-group data blocks" << std::endl;
    return true;
  }
  std::sort(failed.begin(), failed.end());
  auto phases = ECProject::plan_multi_block_recovery(code, k, r, z, failed);
  if (code == "LotusLRC") {
    const std::vector<int> tail = {failed[3], failed[4]};
    return expect_phases("LotusLRC same-group 5 blocks",
                         phases,
                         {ECProject::RecoveryPhaseKind::GlobalMulti,
                          ECProject::RecoveryPhaseKind::GlobalMulti},
                         {failed, tail},
                         {{failed[0], failed[1], failed[2]}, {}});
  }
  return expect_phases((code + " same-group 5 blocks").c_str(),
                       phases,
                       {ECProject::RecoveryPhaseKind::GlobalMulti,
                        ECProject::RecoveryPhaseKind::SingleBlock},
                       {failed, {failed[4]}},
                       {{failed[0], failed[1], failed[2], failed[3]}, {failed[4]}});
}

static std::vector<int> pick_blocks_not_all_same_local_group(const std::string &code_type, int k,
                                                             int r, int z, int min_count)
{
  std::vector<int> picked;
  std::vector<int> local_groups;
  for (int bid = 0; bid < k; ++bid) {
    try {
      const int lg = ECProject::get_block_id_to_local_group_id(code_type, k, r, z, bid);
      picked.push_back(bid);
      local_groups.push_back(lg);
      if (static_cast<int>(picked.size()) >= min_count) {
        const int lg0 = local_groups[0];
        bool all_same = true;
        for (int g : local_groups)
          if (g != lg0) {
            all_same = false;
            break;
          }
        if (!all_same)
          return picked;
      }
    } catch (const std::exception &) {
      continue;
    }
  }
  picked.clear();
  return picked;
}

static bool test_cross_local_group(int k, int r, int z, const std::string &code)
{
  auto failed3 = pick_blocks_not_all_same_local_group(code, k, r, z, 3);
  if (static_cast<int>(failed3.size()) < 3) {
    std::cout << "SKIP: " << code << " could not find 3+ blocks spanning local groups"
              << std::endl;
    return true;
  }
  auto phases = ECProject::plan_multi_block_recovery(code, k, r, z, failed3);
  return expect_phases((code + " cross local group").c_str(),
                       phases,
                       {ECProject::RecoveryPhaseKind::GlobalMulti},
                       {failed3},
                       {failed3});
}

static bool test_two_block_same_group_non_lotus(int k, int r, int z, const std::string &code)
{
  auto failed = pick_same_local_group_data_blocks(code, k, r, z, 2);
  if (failed.size() < 2)
    return true;
  const int first = std::min(failed[0], failed[1]);
  const int second = std::max(failed[0], failed[1]);
  auto phases = ECProject::plan_multi_block_recovery(code, k, r, z, failed);
  return expect_phases((code + " two-block same group GlobalThenSingle").c_str(),
                       phases,
                       {ECProject::RecoveryPhaseKind::GlobalMulti,
                        ECProject::RecoveryPhaseKind::SingleBlock},
                       {failed, {second}},
                       {{first}, {second}});
}

static bool test_lotus_two_block_same_group(int k, int r, int z)
{
  auto failed = pick_same_local_group_data_blocks("LotusLRC", k, r, z, 2);
  if (failed.size() < 2)
    return true;
  auto phases = ECProject::plan_multi_block_recovery("LotusLRC", k, r, z, failed);
  return expect_phases("LotusLRC two-block same group",
                       phases,
                       {ECProject::RecoveryPhaseKind::GlobalMulti},
                       {failed},
                       {{}});
}

static bool test_two_block_different_groups(int k, int r, int z, const std::string &code)
{
  auto pair = pick_two_different_local_group_blocks(code, k, r, z);
  if (pair[0] < 0 || pair[1] < 0)
    return true;
  auto phases = ECProject::plan_multi_block_recovery(code, k, r, z, pair);
  return expect_phases((code + " two-block different groups").c_str(),
                       phases,
                       {ECProject::RecoveryPhaseKind::SingleBlock,
                        ECProject::RecoveryPhaseKind::SingleBlock},
                       {{pair[0]}, {pair[1]}},
                       {{pair[0]}, {pair[1]}});
}

int main()
{
  std::cout << "=== plan_multi_block_recovery unit tests ===" << std::endl;

  const struct {
    std::string code;
    int k, r, z;
  } configs[] = {
      {"AzureLRC", 30, 6, 6},
      {"OptimalLRC", 30, 10, 2},
      {"UniformLRC", 30, 6, 6},
      {"LotusLRC", 72, 3, 8},
  };

  bool ok = true;
  for (const auto &c : configs) {
    std::cout << "\n--- " << c.code << " (k=" << c.k << " r=" << c.r << " z=" << c.z << ") ---"
              << std::endl;
    if (c.code == "LotusLRC") {
      ok = test_same_group_five(c.k, c.r, c.z, c.code) && ok;
      ok = test_lotus_two_block_same_group(c.k, c.r, c.z) && ok;
    } else {
      ok = test_same_group_five(c.k, c.r, c.z, c.code) && ok;
      ok = test_two_block_same_group_non_lotus(c.k, c.r, c.z, c.code) && ok;
    }
    ok = test_cross_local_group(c.k, c.r, c.z, c.code) && ok;
    ok = test_two_block_different_groups(c.k, c.r, c.z, c.code) && ok;
  }

  if (ok) {
    std::cout << "\n=== All plan_multi_block_recovery tests passed ===" << std::endl;
    return 0;
  }
  std::cerr << "\n=== Some tests FAILED ===" << std::endl;
  return 1;
}
