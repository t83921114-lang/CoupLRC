/**
 * Data layout / placement: per-group block counts by code type (AzureLRC, OptimalLRC, UniformLRC, UniLRC, LotusLRC).
 * Used by client and coordinator; no dependency on proto or RPC.
 */
#include "encoder.h"
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <numeric>
namespace ECProject
{

  /* ----- Dispatchers (by code_type) ----- */
  std::vector<int> get_data_block_num_per_group(int k, int r, int z, const std::string &code_type)
  {
    if (code_type == "AzureLRC")
      return get_data_block_num_per_group_azurelrc(k, r, z);
    if (code_type == "OptimalLRC")
      return get_data_block_num_per_group_optimal_lrc(k, r, z);
    if (code_type == "UniformLRC")
      return get_data_block_num_per_group_uniform_lrc(k, r, z);
    if (code_type == "UniLRC")
      return get_data_block_num_per_group_unilrc(k, r, z);
    if (code_type == "LotusLRC")
      return get_data_block_num_per_group_lotuslrc(k, r, z);
    return {};
  }

  std::vector<int> get_global_parity_block_num_per_group(int k, int r, int z, const std::string &code_type)
  {
    if (code_type == "AzureLRC")
      return get_global_parity_block_num_per_group_azurelrc(k, r, z);
    if (code_type == "OptimalLRC")
      return get_global_parity_block_num_per_group_optimal_lrc(k, r, z);
    if (code_type == "UniformLRC")
      return get_global_parity_block_num_per_group_uniform_lrc(k, r, z);
    if (code_type == "UniLRC")
      return get_global_parity_block_num_per_group_unilrc(k, r, z);
    if (code_type == "LotusLRC")
      return get_global_parity_block_num_per_group_lotuslrc(k, r, z);
    return {};
  }

  std::vector<int> get_local_parity_block_num_per_group(int k, int r, int z, const std::string &code_type)
  {
    if (code_type == "AzureLRC")
      return get_local_parity_block_num_per_group_azurelrc(k, r, z);
    if (code_type == "OptimalLRC")
      return get_local_parity_block_num_per_group_optimal_lrc(k, r, z);
    if (code_type == "UniformLRC")
      return get_local_parity_block_num_per_group_uniform_lrc(k, r, z);
    if (code_type == "UniLRC")
      return get_local_parity_block_num_per_group_unilrc(k, r, z);
    if (code_type == "LotusLRC")
      return get_local_parity_block_num_per_group_lotuslrc(k, r, z);
    return {};
  }

  /* ----- AzureLRC ----- */
  std::vector<int> get_data_block_num_per_group_azurelrc(int k, int r, int z)
  {
    return get_data_block_num_per_group_optimal_lrc(k, r, z);
  }

  std::vector<int> get_global_parity_block_num_per_group_azurelrc(int k, int r, int z)
  {
    return get_global_parity_block_num_per_group_optimal_lrc(k, r, z);
  }

  std::vector<int> get_local_parity_block_num_per_group_azurelrc(int k, int r, int z)
  {
    return get_local_parity_block_num_per_group_optimal_lrc(k, r, z); // AzureLRC is the same as OptimalLRC
  }

  std::unordered_map<int, int> get_azurelrc_block_id_to_group_id(int k, int r, int z)
  {
    return get_optimal_lrc_block_id_to_group_id(k, r, z);
  }

  std::unordered_map<int, std::vector<int>> get_azurelrc_group_id_to_block_ids(int k, int r, int z)
  {
    return get_optimal_lrc_group_id_to_block_ids(k, r, z);
  }

  int get_azurelrc_block_id_to_local_group_id(int k, int r, int z, int block_id)
  {
    int local_group_size = k / z;
    if(block_id < k)
      return block_id / local_group_size;
    else if(block_id < k + r)
      return -1; // global parity blocks do not belong to any local group, return -1 to indicate this
    else
      return (block_id - k - r);
    throw std::runtime_error("block id is out of range");
    return -1;
  }

  /* ----- OptimalLRC ----- */
  std::vector<int> get_data_block_num_per_group_optimal_lrc(int k, int r, int z)
  {
    std::vector<int> data_block_num_per_group;
    int group_size = r + 1;
    int local_group_size = k / z;
    int group_num_of_one_local_group = local_group_size / group_size + 1;
    int group_num = z * group_num_of_one_local_group + 1;
    // Evenly split (data + 1 local parity) per rack; last group holds the local parity slot.
    int total_per_rack = local_group_size + 1;
    int base = total_per_rack / group_num_of_one_local_group;
    int rem = total_per_rack % group_num_of_one_local_group;
    for (int i = 0; i < group_num - 1; i++)
    {
      int pos_in_rack = i % group_num_of_one_local_group;
      int slots = base + (pos_in_rack < rem ? 1 : 0);
      bool is_last_in_rack = (pos_in_rack == group_num_of_one_local_group - 1);
      data_block_num_per_group.push_back(is_last_in_rack ? slots - 1 : slots);
    }
    data_block_num_per_group.push_back(0);
    return data_block_num_per_group;
  }

  std::vector<int> get_global_parity_block_num_per_group_optimal_lrc(int k, int r, int z)
  {
    std::vector<int> global_parity_block_num_per_group;
    int group_size = r + 1;
    int local_group_size = k / z;
    int group_num_of_one_local_group = local_group_size / group_size + 1;
    int group_num = z * group_num_of_one_local_group + 1;
    for (int i = 0; i < group_num - 1; i++)
      global_parity_block_num_per_group.push_back(0);
    global_parity_block_num_per_group.push_back(r);
    return global_parity_block_num_per_group;
  }

  std::vector<int> get_local_parity_block_num_per_group_optimal_lrc(int k, int r, int z)
  {
    std::vector<int> local_parity_block_num_per_group;
    int group_size = r + 1;
    int local_group_size = k / z;
    int group_num_of_one_local_group = local_group_size / group_size + 1;
    int group_num = z * group_num_of_one_local_group + 1;
    for (int i = 0; i < group_num - 1; i++)
    {
      if ((i + 1) % group_num_of_one_local_group)
        local_parity_block_num_per_group.push_back(0);
      else
        local_parity_block_num_per_group.push_back(1);
    }
    local_parity_block_num_per_group.push_back(0);
    return local_parity_block_num_per_group;
  }

  int get_optimal_lrc_block_id_to_local_group_id(int k, int r, int z, int block_id)
  {
    int local_group_size = k / z;
    if(block_id < k)
      return block_id / local_group_size;
    else if(block_id < k + r)
      return 0; // global parity blocks repeatedly belong to all local groups, use the first for recovery plan
    else
      return (block_id - k - r);
    throw std::runtime_error("block id is out of range");
    return -1;
  }

  std::unordered_map<int, int> get_optimal_lrc_block_id_to_group_id(int k, int r, int z)
  {
    std::unordered_map<int, int> block_id_to_group_id;
    std::unordered_map<int, std::vector<int>> group_id_to_block_ids = get_optimal_lrc_group_id_to_block_ids(k, r, z);
    for (size_t i = 0; i < group_id_to_block_ids.size(); i++)
      for (size_t j = 0; j < group_id_to_block_ids[(int)i].size(); j++)
        block_id_to_group_id[group_id_to_block_ids[(int)i][(int)j]] = (int)i;
    return block_id_to_group_id;
  }

  std::unordered_map<int, std::vector<int>> get_optimal_lrc_group_id_to_block_ids(int k, int r, int z)
  {
    std::unordered_map<int, std::vector<int>> group_id_to_block_ids;
    std::vector<int> data_block_num_per_group = get_data_block_num_per_group_optimal_lrc(k, r, z);
    std::vector<int> global_parity_block_num_per_group = get_global_parity_block_num_per_group_optimal_lrc(k, r, z);
    std::vector<int> local_parity_block_num_per_group = get_local_parity_block_num_per_group_optimal_lrc(k, r, z);
    int cur_data_block_id = 0;
    int cur_global_parity_block_id = k;
    int cur_local_parity_block_id = k + r;
    for (size_t i = 0; i < data_block_num_per_group.size(); i++){
      std::vector<int> block_ids;
      for (int j = 0; j < data_block_num_per_group[i]; j++){
        block_ids.push_back(cur_data_block_id);
        cur_data_block_id++;
      }
      for (int j = 0; j < global_parity_block_num_per_group[i]; j++){
        block_ids.push_back(cur_global_parity_block_id);
        cur_global_parity_block_id++;
      }
      for (int j = 0; j < local_parity_block_num_per_group[i]; j++){
        block_ids.push_back(cur_local_parity_block_id);
        cur_local_parity_block_id++;
      }
      group_id_to_block_ids[(int)i] = block_ids;
    }
    return group_id_to_block_ids;
  }
  /* ----- UniformLRC ----- */
  std::vector<int> get_data_block_num_per_group_uniform_lrc(int k, int r, int z)
  {
    std::vector<int> data_block_num_per_group;
    std::unordered_map<int, std::vector<int>> group_id_to_block_ids = get_uniform_lrc_group_id_to_block_ids(k, r, z);
    for (size_t i = 0; i < group_id_to_block_ids.size(); i++){
      int data_num = 0;
      for (size_t j = 0; j < group_id_to_block_ids[(int)i].size(); j++){
        if(group_id_to_block_ids[(int)i][j] < k)
          data_num++;
      }
      data_block_num_per_group.push_back(data_num);
    }
    return data_block_num_per_group;
  }

  std::vector<int> get_global_parity_block_num_per_group_uniform_lrc(int k, int r, int z)
  {
    std::vector<int> global_parity_block_num_per_group;
    std::unordered_map<int, std::vector<int>> group_id_to_block_ids = get_uniform_lrc_group_id_to_block_ids(k, r, z);
    for (size_t i = 0; i < group_id_to_block_ids.size(); i++){
      int global_parity_num = 0;
      for (size_t j = 0; j < group_id_to_block_ids[(int)i].size(); j++){
        if(group_id_to_block_ids[(int)i][j] >= k && group_id_to_block_ids[(int)i][j] < k + r)
          global_parity_num++;
      }
      global_parity_block_num_per_group.push_back(global_parity_num);
    }
    return global_parity_block_num_per_group;
  }

  std::vector<int> get_local_parity_block_num_per_group_uniform_lrc(int k, int r, int z)
  {
    std::vector<int> local_parity_block_num_per_group;
    std::unordered_map<int, std::vector<int>> group_id_to_block_ids = get_uniform_lrc_group_id_to_block_ids(k, r, z);
    for (size_t i = 0; i < group_id_to_block_ids.size(); i++){
      int local_parity_num = 0;
      for (size_t j = 0; j < group_id_to_block_ids[(int)i].size(); j++){
        if(group_id_to_block_ids[(int)i][j] >= k + r)
          local_parity_num++;
      }
      local_parity_block_num_per_group.push_back(local_parity_num);
    }
    return local_parity_block_num_per_group;
  }

  std::vector<int> get_uniform_lrc_local_group_sizes(int k, int r, int z)
  {
    std::vector<int> local_group_sizes;
    int group_size = (k + r) / z + 1; // plus 1 for local parity blocks
    int larger_group_num = (k + r) % z;
    for (int i = 0; i < z - larger_group_num; i++)
      local_group_sizes.push_back(group_size);
    for (int i = 0; i < larger_group_num; i++) // put larger local groups in the last positions
      local_group_sizes.push_back(group_size + 1);
    return local_group_sizes;
  }

  int get_uniform_lrc_block_id_to_local_group_id(int k, int r, int z, int block_id)
  {
    if(block_id >= k + r)
      return (block_id - k - r);
    else{
      std::vector<int> local_group_sizes = get_uniform_lrc_local_group_sizes(k, r, z);
      int cur_start = 0;
      for (size_t i = 0; i < local_group_sizes.size(); i++){
        if(block_id < cur_start + local_group_sizes[i] - 1)
          return i;
        cur_start += local_group_sizes[i] - 1; // exclude the local parity block
      }
      throw std::runtime_error("block id is out of range");
      return -1;
    }
  }

  std::unordered_map<int, int> get_uniform_lrc_block_id_to_group_id(int k, int r, int z){
    std::unordered_map<int, int> block_id_to_group_id;
    std::unordered_map<int, std::vector<int>> group_id_to_block_ids = get_uniform_lrc_group_id_to_block_ids(k, r, z);
    for (size_t i = 0; i < group_id_to_block_ids.size(); i++){
      for (size_t j = 0; j < group_id_to_block_ids[(int)i].size(); j++)
        block_id_to_group_id[group_id_to_block_ids[(int)i][(int)j]] = (int)i;
    }
    return block_id_to_group_id;
  }

  std::unordered_map<int, std::vector<int>> get_uniform_lrc_group_id_to_block_ids(int k, int r, int z){
    std::unordered_map<int, std::vector<int>> group_id_to_block_ids;
    std::vector<int> group_sizes = get_uniform_lrc_group_sizes(k, r, z);
    std::vector<int> group_num_per_local_group = get_uniform_lrc_group_num_per_local_group(k, r, z);
    int cur_group_id = 0;
    int cur_block_id = 0;
    for (size_t i = 0; i < group_num_per_local_group.size(); i++){
      for (int j = 0; j < group_num_per_local_group[i]; j++){
        std::vector<int> block_ids;
        for (int l = 0; l < group_sizes[cur_group_id]; l++){
          block_ids.push_back(cur_block_id);
          cur_block_id++;
        }
        if(j == group_num_per_local_group[i] - 1){
          block_ids.erase(block_ids.end() - 1);
          block_ids.push_back(k + r + (int)i); // local parity block
          cur_block_id--; // because we erase the last block
        }
        group_id_to_block_ids[cur_group_id] = block_ids;
        cur_group_id++;
      }
    }
    return group_id_to_block_ids;
  }

  std::vector<int> get_uniform_lrc_group_num_per_local_group(int k, int r, int z)
  {
    std::vector<int> group_num_per_local_group;
    std::vector<int> local_group_sizes = get_uniform_lrc_local_group_sizes(k, r, z);
    int max_capacity = r + 1;
    for (size_t i = 0; i < local_group_sizes.size(); i++)
    {
      int group_num = local_group_sizes[i] / max_capacity + bool(local_group_sizes[i] % max_capacity);
      group_num_per_local_group.push_back(group_num);
    }
    return group_num_per_local_group;
  }

  std::vector<int> get_uniform_lrc_group_sizes(int k, int r, int z)
  {
    std::vector<int> group_sizes;
    std::vector<int> local_group_sizes = get_uniform_lrc_local_group_sizes(k, r, z);
    int max_capacity = r + 1;
    for (size_t i = 0; i < local_group_sizes.size(); i++)
    {
      int remain_size = local_group_sizes[i];
      while (remain_size > 0)
      {
        int put_num = std::min(remain_size, max_capacity);
        group_sizes.push_back(put_num);
        remain_size -= put_num;
      }
    }
    return group_sizes;
  }

  /* ----- UniLRC ----- */
  std::vector<int> get_data_block_num_per_group_unilrc(int k, int r, int z)
  {
    std::vector<int> data_block_num_per_group;
    int local_data_num = k / z;
    for (int i = 0; i < z; i++)
      data_block_num_per_group.push_back(local_data_num);
    return data_block_num_per_group;
  }

  std::vector<int> get_global_parity_block_num_per_group_unilrc(int k, int r, int z)
  {
    std::vector<int> global_parity_block_num_per_group;
    int local_global_parity_num = r / z;
    for (int i = 0; i < z; i++)
      global_parity_block_num_per_group.push_back(local_global_parity_num);
    return global_parity_block_num_per_group;
  }

  std::vector<int> get_local_parity_block_num_per_group_unilrc(int k, int r, int z)
  {
    std::vector<int> local_parity_block_num_per_group;
    for (int i = 0; i < z; i++)
      local_parity_block_num_per_group.push_back(1);
    return local_parity_block_num_per_group;
  }

  /* ----- LotusLRC ----- */
  std::vector<int> get_data_block_num_per_group_lotuslrc(int k, int r, int z)
  {
    std::vector<int> data_block_num_per_group;
    std::vector<int> local_group_sizes = get_lotuslrc_local_group_sizes(k, r, z);
    int local_group_num = local_group_sizes.size();
    std::vector<int> data_block_num_per_local_group = get_data_block_num_per_local_group_lotuslrc(k, r, z);
    std::vector<int> global_parity_block_num_per_local_group = get_global_parity_block_num_per_local_group_lotuslrc(k, r, z);
    std::vector<int> local_parity_block_num_per_local_group = get_local_parity_block_num_per_local_group_lotuslrc(k, r, z);
    std::vector<int> group_sizes = get_lotuslrc_group_sizes(k, r, z);
    std::vector<int> group_num_per_local_group = get_lotuslrc_group_num_per_local_group(k, r, z);
    int cur_group_id = 0;
    for (int i = 0; i < local_group_num; i++)
    {
      int parity_num = global_parity_block_num_per_local_group[i] + local_parity_block_num_per_local_group[i];
      for (int j = 0; j < parity_num; j++)
      {
        data_block_num_per_group.push_back(group_sizes[cur_group_id] - 1);
        cur_group_id++;
      }
      for (int j = 0; j < group_num_per_local_group[i] - parity_num; j++)
      {
        data_block_num_per_group.push_back(group_sizes[cur_group_id]);
        cur_group_id++;
      }
    }
    return data_block_num_per_group;
  }

  std::vector<int> get_global_parity_block_num_per_group_lotuslrc(int k, int r, int z)
  {
    std::vector<int> global_parity_block_num_per_group;
    std::vector<int> local_group_sizes = get_lotuslrc_local_group_sizes(k, r, z);
    int local_group_num = local_group_sizes.size();
    std::vector<int> global_parity_block_num_per_local_group = get_global_parity_block_num_per_local_group_lotuslrc(k, r, z);
    std::vector<int> group_num_per_local_group = get_lotuslrc_group_num_per_local_group(k, r, z);
    for (int i = 0; i < local_group_num; i++)
    {
      int global_parity_num = global_parity_block_num_per_local_group[i];
      for (int j = 0; j < global_parity_num; j++)
        global_parity_block_num_per_group.push_back(1);
      for (int j = 0; j < group_num_per_local_group[i] - global_parity_num; j++)
        global_parity_block_num_per_group.push_back(0);
    }
    return global_parity_block_num_per_group;
  }

  std::vector<int> get_local_parity_block_num_per_group_lotuslrc(int k, int r, int z)
  {
    std::vector<int> local_parity_block_num_per_group;
    std::vector<int> local_group_sizes = get_lotuslrc_local_group_sizes(k, r, z);
    int local_group_num = local_group_sizes.size();
    std::vector<int> global_parity_block_num_per_local_group = get_global_parity_block_num_per_local_group_lotuslrc(k, r, z);
    std::vector<int> local_parity_block_num_per_local_group = get_local_parity_block_num_per_local_group_lotuslrc(k, r, z);
    std::vector<int> group_num_per_local_group = get_lotuslrc_group_num_per_local_group(k, r, z);
    for (int i = 0; i < local_group_num; i++)
    {
      int local_parity_num = local_parity_block_num_per_local_group[i];
      int global_parity_num = global_parity_block_num_per_local_group[i];
      for (int j = 0; j < global_parity_num; j++)
        local_parity_block_num_per_group.push_back(0);
      for (int j = 0; j < local_parity_num; j++)
        local_parity_block_num_per_group.push_back(1);
      for (int j = 0; j < group_num_per_local_group[i] - local_parity_num - global_parity_num; j++)
        local_parity_block_num_per_group.push_back(0);
    }
    return local_parity_block_num_per_group;
  }

  std::unordered_map<int, int> get_lotuslrc_block_id_to_group_id(int k, int r, int z)
  {
    std::unordered_map<int, int> block_id_to_group_id;
    std::unordered_map<int, std::vector<int>> group_id_to_block_ids = get_lotuslrc_group_id_to_block_ids(k, r, z);
    for (size_t i = 0; i < group_id_to_block_ids.size(); i++)
    {
      for (size_t j = 0; j < group_id_to_block_ids[(int)i].size(); j++)
        block_id_to_group_id[group_id_to_block_ids[(int)i][(int)j]] = (int)i;
    }
    return block_id_to_group_id;
  }

  std::unordered_map<int, std::vector<int>> get_lotuslrc_group_id_to_block_ids(int k, int r, int z)
  {
    std::vector<int> data_block_num_per_group = get_data_block_num_per_group_lotuslrc(k, r, z);
    std::vector<int> global_parity_block_num_per_group = get_global_parity_block_num_per_group_lotuslrc(k, r, z);
    std::vector<int> local_parity_block_num_per_group = get_local_parity_block_num_per_group_lotuslrc(k, r, z);
    std::unordered_map<int, std::vector<int>> group_id_to_block_ids;
    int cur_data_block_id = 0;
    int cur_global_parity_block_id = k;
    int cur_local_parity_block_id = k + r;
    for (size_t i = 0; i < data_block_num_per_group.size(); i++)
    {
      std::vector<int> block_ids;
      for (int j = 0; j < data_block_num_per_group[i]; j++)
      {
        block_ids.push_back(cur_data_block_id);
        cur_data_block_id++;
      }
      for (int j = 0; j < global_parity_block_num_per_group[i]; j++)
      {
        block_ids.push_back(cur_global_parity_block_id);
        cur_global_parity_block_id++;
      }
      for (int j = 0; j < local_parity_block_num_per_group[i]; j++)
      {
        block_ids.push_back(cur_local_parity_block_id);
        cur_local_parity_block_id++;
      }
      group_id_to_block_ids[(int)i] = block_ids;
    }
    return group_id_to_block_ids;
  }

  std::vector<int> get_data_block_num_per_local_group_lotuslrc(int k, int r, int z)
  {
    std::vector<int> data_block_num_per_local_group;
    std::vector<int> local_group_sizes = get_lotuslrc_local_group_sizes(k, r, z);
    int local_group_num = local_group_sizes.size();
    if (local_group_num != z / 2)
      throw std::runtime_error("local group num is not equal to z / 2");
    for (int i = 0; i < local_group_num; i++)
    {
      if (i < local_group_num - r)
        data_block_num_per_local_group.push_back(local_group_sizes[i] - 2);
      else
        data_block_num_per_local_group.push_back(local_group_sizes[i] - 3);
    }
    return data_block_num_per_local_group;
  }

  std::vector<int> get_global_parity_block_num_per_local_group_lotuslrc(int k, int r, int z)
  {
    std::vector<int> global_parity_block_num_per_local_group;
    std::vector<int> local_group_sizes = get_lotuslrc_local_group_sizes(k, r, z);
    int local_group_num = local_group_sizes.size();
    if (local_group_num != z / 2)
      throw std::runtime_error("local group num is not equal to z / 2");
    for (int i = 0; i < local_group_num; i++)
    {
      if (i < local_group_num - r)
        global_parity_block_num_per_local_group.push_back(0);
      else
        global_parity_block_num_per_local_group.push_back(1);
    }
    return global_parity_block_num_per_local_group;
  }

  std::vector<int> get_local_parity_block_num_per_local_group_lotuslrc(int k, int r, int z)
  {
    std::vector<int> local_parity_block_num_per_local_group;
    std::vector<int> local_group_sizes = get_lotuslrc_local_group_sizes(k, r, z);
    int local_group_num = local_group_sizes.size();
    if (local_group_num != z / 2)
      throw std::runtime_error("local group num is not equal to z / 2");
    for (int i = 0; i < local_group_num; i++)
      local_parity_block_num_per_local_group.push_back(2);
    return local_parity_block_num_per_local_group;
  }

  int get_lotuslrc_local_group_id_to_block_id(int k, int r, int z, int local_group_id)
  {
    (void)k;
    (void)r;
    (void)z;
    (void)local_group_id;
    return -1; // TODO
  }

  int get_lotuslrc_block_id_to_local_group_id(int k, int r, int z, int block_id)
  {
    if (block_id < k)
    {
      std::vector<int> data_block_num_per_local_group = get_data_block_num_per_local_group_lotuslrc(k, r, z);
      int cur_sum = 0;
      for (size_t i = 0; i < data_block_num_per_local_group.size(); i++)
      {
        cur_sum += data_block_num_per_local_group[i];
        if (block_id < cur_sum)
          return (int)i;
      }
    }
    else if (block_id < k + r)
    {
      std::vector<int> global_parity_block_num_per_local_group = get_global_parity_block_num_per_local_group_lotuslrc(k, r, z);
      int cur_sum = 0;
      for (size_t i = 0; i < global_parity_block_num_per_local_group.size(); i++)
      {
        cur_sum += global_parity_block_num_per_local_group[i];
        if ((block_id - k) < cur_sum)
          return (int)i;
      }
    }
    else
    {
      return (block_id - k - r) / 2;
    }
    throw std::runtime_error("block id is out of range");
  }

  std::vector<int> get_lotuslrc_group_sizes(int k, int r, int z)
  {
    int max_capacity = r + 2;
    std::vector<int> local_group_sizes = get_lotuslrc_local_group_sizes(k, r, z);
    std::vector<int> group_sizes;
    for (size_t i = 0; i < local_group_sizes.size(); i++)
    {
      int group_num = local_group_sizes[i] / max_capacity + bool(local_group_sizes[i] % max_capacity);
      int group_size = local_group_sizes[i] / group_num;
      int larger_group_num = local_group_sizes[i] % group_num;
      for (int j = 0; j < larger_group_num; j++)
        group_sizes.push_back(group_size + 1);
      for (int j = 0; j < group_num - larger_group_num; j++)
        group_sizes.push_back(group_size);
    }
    return group_sizes;
  }

  std::vector<int> get_lotuslrc_local_group_sizes(int k, int r, int z)
  {
    int group_num = z / 2;
    int group_size = (k + r) / group_num + 2;
    int larger_group_num = (k + r) % group_num;
    std::vector<int> group_sizes;
    for (int i = 0; i < group_num - larger_group_num; i++)
      group_sizes.push_back(group_size);
    for (int i = 0; i < larger_group_num; i++)
      group_sizes.push_back(group_size + 1);
    return group_sizes;
  }

  std::vector<int> get_lotuslrc_group_num_per_local_group(int k, int r, int z)
  {
    std::vector<int> group_num_per_local_group;
    int max_capacity = r + 2;
    std::vector<int> local_group_sizes = get_lotuslrc_local_group_sizes(k, r, z);
    for (size_t i = 0; i < local_group_sizes.size(); i++)
    {
      int group_num = local_group_sizes[i] / max_capacity + bool(local_group_sizes[i] % max_capacity);
      group_num_per_local_group.push_back(group_num);
    }
    return group_num_per_local_group;
  }

} // namespace ECProject
