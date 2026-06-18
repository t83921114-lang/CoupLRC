#include "coordinator.h"
#include "tinyxml2.h"
#include <random>
#include <unistd.h>
#include "lrc.h"
#include "encoder.h"
#include <sys/time.h>
#include <chrono>

template <typename T>
inline T ceil(T const &A, T const &B)
{
  return T((A + B - 1) / B);
};

template <typename T>
inline std::vector<size_t> argsort(const std::vector<T> &v)
{
  std::vector<size_t> idx(v.size());
  std::iota(idx.begin(), idx.end(), 0);
  std::sort(idx.begin(), idx.end(), [&v](size_t i1, size_t i2)
            { return v[i1] < v[i2]; });
  return idx;
};

inline int rand_num(int range)
{
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int> dis(0, range - 1);
  int num = dis(gen);
  return num;
};

namespace ECProject
{
  grpc::Status CoordinatorImpl::setParameter(
      grpc::ServerContext *context,
      const coordinator_proto::Parameter *parameter,
      coordinator_proto::RepIfSetParaSuccess *setParameterReply)
  {
    ECSchema system_metadata(parameter->partial_decoding(),
                             (ECProject::EncodeType)parameter->encodetype(),
                             (ECProject::SingleStripePlacementType)parameter->s_stripe_placementtype(),
                             (ECProject::MultiStripesPlacementType)parameter->m_stripe_placementtype(),
                             parameter->k_datablock(),
                             parameter->l_localparityblock(),
                             parameter->g_m_globalparityblock(),
                             parameter->b_datapergroup(),
                             parameter->x_stripepermergegroup());
    m_encode_parameters = system_metadata;
    setParameterReply->set_ifsetparameter(true);
    m_cur_cluster_id = 0;
    m_cur_stripe_id = 0;
    m_object_commit_table.clear();
    m_object_updating_table.clear();
    m_stripe_deleting_table.clear();
    for (auto it = m_cluster_table.begin(); it != m_cluster_table.end(); it++)
    {
      Cluster &t_cluster = it->second;
      t_cluster.blocks.clear();
      t_cluster.stripes.clear();
    }
    for (auto it = m_node_table.begin(); it != m_node_table.end(); it++)
    {
      Node &t_node = it->second;
      t_node.stripes.clear();
    }
    m_stripe_table.clear();
    m_merge_groups.clear();
    m_free_clusters.clear();
    m_agg_start_cid = 0;
    std::cout << "setParameter success" << std::endl;
    return grpc::Status::OK;
  }

  grpc::Status CoordinatorImpl::sayHelloToCoordinator(
      grpc::ServerContext *context,
      const coordinator_proto::RequestToCoordinator *helloRequestToCoordinator,
      coordinator_proto::ReplyFromCoordinator *helloReplyFromCoordinator)
  {
    std::string prefix("Hello ");
    helloReplyFromCoordinator->set_message(prefix + helloRequestToCoordinator->name());
    std::cout << prefix + helloRequestToCoordinator->name() << std::endl;
    return grpc::Status::OK;
  }

  grpc::Status CoordinatorImpl::uploadOriginKeyValue(
      grpc::ServerContext *context,
      const coordinator_proto::RequestProxyIPPort *keyValueSize,
      coordinator_proto::ReplyProxyIPPort *proxyIPPort)
  {

    std::string key = keyValueSize->key();
    m_mutex.lock();
    m_object_commit_table.erase(key);
    m_mutex.unlock();
    int valuesizebytes = keyValueSize->valuesizebytes();

    ObjectInfo new_object;

    int k = m_encode_parameters.k_datablock;
    int g_m = m_encode_parameters.g_m_globalparityblock;
    int l = m_encode_parameters.l_localparityblock;
    // int b = m_encode_parameters.b_datapergroup;
    new_object.object_size = valuesizebytes;
    int block_size = ceil(valuesizebytes, k);

    proxy_proto::ObjectAndPlacement object_placement;
    object_placement.set_key(key);
    object_placement.set_valuesizebyte(valuesizebytes);
    object_placement.set_k(k);
    object_placement.set_g_m(g_m);
    object_placement.set_l(l);
    object_placement.set_encode_type((int)m_encode_parameters.encodetype);
    object_placement.set_block_size(block_size);

    Stripe t_stripe;
    t_stripe.stripe_id = m_cur_stripe_id++;
    t_stripe.k = k;
    t_stripe.l = l;
    t_stripe.g_m = g_m;
    t_stripe.object_keys.push_back(key);
    t_stripe.object_sizes.push_back(valuesizebytes);
    m_stripe_table[t_stripe.stripe_id] = t_stripe;
    new_object.map2stripe = t_stripe.stripe_id;

    int s_cluster_id = generate_placement(t_stripe.stripe_id, block_size);

    Stripe &stripe = m_stripe_table[t_stripe.stripe_id];
    object_placement.set_stripe_id(stripe.stripe_id);
    for (int i = 0; i < int(stripe.blocks.size()); i++)
    {
      object_placement.add_datanodeip(m_node_table[stripe.blocks[i]->map2node].node_ip);
      object_placement.add_datanodeport(m_node_table[stripe.blocks[i]->map2node].node_port);
      object_placement.add_blockkeys(stripe.blocks[i]->block_key);
    }

    grpc::ClientContext cont;
    proxy_proto::SetReply set_reply;
    std::string selected_proxy_ip = m_cluster_table[s_cluster_id].proxy_ip;
    int selected_proxy_port = m_cluster_table[s_cluster_id].proxy_port;
    std::string chosen_proxy = selected_proxy_ip + ":" + std::to_string(selected_proxy_port);
    grpc::Status status = m_proxy_ptrs[chosen_proxy]->encodeAndSetObject(&cont, object_placement, &set_reply);
    proxyIPPort->set_proxyip(selected_proxy_ip);
    proxyIPPort->set_proxyport(selected_proxy_port + ECProject::PROXY_PORT_SHIFT); // use another port to accept data
    if (status.ok())
    {
      m_mutex.lock();
      m_object_updating_table[key] = new_object;
      m_mutex.unlock();
    }
    else
    {
      std::cout << "[SET] Send object placement failed!" << std::endl;
    }

    return grpc::Status::OK;
  }

  void CoordinatorImpl::initialize_optimal_lrc_stripe_placement(Stripe *stripe)
  {
    int k = stripe->k, r = stripe->r, z = stripe->z;
    std::unordered_map<int, int> block_id_to_group_id =
        ECProject::get_optimal_lrc_block_id_to_group_id(k, r, z);

    Block *blocks_info = new Block[stripe->n];
    assert(stripe->object_keys.size() == 1);
    int t_cluster_id = stripe->stripe_id % m_sys_config->ClusterNum;

    for (int i = 0; i < stripe->n; i++)
    {
      blocks_info[i].block_size = m_sys_config->BlockSize;
      blocks_info[i].map2stripe = stripe->stripe_id;
      blocks_info[i].map2key = stripe->object_keys[0];
      blocks_info[i].block_id = i;
      blocks_info[i].map2group = block_id_to_group_id.at(i);

      if (i < stripe->k)
      {
        blocks_info[i].block_type = 'D';
        blocks_info[i].block_key = std::to_string(stripe->stripe_id) + "_D" + std::to_string(i);
      }
      else if (i < stripe->k + stripe->r)
      {
        blocks_info[i].block_type = 'G';
        blocks_info[i].block_key = std::to_string(stripe->stripe_id) + "_G" + std::to_string(i - stripe->k);
      }
      else
      {
        blocks_info[i].block_type = 'L';
        blocks_info[i].block_key = std::to_string(stripe->stripe_id) + "_L" + std::to_string(i - stripe->k - stripe->r);
      }

      blocks_info[i].map2cluster = (t_cluster_id + blocks_info[i].map2group) % m_sys_config->ClusterNum;
      int t_node_id = randomly_select_a_node(blocks_info[i].map2cluster, stripe->stripe_id);
      blocks_info[i].map2node = t_node_id;
      update_stripe_info_in_node(t_node_id, stripe->stripe_id, i);
      m_cluster_table[blocks_info[i].map2cluster].blocks.push_back(&blocks_info[i]);
      m_cluster_table[blocks_info[i].map2cluster].stripes.insert(stripe->stripe_id);
      stripe->blocks.push_back(&blocks_info[i]);
      stripe->place2clusters.insert(blocks_info[i].map2cluster);
      add_to_map(stripe->group_to_blocks, blocks_info[i].map2group, i);
    }

    stripe->num_groups = stripe->group_to_blocks.size();
  }

  void CoordinatorImpl::initialize_uniform_lrc_stripe_placement(Stripe *stripe)
  {
    int k = stripe->k, r = stripe->r, z = stripe->z;
    std::unordered_map<int, int> block_id_to_group_id =
        ECProject::get_uniform_lrc_block_id_to_group_id(k, r, z);

    Block *blocks_info = new Block[stripe->n];
    assert(stripe->object_keys.size() == 1);
    int t_cluster_id = stripe->stripe_id % m_sys_config->ClusterNum;

    for (int i = 0; i < stripe->n; i++)
    {
      blocks_info[i].block_size = m_sys_config->BlockSize;
      blocks_info[i].map2stripe = stripe->stripe_id;
      blocks_info[i].map2key = stripe->object_keys[0];
      blocks_info[i].block_id = i;
      blocks_info[i].map2group = block_id_to_group_id.at(i);

      if (i < stripe->k)
      {
        blocks_info[i].block_type = 'D';
        blocks_info[i].block_key = std::to_string(stripe->stripe_id) + "_D" + std::to_string(i);
      }
      else if (i < stripe->k + stripe->r)
      {
        blocks_info[i].block_type = 'G';
        blocks_info[i].block_key = std::to_string(stripe->stripe_id) + "_G" + std::to_string(i - stripe->k);
      }
      else
      {
        blocks_info[i].block_type = 'L';
        blocks_info[i].block_key = std::to_string(stripe->stripe_id) + "_L" + std::to_string(i - stripe->k - stripe->r);
      }

      blocks_info[i].map2cluster = (t_cluster_id + blocks_info[i].map2group) % m_sys_config->ClusterNum;
      int t_node_id = randomly_select_a_node(blocks_info[i].map2cluster, stripe->stripe_id);
      blocks_info[i].map2node = t_node_id;
      update_stripe_info_in_node(t_node_id, stripe->stripe_id, i);
      m_cluster_table[blocks_info[i].map2cluster].blocks.push_back(&blocks_info[i]);
      m_cluster_table[blocks_info[i].map2cluster].stripes.insert(stripe->stripe_id);
      stripe->blocks.push_back(&blocks_info[i]);
      stripe->place2clusters.insert(blocks_info[i].map2cluster);
      add_to_map(stripe->group_to_blocks, blocks_info[i].map2group, i);
    }

    stripe->num_groups = stripe->group_to_blocks.size();
  }

  void CoordinatorImpl::initialize_lotuslrc_stripe_placement(Stripe *stripe)
  {
    int k = stripe->k, r = stripe->r, z = stripe->z;
    std::unordered_map<int, int> block_id_to_group_id =
        ECProject::get_lotuslrc_block_id_to_group_id(k, r, z);
  
    Block *blocks_info = new Block[stripe->n];
    assert(stripe->object_keys.size() == 1);
    int t_cluster_id = stripe->stripe_id % m_sys_config->ClusterNum;
  
    for (int i = 0; i < stripe->n; i++)
    {
      blocks_info[i].block_size = m_sys_config->BlockSize;
      blocks_info[i].map2stripe = stripe->stripe_id;
      blocks_info[i].map2key = stripe->object_keys[0];
      blocks_info[i].block_id = i;
      blocks_info[i].map2group = block_id_to_group_id.at(i);  // 使用 encoder_layout 的映射
  
      if (i < stripe->k) {
        blocks_info[i].block_type = 'D';
        blocks_info[i].block_key = std::to_string(stripe->stripe_id) + "_D" + std::to_string(i);
      } else if (i < stripe->k + stripe->r) {
        blocks_info[i].block_type = 'G';
        blocks_info[i].block_key = std::to_string(stripe->stripe_id) + "_G" + std::to_string(i - stripe->k);
      } else {
        blocks_info[i].block_type = 'L';
        blocks_info[i].block_key = std::to_string(stripe->stripe_id) + "_L" + std::to_string(i - stripe->k - stripe->r);
      }
  
      blocks_info[i].map2cluster = (t_cluster_id + blocks_info[i].map2group) % m_sys_config->ClusterNum;
      int t_node_id = randomly_select_a_node(blocks_info[i].map2cluster, stripe->stripe_id);
      blocks_info[i].map2node = t_node_id;
      update_stripe_info_in_node(t_node_id, stripe->stripe_id, i);
      m_cluster_table[blocks_info[i].map2cluster].blocks.push_back(&blocks_info[i]);
      m_cluster_table[blocks_info[i].map2cluster].stripes.insert(stripe->stripe_id);
      stripe->blocks.push_back(&blocks_info[i]);
      stripe->place2clusters.insert(blocks_info[i].map2cluster);
      add_to_map(stripe->group_to_blocks, blocks_info[i].map2group, i);
    }
  
    stripe->num_groups = stripe->group_to_blocks.size();
  }

  void CoordinatorImpl::initialize_azurelrc_stripe_placement(Stripe *stripe)
  {
    int k = stripe->k, r = stripe->r, z = stripe->z;
    std::unordered_map<int, int> block_id_to_group_id =
        ECProject::get_azurelrc_block_id_to_group_id(k, r, z);

    Block *blocks_info = new Block[stripe->n];
    assert(stripe->object_keys.size() == 1);
    int t_cluster_id = stripe->stripe_id % m_sys_config->ClusterNum;

    for (int i = 0; i < stripe->n; i++)
    {
      blocks_info[i].block_size = m_sys_config->BlockSize;
      blocks_info[i].map2stripe = stripe->stripe_id;
      blocks_info[i].map2key = stripe->object_keys[0];
      blocks_info[i].block_id = i;
      blocks_info[i].map2group = block_id_to_group_id.at(i);

      if (i < stripe->k)
      {
        blocks_info[i].block_type = 'D';
        blocks_info[i].block_key = std::to_string(stripe->stripe_id) + "_D" + std::to_string(i);
      }
      else if (i < stripe->k + stripe->r)
      {
        blocks_info[i].block_type = 'G';
        blocks_info[i].block_key = std::to_string(stripe->stripe_id) + "_G" + std::to_string(i - stripe->k);
      }
      else
      {
        blocks_info[i].block_type = 'L';
        blocks_info[i].block_key = std::to_string(stripe->stripe_id) + "_L" + std::to_string(i - stripe->k - stripe->r);
      }

      blocks_info[i].map2cluster = (t_cluster_id + blocks_info[i].map2group) % m_sys_config->ClusterNum;
      int t_node_id = randomly_select_a_node(blocks_info[i].map2cluster, stripe->stripe_id);
      blocks_info[i].map2node = t_node_id;
      update_stripe_info_in_node(t_node_id, stripe->stripe_id, i);
      m_cluster_table[blocks_info[i].map2cluster].blocks.push_back(&blocks_info[i]);
      m_cluster_table[blocks_info[i].map2cluster].stripes.insert(stripe->stripe_id);
      stripe->blocks.push_back(&blocks_info[i]);
      stripe->place2clusters.insert(blocks_info[i].map2cluster);
      add_to_map(stripe->group_to_blocks, blocks_info[i].map2group, i);
    }

    stripe->num_groups = stripe->group_to_blocks.size();
  }

  void CoordinatorImpl::initialize_unilrc_stripe_placement(Stripe *stripe)
  {
    int k = stripe->k, r = stripe->r, z = stripe->z;
    Block *blocks_info = new Block[stripe->n];
    assert(stripe->object_keys.size() == 1);
    int t_cluster_id = stripe->stripe_id % m_sys_config->ClusterNum;
    int local_data_num = k / z;
    int local_global_parity_num = r / z;

    for (int i = 0; i < stripe->n; i++)
    {
      blocks_info[i].block_size = m_sys_config->BlockSize;
      blocks_info[i].map2stripe = stripe->stripe_id;
      blocks_info[i].map2key = stripe->object_keys[0];
      blocks_info[i].block_id = i;
      if (i < stripe->k)
        blocks_info[i].map2group = i / local_data_num;
      else if (i < stripe->k + stripe->r)
        blocks_info[i].map2group = (i - stripe->k) / local_global_parity_num;
      else
        blocks_info[i].map2group = (i - stripe->k - stripe->r);
      if (i < stripe->k)
      {
        blocks_info[i].block_type = 'D';
        blocks_info[i].block_key = std::to_string(stripe->stripe_id) + "_D" + std::to_string(i);
      }
      else if (i < stripe->k + stripe->r)
      {
        blocks_info[i].block_type = 'G';
        blocks_info[i].block_key = std::to_string(stripe->stripe_id) + "_G" + std::to_string(i - stripe->k);
      }
      else
      {
        blocks_info[i].block_type = 'L';
        blocks_info[i].block_key = std::to_string(stripe->stripe_id) + "_L" + std::to_string(i - stripe->k - stripe->r);
      }
      blocks_info[i].map2cluster = (t_cluster_id + blocks_info[i].map2group) % m_sys_config->ClusterNum;
      int t_node_id = randomly_select_a_node(blocks_info[i].map2cluster, stripe->stripe_id);
      blocks_info[i].map2node = t_node_id;
      update_stripe_info_in_node(t_node_id, stripe->stripe_id, i);
      m_cluster_table[blocks_info[i].map2cluster].blocks.push_back(&blocks_info[i]);
      m_cluster_table[blocks_info[i].map2cluster].stripes.insert(stripe->stripe_id);
      stripe->blocks.push_back(&blocks_info[i]);
      stripe->place2clusters.insert(blocks_info[i].map2cluster);
      add_to_map(stripe->group_to_blocks, blocks_info[i].map2group, i);
    }

    stripe->num_groups = stripe->group_to_blocks.size();
  }

  void CoordinatorImpl::add_to_map(std::map<int, std::vector<int>> &map, int key, int value)
  {
    if (map.find(key) == map.end())
      map[key] = std::vector<int>();
    map[key].push_back(value);
  }

  int CoordinatorImpl::getClusterAppendSize(Stripe *stripe, const std::map<int, std::pair<int, int>> &block_to_slice_sizes, int curr_group_id, int parity_slice_size)
  {
    int cluster_append_size = 0;

    for (int i = curr_group_id * stripe->k / stripe->z; i < (curr_group_id + 1) * stripe->k / stripe->z; i++)
    {
      if (block_to_slice_sizes.find(i) != block_to_slice_sizes.end())
        cluster_append_size += block_to_slice_sizes.at(i).first;
    }

    cluster_append_size += parity_slice_size * (stripe->r + stripe->z) / stripe->z;
    return cluster_append_size;
  }

  // add repeated fields to plan
  void addBlockToAppendPlan(proxy_proto::AppendStripeDataPlacement &plan,
                            const Block *block,
                            const Node &node,
                            const std::pair<int, int> &slice_info)
  {
    plan.add_datanodeip(node.node_ip);
    plan.add_datanodeport(node.node_port);
    plan.add_blockkeys(block->block_key);
    plan.add_blockids(block->block_id);
    plan.add_offsets(slice_info.second);
    plan.add_sizes(slice_info.first);
  }

  std::vector<proxy_proto::AppendStripeDataPlacement> CoordinatorImpl::generateAppendPlan(Stripe *stripe, int curr_logical_offset, int append_size)
  {
    std::vector<proxy_proto::AppendStripeDataPlacement> append_plans;
    std::string append_mode = m_sys_config->AppendMode;
    int unit_size = m_sys_config->UnitSize;
    int remain_size = stripe->k * m_sys_config->BlockSize - curr_logical_offset;
    assert(remain_size >= append_size && "append size is larger than the remaining size of the stripe!");

    // int curr_group_id = (curr_logical_offset / (unit_size * stripe->k / stripe->z)) % stripe->z;
    int curr_block_id = (curr_logical_offset / unit_size) % stripe->k;
    // compute how many units that need to be appended
    int num_units = (curr_logical_offset + append_size - 1) / unit_size - curr_logical_offset / unit_size + 1;
    // int num_data_groups = std::min((curr_logical_offset + append_size - 1) / (unit_size * stripe->k / stripe->z) - curr_logical_offset / (unit_size * stripe->k / stripe->z) + 1, stripe->z);
    int num_unit_stripes = (curr_logical_offset + append_size - 1) / (unit_size * stripe->k) - curr_logical_offset / (unit_size * stripe->k) + 1;

    // compute the size and offset of the parity slice
    // TODO: optimize the append size that below a unit_size but placed into two units within a unit_stripe
    int parity_slice_size = -1;
    int parity_slice_offset = -1;
    switch (append_mode[0])
    {
    case 'R': // REP_MODE
      parity_slice_size = append_size;
      break;
    case 'U': // UNILRC_MODE
      parity_slice_size = num_unit_stripes * unit_size;
      parity_slice_offset = curr_logical_offset / (unit_size * stripe->k) * unit_size;
      if (num_units == 1)
      {
        parity_slice_size = append_size;
        parity_slice_offset += curr_logical_offset % unit_size;
      }
      if (num_unit_stripes > 1 && (curr_logical_offset + append_size - 1) % (unit_size * stripe->k) < unit_size - 1)
      {
        parity_slice_size = (num_unit_stripes - 1) * unit_size + (curr_logical_offset + append_size - 1) % (unit_size * stripe->k) + 1;
      }
      break;
    case 'C': // CACHED_MODE
      parity_slice_size = num_unit_stripes * unit_size;
      parity_slice_offset = curr_logical_offset / (unit_size * stripe->k) * unit_size;
      break;
    default:
      std::cout << "[ERROR] Invalid append mode: " << append_mode << std::endl;
      return append_plans;
    }

    // key: block_id, value: (slice_size, physical_offset)
    std::map<int, std::pair<int, int>> block_to_slice_sizes;
    int tmp_size = append_size;
    int tmp_offset = curr_logical_offset;
    bool is_merge_parity = curr_logical_offset + append_size == m_sys_config->BlockSize * stripe->k;

    // add data slices to block_to_slice_sizes
    while (tmp_size > 0)
    {
      int sub_slice_size = unit_size;
      // first slice
      if (tmp_size == append_size && curr_logical_offset % unit_size != 0)
      {
        sub_slice_size = std::min(unit_size - curr_logical_offset % unit_size, append_size);
      }
      else
      {
        sub_slice_size = std::min(unit_size, tmp_size);
      }
      if (block_to_slice_sizes.find(curr_block_id) == block_to_slice_sizes.end())
      {
        block_to_slice_sizes[curr_block_id].first = sub_slice_size;
        block_to_slice_sizes[curr_block_id].second = tmp_offset % unit_size + unit_size * (tmp_offset / (stripe->k * unit_size));
      }
      else
      {
        block_to_slice_sizes[curr_block_id].first += sub_slice_size;
      }
      curr_block_id = (curr_block_id + 1) % stripe->k;
      tmp_size -= sub_slice_size;
      tmp_offset += sub_slice_size;
    }

    // add parity slices to block_to_slice_sizes
    for (int i = stripe->k; i < stripe->n; i++)
    {
      block_to_slice_sizes[i].first = parity_slice_size;
      block_to_slice_sizes[i].second = parity_slice_offset;
    }

    for (int i = 0; i < stripe->z; i++)
    {
      proxy_proto::AppendStripeDataPlacement plan;
      plan.set_key(m_toolbox->gen_append_key(stripe->stripe_id, i));
      plan.set_stripe_id(stripe->stripe_id);
      plan.set_append_size(getClusterAppendSize(stripe, block_to_slice_sizes, i, parity_slice_size));
      plan.set_is_merge_parity(is_merge_parity);
      plan.set_cluster_id(stripe->blocks[stripe->group_to_blocks[i][0]]->map2cluster);
      plan.set_append_mode(append_mode);
      if (curr_logical_offset == 0 && append_size == m_sys_config->BlockSize * stripe->k)
      {
        plan.set_is_serialized(false);
        plan.set_is_merge_parity(false);
      }
      else
      {
        plan.set_is_serialized(true);
      }

      // Add data slices to plan
      for (int j = i * stripe->k / stripe->z;
           j < (i + 1) * stripe->k / stripe->z; j++)
      {
        if (block_to_slice_sizes.find(j) != block_to_slice_sizes.end())
        {
          addBlockToAppendPlan(plan, stripe->blocks[j],
                               m_node_table[stripe->blocks[j]->map2node],
                               block_to_slice_sizes.at(j));
        }
      }

      // Add global parity slices to plan
      for (int j = stripe->k + i * stripe->r / stripe->z;
           j < stripe->k + (i + 1) * stripe->r / stripe->z; j++)
      {
        addBlockToAppendPlan(plan, stripe->blocks[j],
                             m_node_table[stripe->blocks[j]->map2node],
                             block_to_slice_sizes.at(j));
      }

      // Add local parity slices to plan
      for (int j = stripe->k + stripe->r + i * stripe->z / stripe->z;
           j < stripe->k + stripe->r + (i + 1) * stripe->z / stripe->z; j++)
      {
        addBlockToAppendPlan(plan, stripe->blocks[j],
                             m_node_table[stripe->blocks[j]->map2node],
                             block_to_slice_sizes.at(j));
      }

      append_plans.push_back(plan);
    }

    return append_plans;
  }

  void CoordinatorImpl::notify_proxies_ready(const proxy_proto::AppendStripeDataPlacement &plan)
  {
    grpc::ClientContext cont;
    proxy_proto::SetReply set_reply;
    std::string chosen_proxy = m_cluster_table[plan.cluster_id()].proxy_ip + ":" + std::to_string(m_cluster_table[plan.cluster_id()].proxy_port);
    grpc::Status status = m_proxy_ptrs[chosen_proxy]->scheduleAppend2Datanode(&cont, plan, &set_reply);
    if (status.ok())
    {
      m_mutex.lock();
      m_object_updating_table[plan.key()] = ObjectInfo(plan.append_size(), plan.stripe_id());
      m_mutex.unlock();
    }
    else
    {
      std::cout << "[APPEND434] Send append plan" << plan.key() << " failed! " << std::endl;
    }
  }

  // Only processing the appending within a single stripe
  grpc::Status CoordinatorImpl::uploadAppendValue(
      grpc::ServerContext *context,
      const coordinator_proto::RequestProxyIPPort *keyValueSize,
      coordinator_proto::ReplyProxyIPsPorts *proxyIPPort)
  {
    std::string clientID = keyValueSize->key();
    int appendSizeBytes = keyValueSize->valuesizebytes();
    std::string append_mode = keyValueSize->append_mode();

    // 1. record metadata
    // logical offset within the block stripe
    if (m_cur_offset_table.find(clientID) == m_cur_offset_table.end())
    {
      // first append
      m_cur_offset_table[clientID] = StripeOffset(m_cur_stripe_id++, 0);
    }
    StripeOffset curStripeOffset = m_cur_offset_table[clientID];

    assert(curStripeOffset.offset + appendSizeBytes <= m_sys_config->BlockSize * m_sys_config->k && "append size is larger than the remaining size of the stripe!");

    // 2. generate data placement
    Stripe *stripe = nullptr;
    if (curStripeOffset.offset == 0)
    {
      // first append
      Stripe t_stripe;
      t_stripe.stripe_id = curStripeOffset.stripe_id;
      t_stripe.n = m_sys_config->n;
      t_stripe.k = m_sys_config->k;
      t_stripe.r = m_sys_config->r;
      t_stripe.z = m_sys_config->z;
      t_stripe.object_keys.push_back(clientID);
      std::string code_type_append = m_sys_config->CodeType;
      if (code_type_append == "UniLRC")
        initialize_unilrc_stripe_placement(&t_stripe);
      else if (code_type_append == "AzureLRC")
        initialize_azurelrc_stripe_placement(&t_stripe);
      else if (code_type_append == "OptimalLRC")
        initialize_optimal_lrc_stripe_placement(&t_stripe);
      else if (code_type_append == "UniformLRC")
        initialize_uniform_lrc_stripe_placement(&t_stripe);
      else if (code_type_append == "LotusLRC")
        initialize_lotuslrc_stripe_placement(&t_stripe);
      m_stripe_table[t_stripe.stripe_id] = t_stripe;
      stripe = &m_stripe_table[t_stripe.stripe_id];
    }
    else
    {
      // append to the existing stripe
      stripe = &m_stripe_table[curStripeOffset.stripe_id];
    }

    std::vector<proxy_proto::AppendStripeDataPlacement> append_plans = generateAppendPlan(stripe, curStripeOffset.offset, appendSizeBytes);
    if (append_plans.empty())
    {
      std::cout << "[ERROR] Invalid append mode: " << append_mode << std::endl;
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid append mode");
    }

    for (const auto &plan : append_plans)
    {
      m_mutex.lock();
      m_object_commit_table.erase(plan.key());
      m_mutex.unlock();
    }

    // 3. notify proxies to receive data
    // need multiple proxies to receive data, so need multiple threads
    std::vector<std::thread> threads;
    int sum_append_size = 0;
    for (const auto &plan : append_plans)
    {
      threads.push_back(std::thread(&CoordinatorImpl::notify_proxies_ready, this, plan));
      proxyIPPort->add_append_keys(plan.key());
      proxyIPPort->add_proxyips(m_cluster_table[plan.cluster_id()].proxy_ip);
      proxyIPPort->add_proxyports(m_cluster_table[plan.cluster_id()].proxy_port + ECProject::PROXY_PORT_SHIFT); // use another port to accept data
      proxyIPPort->add_cluster_slice_sizes(plan.append_size());
      sum_append_size += plan.append_size();
    }
    for (auto &thread : threads)
    {
      thread.join();
    }
    proxyIPPort->set_sum_append_size(sum_append_size);

    m_cur_offset_table[clientID].offset += appendSizeBytes;
    // std::cout << "[Coordinator] stripe_id: " << m_cur_offset_table[clientID].stripe_id << " offset: " << m_cur_offset_table[clientID].offset << " is_erase " << (m_cur_offset_table[clientID].offset == m_sys_config->BlockSize * m_sys_config->k) << std::endl;
    if (m_cur_offset_table[clientID].offset == m_sys_config->BlockSize * m_sys_config->k)
    {
      m_cur_offset_table.erase(clientID);
    }

    return grpc::Status::OK;
  }

  std::vector<proxy_proto::AppendStripeDataPlacement> CoordinatorImpl::generate_add_plans(Stripe *stripe)
  {
    std::vector<proxy_proto::AppendStripeDataPlacement> add_plans;
    for (int i = 0; i < stripe->num_groups; i++)
    {
      proxy_proto::AppendStripeDataPlacement plan;
      int mapped_cluster_id = stripe->blocks[stripe->group_to_blocks[i][0]]->map2cluster;
      size_t append_size = stripe->group_to_blocks[i].size() * m_sys_config->BlockSize;

      plan.set_key(m_toolbox->gen_append_key(stripe->stripe_id, i));
      plan.set_stripe_id(stripe->stripe_id);
      plan.set_append_size(append_size);
      plan.set_is_merge_parity(false);
      plan.set_cluster_id(mapped_cluster_id);
      plan.set_append_mode("UNILRC_MODE");
      plan.set_is_serialized(false);

      for (int j = 0; j < stripe->group_to_blocks[i].size(); j++)
      {
        addBlockToAppendPlan(plan, stripe->blocks[stripe->group_to_blocks[i][j]], m_node_table[stripe->blocks[stripe->group_to_blocks[i][j]]->map2node], std::make_pair(m_sys_config->BlockSize, 0));
      }

      add_plans.push_back(plan);
    }

    return add_plans;
  }

  std::vector<proxy_proto::AppendStripeDataPlacement> CoordinatorImpl::generate_sub_add_plans(Stripe *stripe, size_t subset_size)
  {
    int data_block_num = subset_size / m_sys_config->BlockSize;
    int k = m_sys_config->k;
    int r = m_sys_config->r;
    int z = m_sys_config->z;
    std::vector<proxy_proto::AppendStripeDataPlacement> add_plans;
    for (int i = 0; i < stripe->num_groups; i++)
    {
      proxy_proto::AppendStripeDataPlacement plan;
      int block_num = 0;
      for (int j = 0; j < stripe->group_to_blocks[i].size(); j++)
      {
        int block_id = stripe->group_to_blocks[i][j];
        if(block_id < k && block_id >= data_block_num)
        {
          continue;
        }
        addBlockToAppendPlan(plan, stripe->blocks[stripe->group_to_blocks[i][j]], m_node_table[stripe->blocks[stripe->group_to_blocks[i][j]]->map2node], std::make_pair(m_sys_config->BlockSize, 0));
        block_num++;
      }

      size_t append_size = block_num * m_sys_config->BlockSize;
      if(append_size == 0)
      {
        //plan.set_append_size(0);
        //add_plans.push_back(plan);
        continue; // no data to append
      }

      int mapped_cluster_id = stripe->blocks[stripe->group_to_blocks[i][0]]->map2cluster;

      plan.set_key(m_toolbox->gen_append_key(stripe->stripe_id, i));
      plan.set_stripe_id(stripe->stripe_id);
      plan.set_is_merge_parity(false);
      plan.set_cluster_id(mapped_cluster_id);
      plan.set_append_mode("UNILRC_MODE");
      plan.set_is_serialized(false);
      plan.set_append_size(append_size);

      add_plans.push_back(plan);
    }

    return add_plans;
  }

  void CoordinatorImpl::print_stripe_data_placement(Stripe &stripe)
  {
    std::cout << "Stripe " << stripe.stripe_id << " data placement: " << std::endl;
    for (int i = 0; i < stripe.num_groups; i++)
    {
      std::cout << "Group " << i << ": (" << stripe.group_to_blocks[i].size() << " blocks, mapped to cluster " << stripe.blocks[stripe.group_to_blocks[i][0]]->map2cluster << ") ";
      for (int j = 0; j < stripe.group_to_blocks[i].size(); j++)
      {
        std::cout << stripe.blocks[stripe.group_to_blocks[i][j]]->block_key << " ";
      }
      std::cout << std::endl;
    }
  }

  // set only the full block stripe
  grpc::Status CoordinatorImpl::uploadSetValue(
      grpc::ServerContext *context,
      const coordinator_proto::RequestProxyIPPort *keyValueSize,
      coordinator_proto::ReplyProxyIPsPorts *proxyIPPort)
  {
    std::string clientID = keyValueSize->key();
    size_t setSizeBytes = keyValueSize->valuesizebytes();
    std::string code_type = m_sys_config->CodeType;
    assert(setSizeBytes == static_cast<size_t>(m_sys_config->BlockSize) * static_cast<size_t>(m_sys_config->k) && "set size is not equal to the block stripe size!");
    assert((code_type == "UniLRC" || code_type == "AzureLRC" || code_type == "OptimalLRC" || code_type == "UniformLRC" || code_type == "LotusLRC") && "Error: code type must be UniLRC, AzureLRC, OptimalLRC, UniformLRC, or LotusLRC!");

    Stripe t_stripe;
    t_stripe.stripe_id = m_cur_stripe_id++;
    t_stripe.n = m_sys_config->n;
    t_stripe.k = m_sys_config->k;
    t_stripe.r = m_sys_config->r;
    t_stripe.z = m_sys_config->z;
    t_stripe.object_keys.push_back(clientID);
    if (code_type == "UniLRC")
      initialize_unilrc_stripe_placement(&t_stripe);
    else if (code_type == "AzureLRC")
      initialize_azurelrc_stripe_placement(&t_stripe);
    else if (code_type == "OptimalLRC")
      initialize_optimal_lrc_stripe_placement(&t_stripe);
    else if (code_type == "UniformLRC")
      initialize_uniform_lrc_stripe_placement(&t_stripe);
    else if (code_type == "LotusLRC")
      initialize_lotuslrc_stripe_placement(&t_stripe);

    print_stripe_data_placement(t_stripe);

    std::vector<proxy_proto::AppendStripeDataPlacement> add_plans = generate_add_plans(&t_stripe);

    for (const auto &plan : add_plans)
    {
      m_mutex.lock();
      m_object_commit_table.erase(plan.key());
      m_mutex.unlock();
    }

    std::vector<std::thread> threads;
    size_t sum_append_size = 0;
    for (const auto &plan : add_plans)
    {
      threads.push_back(std::thread(&CoordinatorImpl::notify_proxies_ready, this, plan));
      proxyIPPort->add_append_keys(plan.key());
      proxyIPPort->add_proxyips(m_cluster_table[plan.cluster_id()].proxy_ip);
      proxyIPPort->add_proxyports(m_cluster_table[plan.cluster_id()].proxy_port + ECProject::PROXY_PORT_SHIFT); // use another port to accept data
      proxyIPPort->add_cluster_slice_sizes(plan.append_size());
      sum_append_size += plan.append_size();
    }
    for (auto &thread : threads)
    {
      thread.join();
    }
    proxyIPPort->set_sum_append_size(sum_append_size);

    m_stripe_table[t_stripe.stripe_id] = std::move(t_stripe);

    return grpc::Status::OK;
  }

  grpc::Status CoordinatorImpl::uploadSubsetValue(
      grpc::ServerContext *context,
      const coordinator_proto::RequestProxyIPPort *keyValueSize,
      coordinator_proto::ReplyProxyIPsPorts *proxyIPPort)
  {
    std::string clientID = keyValueSize->key();
    size_t setSizeBytes = keyValueSize->valuesizebytes();
    std::string code_type = m_sys_config->CodeType;
    assert(setSizeBytes <= static_cast<size_t>(m_sys_config->BlockSize) * static_cast<size_t>(m_sys_config->k) && "subset size is larger than the block size!");
    assert((code_type == "UniLRC" || code_type == "AzureLRC" || code_type == "OptimalLRC" || code_type == "UniformLRC") && "Error: code type must be UniLRC, AzureLRC, OptimalLRC, or UniformLRC!");

    Stripe t_stripe;
    t_stripe.stripe_id = m_cur_stripe_id++;
    t_stripe.n = m_sys_config->n;
    t_stripe.k = m_sys_config->k;
    t_stripe.r = m_sys_config->r;
    t_stripe.z = m_sys_config->z;
    t_stripe.object_keys.push_back(clientID);
    if (code_type == "UniLRC")
      initialize_unilrc_stripe_placement(&t_stripe);
    else if (code_type == "AzureLRC")
      initialize_azurelrc_stripe_placement(&t_stripe);
    else if (code_type == "OptimalLRC")
      initialize_optimal_lrc_stripe_placement(&t_stripe);
    else if (code_type == "UniformLRC")
      initialize_uniform_lrc_stripe_placement(&t_stripe);
    else if (code_type == "LotusLRC")
      initialize_lotuslrc_stripe_placement(&t_stripe);

    print_stripe_data_placement(t_stripe);

    std::vector<proxy_proto::AppendStripeDataPlacement> add_plans = generate_sub_add_plans(&t_stripe, setSizeBytes);

    for (const auto &plan : add_plans)
    {
      m_mutex.lock();
      m_object_commit_table.erase(plan.key());
      m_mutex.unlock();
    }

    std::vector<std::thread> threads;
    size_t sum_append_size = 0;
    for (const auto &plan : add_plans)
    {
      threads.push_back(std::thread(&CoordinatorImpl::notify_proxies_ready, this, plan));
      proxyIPPort->add_append_keys(plan.key());
      proxyIPPort->add_proxyips(m_cluster_table[plan.cluster_id()].proxy_ip);
      proxyIPPort->add_proxyports(m_cluster_table[plan.cluster_id()].proxy_port + ECProject::PROXY_PORT_SHIFT); // use another port to accept data
      proxyIPPort->add_cluster_slice_sizes(plan.append_size());
      //proxyIPPort->add_group_ids(group_id);
      sum_append_size += plan.append_size();
      //group_id++;
    }
    for (auto &thread : threads)
    {
      thread.join();
    }
    proxyIPPort->set_sum_append_size(sum_append_size);

    m_stripe_table[t_stripe.stripe_id] = std::move(t_stripe);

    return grpc::Status::OK;
  }
  
  std::vector<int> CoordinatorImpl::get_recovery_group_ids(std::string code_type, int k, int r, int z, int failed_block_id)
  {
    std::vector<int> recovery_group_ids;
    if (code_type == "AzureLRC")
    {
      if (failed_block_id >= k && failed_block_id < k + r)
      {
        for (int i = 1; i <= z; i++)
        {
          recovery_group_ids.push_back(i);
        }
      }
      else if (failed_block_id >= k + r)
      {
        recovery_group_ids.push_back(failed_block_id - k - r);
      }
      else
      {
        recovery_group_ids.push_back(failed_block_id / (k / z));
      }
    }
    else if (code_type == "UniLRC")
    {
      if (failed_block_id >= k && failed_block_id < k + r)
      {
        recovery_group_ids.push_back((failed_block_id - k) / (r / z));
      }
      else if (failed_block_id >= k + r)
      {
        recovery_group_ids.push_back(failed_block_id - k - r);
      }
      else
      {
        recovery_group_ids.push_back(failed_block_id / (k / z));
      }
    }
    else if (code_type == "OptimalLRC")
    {
      if (failed_block_id >= k && failed_block_id < k + r)
      {
        int group_num = (k / z / (r + 1) + (bool)(k / z % (r + 1))) * z + 1;
        recovery_group_ids.push_back(group_num - 1);
        for (int i = 0; i < group_num / z; i++)
        {
          recovery_group_ids.push_back(i);
        }
      }
      else if (failed_block_id >= k + r)
      {
        int local_group_size = k / z;
        int local_group_id = (failed_block_id - k - r);
        int group_num_of_one_local_group = local_group_size / (r + 1) + 1;
        int group_num = z * group_num_of_one_local_group + 1;
        recovery_group_ids.push_back((local_group_id + 1) * group_num_of_one_local_group - 1);
        for (int i = local_group_id * group_num_of_one_local_group; i < (local_group_id + 1) * group_num_of_one_local_group - 1; i++)
        {
          recovery_group_ids.push_back(i);
        }
        recovery_group_ids.push_back(group_num - 1);
      }
      else
      {
        int local_group_size = k / z;
        int group_num_of_one_local_group = local_group_size / (r + 1) + 1;
        int local_group_id = failed_block_id / local_group_size;
        int group_id_in_local_group = failed_block_id % local_group_size / (r + 1);
        recovery_group_ids.push_back(local_group_id * group_num_of_one_local_group + group_id_in_local_group);
        for (int i = 0; i < group_num_of_one_local_group; i++)
        {
          if (i != group_id_in_local_group)
          {
            recovery_group_ids.push_back(local_group_id * group_num_of_one_local_group + i);
          }
        }
        int group_num = z * group_num_of_one_local_group + 1;
        recovery_group_ids.push_back(group_num - 1);
      }
    }
    else if (code_type == "UniformLRC")
    {
      if (failed_block_id >= k + r)
      {
        int larger_local_group_num = (k + r) % z;
        int local_group_id = failed_block_id - k - r;
        int local_group_size = (k + r) / z;
        int group_num_of_one_local_group = local_group_size / r + bool(local_group_size % r);
        if (local_group_id + larger_local_group_num < z)
        {
          recovery_group_ids.push_back((local_group_id + 1) * group_num_of_one_local_group - 1);
          for (int i = local_group_id * group_num_of_one_local_group; i < (local_group_id + 1) * group_num_of_one_local_group - 1; i++)
          {
            recovery_group_ids.push_back(i);
          }
        }
        else
        {
          int smaller_local_group_num = z - larger_local_group_num;
          int group_num_of_all_small_group = smaller_local_group_num * group_num_of_one_local_group;
          local_group_size++;
          group_num_of_one_local_group = local_group_size / r + (bool)(local_group_size % r);
          local_group_id = local_group_id - smaller_local_group_num;
          recovery_group_ids.push_back(group_num_of_all_small_group + (local_group_id + 1) * group_num_of_one_local_group - 1);
          for (int i = group_num_of_all_small_group + local_group_id * group_num_of_one_local_group; i < group_num_of_all_small_group + (local_group_id + 1) * group_num_of_one_local_group - 1; i++)
          {
            recovery_group_ids.push_back(i);
          }
        }
      }
      else if (failed_block_id < k + r)
      {
        int larger_local_group_num = (k + r) % z;
        int smaller_local_group_num = z - larger_local_group_num;
        int local_group_size = (k + r) / z;
        int group_num_of_one_local_group = local_group_size / r + bool(local_group_size % r);
        int block_num_of_smaller_local_group = (z - larger_local_group_num) * local_group_size;
        int group_num_of_smaller_local_group = smaller_local_group_num * group_num_of_one_local_group;
        int local_group_id = 0;
        if (failed_block_id < block_num_of_smaller_local_group)
        {
          local_group_id = failed_block_id / local_group_size;
          int block_num_in_previous_local_group = local_group_id * local_group_size;
          int group_id = local_group_id * group_num_of_one_local_group + (failed_block_id - block_num_in_previous_local_group) / r;
          recovery_group_ids.push_back(group_id);
          for (int i = local_group_id * group_num_of_one_local_group; i < local_group_id * group_num_of_one_local_group + group_num_of_one_local_group; i++)
          {
            if (i != group_id)
            {
              recovery_group_ids.push_back(i);
            }
          }
        }
        else
        {
          local_group_size++;
          group_num_of_one_local_group = local_group_size / r + bool(local_group_size % r);
          local_group_id = (failed_block_id - block_num_of_smaller_local_group) / local_group_size;
          int block_num_in_previous_local_group = local_group_id * local_group_size + block_num_of_smaller_local_group;
          int group_id = local_group_id * group_num_of_one_local_group + (failed_block_id - block_num_in_previous_local_group) / r;
          recovery_group_ids.push_back(group_id + group_num_of_smaller_local_group);
          for (int i = local_group_id * group_num_of_one_local_group; i < local_group_id * group_num_of_one_local_group + group_num_of_one_local_group; i++)
          {
            if (i != group_id)
            {
              recovery_group_ids.push_back(i + group_num_of_smaller_local_group);
            }
          }
        }
      }
    }

    return recovery_group_ids;
  }

  void CoordinatorImpl::init_recovery_group_lookup_table()
  {
    for (int i = 0; i < m_sys_config->n; i++)
    {
      m_recovery_group_lookup_table[i] = get_recovery_group_ids(m_sys_config->CodeType, m_sys_config->k, m_sys_config->r, m_sys_config->z, i);
    }
  }

  void CoordinatorImpl::getStripeFromProxy(std::string client_ip, int client_port, std::string proxy_ip, int proxy_port, int stripe_id, int group_id, std::vector<int> block_ids)
  {
    std::cout << "[GET] getting stripe " << stripe_id << " from proxy " << proxy_ip << ":" << proxy_port << std::endl;
    for(int i = 0; i < block_ids.size(); i++){
      std::cout << "block_id: " << block_ids[i] << std::endl;
    }
    grpc::ClientContext cont;
    proxy_proto::StripeAndBlockIDs stripe_block_ids;
    proxy_proto::GetReply stripe_reply;
    stripe_block_ids.set_stripe_id(stripe_id);
    stripe_block_ids.set_clientip(client_ip);
    stripe_block_ids.set_clientport(client_port);
    stripe_block_ids.set_group_id(group_id);

    for (int i = 0; i < block_ids.size(); i++)
    {
      stripe_block_ids.add_block_ids(block_ids[i]);
      stripe_block_ids.add_block_keys(m_stripe_table[stripe_id].blocks[block_ids[i]]->block_key);
      stripe_block_ids.add_datanodeips(m_node_table[m_stripe_table[stripe_id].blocks[block_ids[i]]->map2node].node_ip);
      stripe_block_ids.add_datanodeports(m_node_table[m_stripe_table[stripe_id].blocks[block_ids[i]]->map2node].node_port);
    }
    grpc::Status status = m_proxy_ptrs[proxy_ip + ":" + std::to_string(proxy_port)]->getBlocks(&cont, stripe_block_ids, &stripe_reply);
    if (status.ok())
    {
      std::cout << "[GET] getting stripe " << stripe_id << " from proxy " << proxy_ip << ":" << proxy_port << " succeeded!" << std::endl;
    }
    else
    {
      std::cout << "[GET] getting stripe " << stripe_id << " from proxy " << proxy_ip << ":" << proxy_port << " failed!" << std::endl;
    }
  }


  grpc::Status 
  CoordinatorImpl::getStripe(
      grpc::ServerContext *context,
      const coordinator_proto::KeyAndClientIP *keyClient,
      coordinator_proto::ReplyProxyIPsPorts *proxyIPPort)
  {

    //std::chrono::high_resolution_clock::time_point start = std::chrono::high_resolution_clock::now();
    int stripe_id = std::stoi(keyClient->key());
    Stripe &t_stripe = m_stripe_table[stripe_id];
    int k = t_stripe.k;
    std::string code_type = m_sys_config->CodeType;
    //std::cout << "[GET] getting stripe " << stripe_id << " with " << num_data_groups << " data groups" << std::endl;
    std::vector<int> block_num_per_group = ECProject::get_data_block_num_per_group(k, m_sys_config->r, m_sys_config->z, code_type);
    int num_data_groups = block_num_per_group.size();
    std::vector<int> get_cluster_ids;
    for (int i = 0; i < num_data_groups; i++)
    {
      get_cluster_ids.push_back(t_stripe.blocks[t_stripe.group_to_blocks[i][0]]->map2cluster);
      //std::cout << "group " << i << " is mapped to cluster " << get_cluster_ids[i] << std::endl;
    }
    for (int i = 0; i < num_data_groups; i++)
    {
      proxyIPPort->add_proxyips(m_cluster_table[get_cluster_ids[i]].proxy_ip);
      proxyIPPort->add_proxyports(m_cluster_table[get_cluster_ids[i]].proxy_port);
      proxyIPPort->add_cluster_slice_sizes(block_num_per_group[i]);
    }
    /*for(int i = 0; i < t_stripe.num_groups; i++){
      m_proxy_ptrs[proxyIPPort->proxyips(i) + ":" + std::to_string(proxyIPPort->proxyports(i))]->getStripe(stripe_id, t_stripe.group_to_blocks[i]);
    }*/
    std::vector<std::thread> threads;
    for (int i = 0; i < num_data_groups; i++)
    {
      if(block_num_per_group[i] == 0){
        continue;
      }
      std::vector<int> block_ids;
      for (int j = 0; j < t_stripe.group_to_blocks[i].size(); j++)
      {
        if(t_stripe.blocks[t_stripe.group_to_blocks[i][j]]->block_id < k){
          block_ids.push_back(t_stripe.group_to_blocks[i][j]);
        }
      }
      threads.push_back(std::thread(&CoordinatorImpl::getStripeFromProxy, this, keyClient->clientip(), keyClient->clientport(), 
        proxyIPPort->proxyips(i), proxyIPPort->proxyports(i), stripe_id, i, block_ids));
    }
    for (auto &thread : threads)
    {
      thread.detach();
    }
    /*std::chrono::high_resolution_clock::time_point end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = std::chrono::duration_cast<std::chrono::duration<double>>(end - start);
    std::cout << "[GET] getting stripe " << stripe_id << " took " << duration.count() << " seconds" << std::endl;*/

    return grpc::Status::OK;
  }
  
  grpc::Status
  CoordinatorImpl::getBlocks(
      grpc::ServerContext *context,
      const coordinator_proto::BlockIDsAndClientIP *blockIDsClient,
      coordinator_proto::ReplyProxyIPsPorts *proxyIPPort
  )
  {
    std::string client_ip = blockIDsClient->clientip();
    int client_port = blockIDsClient->clientport();
    int start_block_id = blockIDsClient->start_block_id();
    int end_block_id = blockIDsClient->end_block_id();
    std::vector<int> stripe_ids;
    std::vector<int> block_ids;
    std::vector<int> relative_block_ids;
    for(int i = start_block_id; i <= end_block_id; i++){
      int stripe_id = i / m_sys_config->k;
      stripe_ids.push_back(stripe_id);
      block_ids.push_back(i % m_sys_config->k);
      relative_block_ids.push_back(i - start_block_id);
    }
    std::vector<int> get_cluster_ids;
    std::vector<int> unique_cluster_ids;
    for (int i = 0; i < stripe_ids.size(); i++)
    {
      get_cluster_ids.push_back(m_stripe_table[stripe_ids[i]].blocks[block_ids[i]]->map2cluster);
      if(std::find(unique_cluster_ids.begin(), unique_cluster_ids.end(), get_cluster_ids[i]) == unique_cluster_ids.end()){
        unique_cluster_ids.push_back(get_cluster_ids[i]);
      }
    }
    proxy_proto::StripeAndBlockIDs stripe_block_ids[unique_cluster_ids.size()];
    for(int i = 0; i < stripe_ids.size(); i++){
      int idx = std::find(unique_cluster_ids.begin(), unique_cluster_ids.end(), get_cluster_ids[i]) - unique_cluster_ids.begin();
      stripe_block_ids[idx].add_block_ids(relative_block_ids[i]);
      stripe_block_ids[idx].add_block_keys(m_stripe_table[stripe_ids[i]].blocks[block_ids[i]]->block_key);
      stripe_block_ids[idx].add_datanodeips(m_node_table[m_stripe_table[stripe_ids[i]].blocks[block_ids[i]]->map2node].node_ip);
      stripe_block_ids[idx].add_datanodeports(m_node_table[m_stripe_table[stripe_ids[i]].blocks[block_ids[i]]->map2node].node_port);
    }
    std::vector<std::thread> get_threads;
    for(int i = 0; i < unique_cluster_ids.size(); i++){
      get_threads.push_back(std::thread([this, &stripe_block_ids, &client_ip, &client_port, &proxyIPPort, &unique_cluster_ids, i](){
        grpc::ClientContext cont;
        proxy_proto::GetReply stripe_reply;
        stripe_block_ids[i].set_clientip(client_ip);
        stripe_block_ids[i].set_clientport(client_port);
        grpc::Status status = m_proxy_ptrs[m_cluster_table[unique_cluster_ids[i]].proxy_ip + ":" + std::to_string(m_cluster_table[unique_cluster_ids[i]].proxy_port)]->getBlocks(&cont, stripe_block_ids[i], &stripe_reply);
        if (status.ok())
        {
          std::cout << "[GET] getting blocks from proxy " << m_cluster_table[unique_cluster_ids[i]].proxy_ip << ":" << m_cluster_table[unique_cluster_ids[i]].proxy_port << " succeeded!" << std::endl;
        }
        else
        {
          std::cout << "[GET] getting blocks from proxy " << m_cluster_table[unique_cluster_ids[i]].proxy_ip << ":" << m_cluster_table[unique_cluster_ids[i]].proxy_port << " failed!" << std::endl;
        }
      }));
    }
    for (auto &thread : get_threads)
    {
      thread.join();
    }
    return grpc::Status::OK;

  }

  grpc::Status
  CoordinatorImpl::getDegradedReadBlocks(
      grpc::ServerContext *context,
      const coordinator_proto::BlockIDsAndClientIP *blockIDsClient,
      coordinator_proto::ReplyProxyIPsPorts *proxyIPPort
  )
  {
    std::string client_ip = blockIDsClient->clientip();
    int client_port = blockIDsClient->clientport();
    int start_block_id = blockIDsClient->start_block_id();
    int end_block_id = blockIDsClient->end_block_id();
    std::vector<int> stripe_ids;
    std::vector<int> block_ids;
    std::vector<int> relative_block_ids;
    for(int i = start_block_id; i <= end_block_id; i++){
      int stripe_id = i / m_sys_config->k;
      stripe_ids.push_back(stripe_id);
      block_ids.push_back(i % m_sys_config->k);
      relative_block_ids.push_back(i - start_block_id);
    }
    for(int i = 0; i < stripe_ids.size(); i++){
      degraded_read_one_block_for_workload(stripe_ids[i], block_ids[i], client_ip, client_port, relative_block_ids[i]);
    }
    return grpc::Status::OK;

  }


  grpc::Status
  CoordinatorImpl::getValue(
      grpc::ServerContext *context,
      const coordinator_proto::KeyAndClientIP *keyClient,
      coordinator_proto::RepIfGetSuccess *getReplyClient)
  {
    try
    {
      std::string key = keyClient->key();
      std::string client_ip = keyClient->clientip();
      int client_port = keyClient->clientport();
      ObjectInfo object_info;
      m_mutex.lock();
      object_info = m_object_commit_table.at(key);
      m_mutex.unlock();
      int k = m_encode_parameters.k_datablock;
      int g_m = m_encode_parameters.g_m_globalparityblock;
      int l = m_encode_parameters.l_localparityblock;
      // int b = m_encode_parameters.b_datapergroup;

      grpc::ClientContext decode_and_get;
      proxy_proto::ObjectAndPlacement object_placement;
      grpc::Status status;
      proxy_proto::GetReply get_reply;
      getReplyClient->set_valuesizebytes(object_info.object_size);
      object_placement.set_key(key);
      object_placement.set_valuesizebyte(object_info.object_size);
      object_placement.set_k(k);
      object_placement.set_l(l);
      object_placement.set_g_m(g_m);
      object_placement.set_stripe_id(object_info.map2stripe);
      object_placement.set_encode_type(m_encode_parameters.encodetype);
      object_placement.set_clientip(client_ip);
      object_placement.set_clientport(client_port);
      Stripe &t_stripe = m_stripe_table[object_info.map2stripe];
      std::unordered_set<int> t_cluster_set;
      for (int i = 0; i < int(t_stripe.blocks.size()); i++)
      {
        if (t_stripe.blocks[i]->map2key == key)
        {
          object_placement.add_datanodeip(m_node_table[t_stripe.blocks[i]->map2node].node_ip);
          object_placement.add_datanodeport(m_node_table[t_stripe.blocks[i]->map2node].node_port);
          object_placement.add_blockkeys(t_stripe.blocks[i]->block_key);
          object_placement.add_blockids(t_stripe.blocks[i]->block_id);
          t_cluster_set.insert(t_stripe.blocks[i]->map2cluster);
        }
      }
      // randomly select a cluster
      int idx = rand_num(int(t_cluster_set.size()));
      int r_cluster_id = *(std::next(t_cluster_set.begin(), idx));
      std::string chosen_proxy = m_cluster_table[r_cluster_id].proxy_ip + ":" + std::to_string(m_cluster_table[r_cluster_id].proxy_port);
      status = m_proxy_ptrs[chosen_proxy]->decodeAndGetObject(&decode_and_get, object_placement, &get_reply);
      if (status.ok())
      {
        std::cout << "[GET] getting value of " << key << std::endl;
      }
    }
    catch (std::exception &e)
    {
      std::cout << "getValue exception" << std::endl;
      std::cout << e.what() << std::endl;
    }
    return grpc::Status::OK;
  }

  int CoordinatorImpl::get_cluster_id_by_group_id(Stripe &t_stripe, int group_id)
  {
    int block_id = t_stripe.group_to_blocks[group_id][0];
    return t_stripe.blocks[block_id]->map2cluster;
  }

  void CoordinatorImpl::add_block_list_to_recovery_request(Stripe &stripe, const std::vector<int> &block_ids, proxy_proto::RecoveryRequest *request)
  {
    for (int bid : block_ids)
    {
      Block *t_block = stripe.blocks[bid];
      request->add_datanodeip(m_node_table[t_block->map2node].node_ip);
      request->add_datanodeport(m_node_table[t_block->map2node].node_port);
      request->add_blockkeys(t_block->block_key);
      request->add_blockids(t_block->block_id);
    }
  }

  void CoordinatorImpl::add_block_list_to_degraded_read_request(Stripe &stripe, const std::vector<int> &block_ids, proxy_proto::DegradedReadRequest *request)
  {
    for (int bid : block_ids)
    {
      Block *t_block = stripe.blocks[bid];
      request->add_datanodeip(m_node_table[t_block->map2node].node_ip);
      request->add_datanodeport(m_node_table[t_block->map2node].node_port);
      request->add_blockkeys(t_block->block_key);
      request->add_blockids(t_block->block_id);
    }
  }

  namespace {

  struct RecoveryBreakdownSamples
  {
    std::vector<double> disk_io_start_time;
    std::vector<double> disk_io_end_time;
    std::vector<double> decode_start_time;
    std::vector<double> decode_end_time;
    std::vector<double> network_start_time;
    std::vector<double> network_end_time;
    std::vector<double> grpc_notify_time;
    std::vector<double> grpc_start_time;
    std::vector<double> data_node_grpc_notify_time;
    std::vector<double> data_node_grpc_start_time;
    double cross_rack_network_time = 0.0;
    double cross_rack_xor_time = 0.0;
    double dest_data_node_network_time = 0.0;
    double dest_data_node_disk_io_time = 0.0;

    void append_recovery_reply(const proxy_proto::RecoveryReply &reply, double grpc_notify)
    {
      disk_io_start_time.push_back(reply.disk_io_start_time());
      disk_io_end_time.push_back(reply.disk_io_end_time());
      decode_start_time.push_back(reply.decode_start_time());
      decode_end_time.push_back(reply.decode_end_time());
      network_start_time.push_back(reply.network_start_time());
      network_end_time.push_back(reply.network_end_time());
      grpc_notify_time.push_back(grpc_notify);
      grpc_start_time.push_back(reply.grpc_start_time());
      data_node_grpc_notify_time.push_back(reply.data_node_grpc_notify_time());
      data_node_grpc_start_time.push_back(reply.data_node_grpc_start_time());
      cross_rack_network_time = std::max(cross_rack_network_time, reply.cross_rack_time());
      cross_rack_xor_time = std::max(cross_rack_xor_time, reply.cross_rack_xor_time());
      dest_data_node_network_time = std::max(dest_data_node_network_time, reply.dest_data_node_network_time());
      dest_data_node_disk_io_time = std::max(dest_data_node_disk_io_time, reply.dest_data_node_disk_io_time());
    }

    void append_degraded_read_reply(const proxy_proto::DegradedReadReply &reply, double grpc_notify)
    {
      disk_io_start_time.push_back(reply.disk_io_start_time());
      disk_io_end_time.push_back(reply.disk_io_end_time());
      decode_start_time.push_back(reply.decode_start_time());
      decode_end_time.push_back(reply.decode_end_time());
      network_start_time.push_back(reply.network_start_time());
      network_end_time.push_back(reply.network_end_time());
      grpc_notify_time.push_back(grpc_notify);
      grpc_start_time.push_back(reply.grpc_start_time());
      data_node_grpc_notify_time.push_back(reply.data_node_grpc_notify_time());
      data_node_grpc_start_time.push_back(reply.data_node_grpc_start_time());
      cross_rack_network_time = std::max(cross_rack_network_time, reply.cross_rack_time());
      cross_rack_xor_time = std::max(cross_rack_xor_time, reply.cross_rack_xor_time());
    }

    void fill_recovery_reply(coordinator_proto::RecoveryReply *reply) const
    {
      if (disk_io_start_time.empty())
        return;
      const double max_disk_io_time =
          *std::max_element(disk_io_end_time.begin(), disk_io_end_time.end()) -
          *std::min_element(disk_io_start_time.begin(), disk_io_start_time.end());
      reply->set_disk_read_time(max_disk_io_time);
      const double max_decode_time =
          *std::max_element(decode_end_time.begin(), decode_end_time.end()) -
          *std::min_element(decode_start_time.begin(), decode_start_time.end());
      reply->set_decode_time(max_decode_time + cross_rack_xor_time);
      const double max_network_time =
          *std::max_element(network_end_time.begin(), network_end_time.end()) -
          *std::min_element(network_start_time.begin(), network_start_time.end());
      const double max_grpc_delay =
          *std::max_element(grpc_start_time.begin(), grpc_start_time.end()) -
          *std::min_element(grpc_notify_time.begin(), grpc_notify_time.end());
      const double max_data_node_grpc_delay =
          *std::max_element(data_node_grpc_start_time.begin(), data_node_grpc_start_time.end()) -
          *std::min_element(data_node_grpc_notify_time.begin(), data_node_grpc_notify_time.end());
      reply->set_network_time(max_network_time + cross_rack_network_time + dest_data_node_network_time +
                              max_grpc_delay + max_data_node_grpc_delay);
      reply->set_disk_write_time(dest_data_node_disk_io_time);
    }
  };

  } // namespace

  bool CoordinatorImpl::recovery_one_block_breakdown(int stripe_id, int failed_block_id,
    std::vector<double> &disk_io_start_time, std::vector<double> &disk_io_end_time, std::vector<double> &decode_start_time, std::vector<double> &decode_end_time,
    std::vector<double> &network_start_time, std::vector<double> &network_end_time, double &cross_rack_network_time, double &cross_rack_xor_time,
    std::vector<double> &grpc_notify_time, std::vector<double> &grpc_start_time, std::vector<double> &data_node_grpc_notify_time, std::vector<double> &data_node_grpc_start_time,
    double &dest_data_node_network_time, double &dest_data_node_disk_io_time)
  {
    std::string code_type = m_sys_config->CodeType;
    Stripe &t_stripe = m_stripe_table[stripe_id];
    std::vector<int> recovery_group_ids = get_recovery_group_ids(m_sys_config->CodeType, m_sys_config->k, m_sys_config->r, m_sys_config->z, failed_block_id);
    grpc::Status status;

    if (recovery_group_ids.size() == 1)
    {
      //assert((code_type == "UniLRC") || (code_type == "AzureLRC" && (failed_block_id < m_sys_config->k || failed_block_id >= m_sys_config->k + m_sys_config->r)));

      grpc::ClientContext recovery_context;
      proxy_proto::RecoveryRequest recovery_request;
      proxy_proto::RecoveryReply recovery_reply;

      int chosen_cluster_id = get_cluster_id_by_group_id(t_stripe, recovery_group_ids[0]);
      std::string chosen_proxy = m_cluster_table[chosen_cluster_id].proxy_ip + ":" + std::to_string(m_cluster_table[chosen_cluster_id].proxy_port);
      recovery_request.set_failed_block_id(failed_block_id);
      recovery_request.set_failed_block_key(t_stripe.blocks[failed_block_id]->block_key);
      // Write recovery result back to the original failed block node.
      int t_node_id = t_stripe.blocks[failed_block_id]->map2node;
      recovery_request.set_replaced_node_ip(m_node_table[t_node_id].node_ip);
      recovery_request.set_replaced_node_port(m_node_table[t_node_id].node_port);
      recovery_request.set_cross_rack_num(0);
      std::vector<int> blockids = t_stripe.group_to_blocks[recovery_group_ids[0]];
      for (int i = 0; i < int(blockids.size()); i++)
      {
        if (blockids[i] == failed_block_id)
          continue;

        Block *t_block = t_stripe.blocks[blockids[i]];
        recovery_request.add_datanodeip(m_node_table[t_block->map2node].node_ip);
        recovery_request.add_datanodeport(m_node_table[t_block->map2node].node_port);
        recovery_request.add_blockkeys(t_block->block_key);
        recovery_request.add_blockids(t_block->block_id);
      }
      std::chrono::high_resolution_clock::time_point start = std::chrono::high_resolution_clock::now();
      grpc_notify_time.push_back(std::chrono::duration_cast<std::chrono::duration<double>>(start.time_since_epoch()).count());
      status = m_proxy_ptrs[chosen_proxy]->recoveryBreakdown(&recovery_context, recovery_request, &recovery_reply);
      if (status.ok())
      {
        disk_io_start_time.push_back(recovery_reply.disk_io_start_time());
        disk_io_end_time.push_back(recovery_reply.disk_io_end_time());
        decode_start_time.push_back(recovery_reply.decode_start_time());
        decode_end_time.push_back(recovery_reply.decode_end_time());
        network_start_time.push_back(recovery_reply.network_start_time());
        network_end_time.push_back(recovery_reply.network_end_time());
        cross_rack_network_time = recovery_reply.cross_rack_time();
        cross_rack_xor_time = recovery_reply.cross_rack_xor_time();
        data_node_grpc_notify_time.push_back(recovery_reply.data_node_grpc_notify_time());
        data_node_grpc_start_time.push_back(recovery_reply.data_node_grpc_start_time());
        dest_data_node_network_time = recovery_reply.dest_data_node_network_time();
        dest_data_node_disk_io_time = recovery_reply.dest_data_node_disk_io_time();
        grpc_start_time.push_back(recovery_reply.grpc_start_time());
        std::cout << "[Coordinator] recovery of " << stripe_id << "_" << failed_block_id << " success!" << std::endl;
        return true;
      }
      else
      {
        std::cout << "[Coordinator] recovery of " << stripe_id << "_" << failed_block_id << " failed!" << std::endl;
        return false;
      }
    }
    else
    {
      int dest_group_id = t_stripe.blocks[failed_block_id]->map2group;
      int dest_cluster_id = get_cluster_id_by_group_id(t_stripe, dest_group_id);
      std::string dest_proxy_ip = m_cluster_table[dest_cluster_id].proxy_ip;
      int dest_proxy_port = m_cluster_table[dest_cluster_id].proxy_port;
      std::vector<int> chosen_cluster_ids;
      for(int i = 0; i < recovery_group_ids.size(); i++){
        chosen_cluster_ids.push_back(get_cluster_id_by_group_id(t_stripe, recovery_group_ids[i]));
      }
      std::vector<std::string> chosen_proxies;
      for(int i = 0; i < chosen_cluster_ids.size(); i++){
        chosen_proxies.push_back(m_cluster_table[chosen_cluster_ids[i]].proxy_ip + ":" + std::to_string(m_cluster_table[chosen_cluster_ids[i]].proxy_port));
      }
      std::vector<std::thread> threads;
      for(int i = 0; i < recovery_group_ids.size(); i++){
        if(recovery_group_ids[i] == dest_group_id){
          continue;
        }
        threads.push_back(std::thread([&t_stripe, &chosen_proxies, &recovery_group_ids, i, failed_block_id, dest_proxy_ip, dest_proxy_port, this,
          &disk_io_start_time, &disk_io_end_time, &decode_start_time, &decode_end_time, &network_start_time, &network_end_time, 
          &grpc_notify_time, &grpc_start_time, &data_node_grpc_notify_time, &data_node_grpc_start_time
        ](){
          grpc::ClientContext degraded_read_context;
          proxy_proto::DegradedReadRequest degraded_read_request;
          proxy_proto::DegradedReadReply degraded_read_reply;
          degraded_read_request.set_clientip(dest_proxy_ip);
          degraded_read_request.set_clientport(dest_proxy_port + ECProject::PROXY_PORT_SHIFT);
          degraded_read_request.set_failed_block_id(failed_block_id);
          degraded_read_request.set_failed_block_key(t_stripe.blocks[failed_block_id]->block_key);
          std::vector<int> blockids = t_stripe.group_to_blocks[recovery_group_ids[i]];
          for (int j = 0; j < int(blockids.size()); j++)
          {
            if(m_sys_config->CodeType == "AzureLRC" && degraded_read_request.blockids_size() == (m_sys_config->k / m_sys_config->z))
              break;

            if ((m_sys_config->CodeType == "AzureLRC" && blockids[j] >= m_sys_config->k + m_sys_config->r) || blockids[j] == failed_block_id)
              continue;

            Block *t_block = t_stripe.blocks[blockids[j]];
            degraded_read_request.add_datanodeip(m_node_table[t_block->map2node].node_ip);
            degraded_read_request.add_datanodeport(m_node_table[t_block->map2node].node_port);
            degraded_read_request.add_blockkeys(t_block->block_key);
            degraded_read_request.add_blockids(t_block->block_id);
          }
          std::chrono::high_resolution_clock::time_point start = std::chrono::high_resolution_clock::now();

          grpc::Status status = m_proxy_ptrs[chosen_proxies[i]]->degradedReadBreakdown(&degraded_read_context, degraded_read_request, &degraded_read_reply);
          if (status.ok())
          {
            std::lock_guard<std::mutex> lock(m_mutex);
            disk_io_start_time.push_back(degraded_read_reply.disk_io_start_time());
            disk_io_end_time.push_back(degraded_read_reply.disk_io_end_time());
            decode_start_time.push_back(degraded_read_reply.decode_start_time());
            decode_end_time.push_back(degraded_read_reply.decode_end_time());
            network_start_time.push_back(degraded_read_reply.network_start_time());
            network_end_time.push_back(degraded_read_reply.network_end_time());
            grpc_start_time.push_back(degraded_read_reply.grpc_start_time());
            data_node_grpc_notify_time.push_back(degraded_read_reply.data_node_grpc_notify_time());
            data_node_grpc_start_time.push_back(degraded_read_reply.data_node_grpc_start_time());
            grpc_notify_time.push_back(std::chrono::duration_cast<std::chrono::duration<double>>(start.time_since_epoch()).count());
    
            std::cout << "[Coordinator] partial degraded read of " << failed_block_id << " success!" << std::endl;
          }
          else
          {
            std::cout << "[Coordinator] partial degraded read of " << failed_block_id << " failed!" << std::endl;
          }
        }));

      }
      int cross_rack_num = recovery_group_ids.size() - 1;
      threads.push_back(std::thread([this, &t_stripe, cross_rack_num, dest_group_id, dest_cluster_id, dest_proxy_ip, dest_proxy_port, stripe_id, failed_block_id,
        &disk_io_start_time, &disk_io_end_time, &decode_start_time, &decode_end_time, &network_start_time, &network_end_time, 
        &grpc_notify_time, &grpc_start_time, &data_node_grpc_notify_time, &data_node_grpc_start_time, &cross_rack_network_time, &cross_rack_xor_time,
        &dest_data_node_network_time, &dest_data_node_disk_io_time
        ](){
        grpc::ClientContext recovery_context;
        proxy_proto::RecoveryRequest recovery_request;
        proxy_proto::RecoveryReply recovery_reply;
        recovery_request.set_failed_block_id(failed_block_id);
        recovery_request.set_failed_block_key(t_stripe.blocks[failed_block_id]->block_key);
        // Write recovery result back to the original failed block node.
        int t_node_id = t_stripe.blocks[failed_block_id]->map2node;
        recovery_request.set_replaced_node_ip(m_node_table[t_node_id].node_ip);
        recovery_request.set_replaced_node_port(m_node_table[t_node_id].node_port);
        recovery_request.set_cross_rack_num(cross_rack_num);
        std::vector<int> blockids = t_stripe.group_to_blocks[dest_group_id];
        for (int i = 0; i < int(blockids.size()); i++)
        {
          if(m_sys_config->CodeType == "AzureLRC" && recovery_request.blockids_size() == (m_sys_config->k / m_sys_config->z))
            break;

          if (blockids[i] == failed_block_id)
            continue;

          Block *t_block = t_stripe.blocks[blockids[i]];
          recovery_request.add_datanodeip(m_node_table[t_block->map2node].node_ip);
          recovery_request.add_datanodeport(m_node_table[t_block->map2node].node_port);
          recovery_request.add_blockkeys(t_block->block_key);
          recovery_request.add_blockids(t_block->block_id);
        }
        //std::cout << "[Coordinator] start recovery of " << stripe_id << "_" << failed_block_id << std::endl;
        std::chrono::high_resolution_clock::time_point start = std::chrono::high_resolution_clock::now();

        grpc::Status status = m_proxy_ptrs[dest_proxy_ip + ":" + std::to_string(dest_proxy_port)]->recoveryBreakdown(&recovery_context, recovery_request, &recovery_reply);
        if (status.ok())
        {
          std::lock_guard<std::mutex> lock(m_mutex);
          grpc_notify_time.push_back(std::chrono::duration_cast<std::chrono::duration<double>>(start.time_since_epoch()).count());
          disk_io_start_time.push_back(recovery_reply.disk_io_start_time());
          disk_io_end_time.push_back(recovery_reply.disk_io_end_time());
          decode_start_time.push_back(recovery_reply.decode_start_time());
          decode_end_time.push_back(recovery_reply.decode_end_time());
          network_start_time.push_back(recovery_reply.network_start_time());
          network_end_time.push_back(recovery_reply.network_end_time());
          grpc_start_time.push_back(recovery_reply.grpc_start_time());
          cross_rack_network_time = recovery_reply.cross_rack_time();
          cross_rack_xor_time = recovery_reply.cross_rack_xor_time();
          data_node_grpc_notify_time.push_back(recovery_reply.data_node_grpc_notify_time());
          data_node_grpc_start_time.push_back(recovery_reply.data_node_grpc_start_time());
          dest_data_node_network_time = recovery_reply.dest_data_node_network_time();
          dest_data_node_disk_io_time = recovery_reply.dest_data_node_disk_io_time();
          std::cout << "[Coordinator] recovery of " << stripe_id << "_" << failed_block_id << " success!" << std::endl;
        }
        else
        {
          std::cout << "[Coordinator] recovery of " << stripe_id << "_" << failed_block_id << " failed!" << std::endl;
        }
      }
      ));
      for(int i = 0; i < threads.size(); i++){
        threads[i].join();
      }
    }
    return true;
  }

  grpc::Status CoordinatorImpl::decodeTest(
    grpc::ServerContext *context,
    const coordinator_proto::KeyAndClientIP *keyClient,
    coordinator_proto::DegradedReadReply *degradedReadReply)
  {
    std::string code_type = m_sys_config->CodeType;
    int k = m_sys_config->k;
    int r = m_sys_config->r;
    int z = m_sys_config->z;
    int block_size = m_sys_config->BlockSize;
    int stripe_id = std::stoi(keyClient->key().substr(0, keyClient->key().find('_')));
    int failed_block_id = std::stoi(keyClient->key().substr(keyClient->key().find('_') + 1));

    std::vector<int> recovery_block_ids;
    auto plan = ECProject::get_recovery_group_and_block_ids(code_type, k, r, z, failed_block_id);
    if (plan.empty())
    {
      std::cout << "[Coordinator] decodeTest: get_recovery_group_and_block_ids returned empty" << std::endl;
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "get_recovery_group_and_block_ids returned empty");
    }
    for (const auto &p : plan)
      for (int bid : p.second)
        recovery_block_ids.push_back(bid);
    int block_num = (int)recovery_block_ids.size();
    unsigned char *recovery_data = static_cast<unsigned char*>(std::aligned_alloc(32, m_sys_config->BlockSize * block_num));
    std::vector<unsigned char *> recovery_data_ptrs;
    for(int i = 0; i < block_num; i++){
      recovery_data_ptrs.push_back(recovery_data + i * block_size);
    }
    
    unsigned char *res = static_cast<unsigned char*>(std::aligned_alloc(32, m_sys_config->BlockSize));
    std::chrono::high_resolution_clock::time_point start = std::chrono::high_resolution_clock::now();
    if(code_type == "AzureLRC"){
      decode_azure_lrc(k, r, z, block_num, &recovery_block_ids, recovery_data_ptrs.data(), res, block_size, failed_block_id);
    }
    else if(code_type == "UniLRC"){
      decode_unilrc(k, r, z, block_num, &recovery_block_ids, recovery_data_ptrs.data(), res, block_size);
    }
    else if(code_type == "OptimalLRC"){
      decode_optimal_lrc(k, r, z, block_num, &recovery_block_ids, recovery_data_ptrs.data(), res, block_size, failed_block_id);
    }
    else if(code_type == "UniformLRC"){
      decode_uniform_lrc(k, r, z, block_num, &recovery_block_ids, recovery_data_ptrs.data(), res, block_size, failed_block_id);
    }
    else{
      std::cout << "[Coordinator] decodeTest: unknown code type!" << std::endl;
      return grpc::Status(grpc::INVALID_ARGUMENT, "unknown code type");
    }
    std::chrono::high_resolution_clock::time_point end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = std::chrono::duration_cast<std::chrono::duration<double>>(end - start);
    std::cout << "[Coordinator] decodeTest took " << duration.count() << " seconds" << std::endl;
    degradedReadReply->set_decode_time(duration.count());
    delete[] res;
    delete[] recovery_data;

    return grpc::Status::OK;
  } 

  bool CoordinatorImpl::recovery_one_block(int stripe_id, int failed_block_id)
  {
    auto plan = ECProject::get_recovery_group_and_block_ids(
        m_sys_config->CodeType, m_sys_config->k, m_sys_config->r, m_sys_config->z, failed_block_id);
    return recovery_one_block_with_plan(stripe_id, failed_block_id, plan);
  }

  bool CoordinatorImpl::recovery_one_block_with_plan(int stripe_id, int failed_block_id,
      const std::vector<std::pair<int, std::vector<int>>> &plan)
  {
    Stripe &t_stripe = m_stripe_table[stripe_id];
    grpc::Status status;

    if (!plan.empty())
    {
      if (plan.size() == 1)
      {
        int group_id = plan[0].first;
        const std::vector<int> &block_ids = plan[0].second;
        int chosen_cluster_id = get_cluster_id_by_group_id(t_stripe, group_id);
        std::string chosen_proxy = m_cluster_table[chosen_cluster_id].proxy_ip + ":" + std::to_string(m_cluster_table[chosen_cluster_id].proxy_port);
        grpc::ClientContext recovery_context;
        proxy_proto::RecoveryRequest recovery_request;
        proxy_proto::RecoveryReply recovery_reply;
        recovery_request.set_failed_block_id(failed_block_id);
        recovery_request.set_failed_block_key(t_stripe.blocks[failed_block_id]->block_key);
        // Write recovery result back to the original failed block node.
        int t_node_id = t_stripe.blocks[failed_block_id]->map2node;
        recovery_request.set_replaced_node_ip(m_node_table[t_node_id].node_ip);
        recovery_request.set_replaced_node_port(m_node_table[t_node_id].node_port);
        recovery_request.set_cross_rack_num(0);
        add_block_list_to_recovery_request(t_stripe, block_ids, &recovery_request);
        status = m_proxy_ptrs[chosen_proxy]->recovery(&recovery_context, recovery_request, &recovery_reply);
        if (status.ok())
        {
          std::cout << "[Coordinator] recovery of " << stripe_id << "_" << failed_block_id << " success!" << std::endl;
          return true;
        }
        std::cout << "[Coordinator] recovery of " << stripe_id << "_" << failed_block_id << " failed!" << std::endl;
        return false;
      }
      int dest_group_id = t_stripe.blocks[failed_block_id]->map2group;
      int dest_cluster_id = get_cluster_id_by_group_id(t_stripe, dest_group_id);
      std::string dest_proxy_ip = m_cluster_table[dest_cluster_id].proxy_ip;
      int dest_proxy_port = m_cluster_table[dest_cluster_id].proxy_port;
      std::vector<std::string> chosen_proxies;
      for (size_t i = 0; i < plan.size(); i++)
        chosen_proxies.push_back(m_cluster_table[get_cluster_id_by_group_id(t_stripe, plan[i].first)].proxy_ip + ":" + std::to_string(m_cluster_table[get_cluster_id_by_group_id(t_stripe, plan[i].first)].proxy_port));
      std::vector<std::thread> threads;
      int cross_rack_num = 0;
      for (size_t i = 0; i < plan.size(); i++)
      {
        if (plan[i].first == dest_group_id)
          continue;
        std::vector<int> block_ids = plan[i].second;
        std::string proxy_key = chosen_proxies[i];
        threads.push_back(std::thread([this, &t_stripe, proxy_key, block_ids, failed_block_id, dest_proxy_ip, dest_proxy_port]() {
          grpc::ClientContext degraded_read_context;
          proxy_proto::DegradedReadRequest degraded_read_request;
          proxy_proto::DegradedReadReply degraded_read_reply;
          degraded_read_request.set_clientip(dest_proxy_ip);
          degraded_read_request.set_clientport(dest_proxy_port + ECProject::PROXY_PORT_SHIFT);
          degraded_read_request.set_failed_block_id(failed_block_id);
          degraded_read_request.set_failed_block_key(t_stripe.blocks[failed_block_id]->block_key);
          add_block_list_to_degraded_read_request(t_stripe, block_ids, &degraded_read_request);
          grpc::Status st = m_proxy_ptrs[proxy_key]->degradedRead(&degraded_read_context, degraded_read_request, &degraded_read_reply);
          if (st.ok())
            std::cout << "[Coordinator] partial degraded read of " << failed_block_id << " success!" << std::endl;
          else
            std::cout << "[Coordinator] partial degraded read of " << failed_block_id << " failed!" << std::endl;
        }));
        cross_rack_num++;
      }
      std::vector<int> dest_block_ids;
      for (size_t i = 0; i < plan.size(); i++)
        if (plan[i].first == dest_group_id)
        {
          dest_block_ids = plan[i].second;
          break;
        }
      bool dest_success = false;
      std::mutex dest_mutex;
      threads.push_back(std::thread([this, &t_stripe, dest_proxy_ip, dest_proxy_port, dest_cluster_id, stripe_id, failed_block_id, cross_rack_num, dest_group_id, dest_block_ids, &plan, &dest_success, &dest_mutex]() {
        grpc::ClientContext recovery_context;
        proxy_proto::RecoveryRequest recovery_request;
        proxy_proto::RecoveryReply recovery_reply;
        recovery_request.set_failed_block_id(failed_block_id);
        recovery_request.set_failed_block_key(t_stripe.blocks[failed_block_id]->block_key);
        // Write recovery result back to the original failed block node.
        int t_node_id = t_stripe.blocks[failed_block_id]->map2node;
        recovery_request.set_replaced_node_ip(m_node_table[t_node_id].node_ip);
        recovery_request.set_replaced_node_port(m_node_table[t_node_id].node_port);
        recovery_request.set_cross_rack_num(cross_rack_num);
        for (size_t i = 0; i < plan.size(); i++)
          if (plan[i].first != dest_group_id)
          {
            int cid = get_cluster_id_by_group_id(t_stripe, plan[i].first);
            recovery_request.add_proxyip(m_cluster_table[cid].proxy_ip);
            recovery_request.add_proxyport(m_cluster_table[cid].proxy_port);
          }
        add_block_list_to_recovery_request(t_stripe, dest_block_ids, &recovery_request);
        grpc::Status st = m_proxy_ptrs[dest_proxy_ip + ":" + std::to_string(dest_proxy_port)]->recovery(&recovery_context, recovery_request, &recovery_reply);
        {
          std::lock_guard<std::mutex> lock(dest_mutex);
          dest_success = st.ok();
        }
        if (st.ok())
          std::cout << "[Coordinator] recovery of " << stripe_id << "_" << failed_block_id << " success!" << std::endl;
        else
          std::cout << "[Coordinator] recovery of " << stripe_id << "_" << failed_block_id << " failed!" << std::endl;
      }));
      for (size_t i = 0; i + 1 < threads.size(); i++)
        threads[i].detach();
      threads.back().join();
      {
        std::lock_guard<std::mutex> lock(dest_mutex);
        return dest_success;
      }
    }
    std::cout << "[Coordinator] recovery_one_block_with_plan: empty plan for block " << failed_block_id << std::endl;
    return false;
  }

  bool CoordinatorImpl::recovery_one_block_with_plan_breakdown(
      int stripe_id, int failed_block_id,
      const std::vector<std::pair<int, std::vector<int>>> &plan,
      coordinator_proto::RecoveryReply *breakdown_reply)
  {
    Stripe &t_stripe = m_stripe_table[stripe_id];
    RecoveryBreakdownSamples samples;
    if (plan.empty())
    {
      std::cout << "[Coordinator] recovery_one_block_with_plan_breakdown: empty plan for block "
                << failed_block_id << std::endl;
      return false;
    }

    if (plan.size() == 1)
    {
      int group_id = plan[0].first;
      const std::vector<int> &block_ids = plan[0].second;
      int chosen_cluster_id = get_cluster_id_by_group_id(t_stripe, group_id);
      std::string chosen_proxy = m_cluster_table[chosen_cluster_id].proxy_ip + ":" +
                                 std::to_string(m_cluster_table[chosen_cluster_id].proxy_port);
      grpc::ClientContext recovery_context;
      proxy_proto::RecoveryRequest recovery_request;
      proxy_proto::RecoveryReply recovery_reply;
      recovery_request.set_failed_block_id(failed_block_id);
      recovery_request.set_failed_block_key(t_stripe.blocks[failed_block_id]->block_key);
      int t_node_id = t_stripe.blocks[failed_block_id]->map2node;
      recovery_request.set_replaced_node_ip(m_node_table[t_node_id].node_ip);
      recovery_request.set_replaced_node_port(m_node_table[t_node_id].node_port);
      recovery_request.set_cross_rack_num(0);
      add_block_list_to_recovery_request(t_stripe, block_ids, &recovery_request);
      std::chrono::high_resolution_clock::time_point grpc_notify = std::chrono::high_resolution_clock::now();
      double grpc_notify_time =
          std::chrono::duration_cast<std::chrono::duration<double>>(grpc_notify.time_since_epoch()).count();
      grpc::Status status = m_proxy_ptrs[chosen_proxy]->recoveryBreakdown(&recovery_context, recovery_request, &recovery_reply);
      if (!status.ok())
      {
        std::cout << "[Coordinator] recovery breakdown of " << stripe_id << "_" << failed_block_id
                  << " failed!" << std::endl;
        return false;
      }
      samples.append_recovery_reply(recovery_reply, grpc_notify_time);
      samples.fill_recovery_reply(breakdown_reply);
      std::cout << "[Coordinator] recovery breakdown of " << stripe_id << "_" << failed_block_id
                << " success!" << std::endl;
      return true;
    }

    int dest_group_id = t_stripe.blocks[failed_block_id]->map2group;
    int dest_cluster_id = get_cluster_id_by_group_id(t_stripe, dest_group_id);
    std::string dest_proxy_ip = m_cluster_table[dest_cluster_id].proxy_ip;
    int dest_proxy_port = m_cluster_table[dest_cluster_id].proxy_port;
    std::vector<std::string> chosen_proxies;
    for (size_t i = 0; i < plan.size(); i++)
      chosen_proxies.push_back(m_cluster_table[get_cluster_id_by_group_id(t_stripe, plan[i].first)].proxy_ip +
                               ":" + std::to_string(m_cluster_table[get_cluster_id_by_group_id(t_stripe, plan[i].first)].proxy_port));

    std::vector<std::thread> threads;
    int cross_rack_num = 0;
    bool dest_success = false;
    std::mutex samples_mutex;

    for (size_t i = 0; i < plan.size(); i++)
    {
      if (plan[i].first == dest_group_id)
        continue;
      std::vector<int> block_ids = plan[i].second;
      std::string proxy_key = chosen_proxies[i];
      threads.push_back(std::thread([this, &t_stripe, proxy_key, block_ids, failed_block_id, dest_proxy_ip,
                                     dest_proxy_port, &samples, &samples_mutex]() {
        grpc::ClientContext degraded_read_context;
        proxy_proto::DegradedReadRequest degraded_read_request;
        proxy_proto::DegradedReadReply degraded_read_reply;
        degraded_read_request.set_clientip(dest_proxy_ip);
        degraded_read_request.set_clientport(dest_proxy_port + ECProject::PROXY_PORT_SHIFT);
        degraded_read_request.set_failed_block_id(failed_block_id);
        degraded_read_request.set_failed_block_key(t_stripe.blocks[failed_block_id]->block_key);
        add_block_list_to_degraded_read_request(t_stripe, block_ids, &degraded_read_request);
        std::chrono::high_resolution_clock::time_point grpc_notify = std::chrono::high_resolution_clock::now();
        double grpc_notify_time =
            std::chrono::duration_cast<std::chrono::duration<double>>(grpc_notify.time_since_epoch()).count();
        grpc::Status st = m_proxy_ptrs[proxy_key]->degradedReadBreakdown(&degraded_read_context, degraded_read_request,
                                                                         &degraded_read_reply);
        if (st.ok())
        {
          std::lock_guard<std::mutex> lock(samples_mutex);
          samples.append_degraded_read_reply(degraded_read_reply, grpc_notify_time);
          std::cout << "[Coordinator] partial degraded read breakdown of " << failed_block_id << " success!"
                    << std::endl;
        }
        else
        {
          std::cout << "[Coordinator] partial degraded read breakdown of " << failed_block_id << " failed!"
                    << std::endl;
        }
      }));
      cross_rack_num++;
    }

    std::vector<int> dest_block_ids;
    for (size_t i = 0; i < plan.size(); i++)
      if (plan[i].first == dest_group_id)
      {
        dest_block_ids = plan[i].second;
        break;
      }

    threads.push_back(std::thread([this, &t_stripe, dest_proxy_ip, dest_proxy_port, stripe_id, failed_block_id,
                                   cross_rack_num, dest_group_id, dest_block_ids, &plan, &samples, &samples_mutex,
                                   &dest_success]() {
      grpc::ClientContext recovery_context;
      proxy_proto::RecoveryRequest recovery_request;
      proxy_proto::RecoveryReply recovery_reply;
      recovery_request.set_failed_block_id(failed_block_id);
      recovery_request.set_failed_block_key(t_stripe.blocks[failed_block_id]->block_key);
      int t_node_id = t_stripe.blocks[failed_block_id]->map2node;
      recovery_request.set_replaced_node_ip(m_node_table[t_node_id].node_ip);
      recovery_request.set_replaced_node_port(m_node_table[t_node_id].node_port);
      recovery_request.set_cross_rack_num(cross_rack_num);
      for (size_t i = 0; i < plan.size(); i++)
        if (plan[i].first != dest_group_id)
        {
          int cid = get_cluster_id_by_group_id(t_stripe, plan[i].first);
          recovery_request.add_proxyip(m_cluster_table[cid].proxy_ip);
          recovery_request.add_proxyport(m_cluster_table[cid].proxy_port);
        }
      add_block_list_to_recovery_request(t_stripe, dest_block_ids, &recovery_request);
      std::chrono::high_resolution_clock::time_point grpc_notify = std::chrono::high_resolution_clock::now();
      double grpc_notify_time =
          std::chrono::duration_cast<std::chrono::duration<double>>(grpc_notify.time_since_epoch()).count();
      grpc::Status st = m_proxy_ptrs[dest_proxy_ip + ":" + std::to_string(dest_proxy_port)]->recoveryBreakdown(
          &recovery_context, recovery_request, &recovery_reply);
      std::lock_guard<std::mutex> lock(samples_mutex);
      dest_success = st.ok();
      if (st.ok())
      {
        samples.append_recovery_reply(recovery_reply, grpc_notify_time);
        std::cout << "[Coordinator] recovery breakdown of " << stripe_id << "_" << failed_block_id << " success!"
                  << std::endl;
      }
      else
      {
        std::cout << "[Coordinator] recovery breakdown of " << stripe_id << "_" << failed_block_id << " failed!"
                  << std::endl;
      }
    }));

    for (auto &th : threads)
      th.join();

    if (!dest_success)
      return false;
    samples.fill_recovery_reply(breakdown_reply);
    return true;
  }


  grpc::Status CoordinatorImpl::getRecoveryBreakdown(
      grpc::ServerContext *context,
      const coordinator_proto::KeyAndClientIP *keyClient,
      coordinator_proto::RecoveryReply *recoveryReply)
  {
    std::chrono::time_point<std::chrono::high_resolution_clock> START = std::chrono::high_resolution_clock::now();
    recoveryReply->set_grpc_start_time(std::chrono::duration_cast<std::chrono::duration<double>>(START.time_since_epoch()).count());
    int stripe_id = std::stoi(keyClient->key().substr(0, keyClient->key().find('_')));
    int failed_block_id = std::stoi(keyClient->key().substr(keyClient->key().find('_') + 1));
    auto plan = ECProject::get_recovery_group_and_block_ids(
        m_sys_config->CodeType, m_sys_config->k, m_sys_config->r, m_sys_config->z, failed_block_id);
    if (recovery_one_block_with_plan_breakdown(stripe_id, failed_block_id, plan, recoveryReply))
      return grpc::Status::OK;
    return grpc::Status(grpc::StatusCode::INTERNAL, "Recovery failed!");
  }

  grpc::Status CoordinatorImpl::getRecovery(
      grpc::ServerContext *context,
      const coordinator_proto::KeyAndClientIP *keyClient,
      coordinator_proto::RecoveryReply *recoveryReply)
  {
    int stripe_id = std::stoi(keyClient->key().substr(0, keyClient->key().find('_')));
    int failed_block_id = std::stoi(keyClient->key().substr(keyClient->key().find('_') + 1));
    bool if_success = recovery_one_block(stripe_id, failed_block_id);

    if (if_success)
    {
      return grpc::Status::OK;
    }
    else
    {
      return grpc::Status(grpc::StatusCode::INTERNAL, "Recovery failed!");
    }
  }

  bool CoordinatorImpl::degraded_read_one_block_breakdown(int stripe_id, int failed_block_id, std::string client_ip, int client_port, 
    std::vector<double> &disk_io_start_time, std::vector<double> &disk_io_end_time, std::vector<double> &decode_start_time, std::vector<double> &decode_end_time,
    std::vector<double> &network_start_time, std::vector<double> &network_end_time, double &cross_rack_network_time, double &cross_rack_xor_time,
    std::vector<double> &grpc_notify_time, std::vector<double> &grpc_start_time, std::vector<double> &data_node_grpc_notify_time, std::vector<double> &data_node_grpc_start_time)
  {
    std::string code_type = m_sys_config->CodeType;
    Stripe &t_stripe = m_stripe_table[stripe_id];
    std::vector<int> recovery_group_ids = get_recovery_group_ids(m_sys_config->CodeType, m_sys_config->k, m_sys_config->r, m_sys_config->z, failed_block_id);
    grpc::Status status;

    if (recovery_group_ids.size() == 1)
    {
      //assert((code_type == "UniLRC") || (code_type == "AzureLRC" && (failed_block_id < m_sys_config->k || failed_block_id >= m_sys_config->k + m_sys_config->r)));

      grpc::ClientContext recovery_context;
      proxy_proto::RecoveryRequest recovery_request;
      proxy_proto::DegradedReadReply degraded_read_reply;

      int chosen_cluster_id = get_cluster_id_by_group_id(t_stripe, recovery_group_ids[0]);
      std::string chosen_proxy = m_cluster_table[chosen_cluster_id].proxy_ip + ":" + std::to_string(m_cluster_table[chosen_cluster_id].proxy_port);
      recovery_request.set_failed_block_id(failed_block_id);
      recovery_request.set_failed_block_key(t_stripe.blocks[failed_block_id]->block_key);
      recovery_request.set_replaced_node_ip(client_ip);
      recovery_request.set_replaced_node_port(client_port);
      recovery_request.set_cross_rack_num(0);
      std::vector<int> blockids = t_stripe.group_to_blocks[recovery_group_ids[0]];
      for (int i = 0; i < int(blockids.size()); i++)
      {
        if (blockids[i] == failed_block_id)
          continue;

        Block *t_block = t_stripe.blocks[blockids[i]];
        recovery_request.add_datanodeip(m_node_table[t_block->map2node].node_ip);
        recovery_request.add_datanodeport(m_node_table[t_block->map2node].node_port);
        recovery_request.add_blockkeys(t_block->block_key);
        recovery_request.add_blockids(t_block->block_id);
      }

      std::chrono::high_resolution_clock::time_point grpc_notify = std::chrono::high_resolution_clock::now();
      grpc_notify_time.push_back(std::chrono::duration_cast<std::chrono::duration<double>>(grpc_notify.time_since_epoch()).count());
      status = m_proxy_ptrs[chosen_proxy]->degradedRead2ClientBreakdown(&recovery_context, recovery_request, &degraded_read_reply);
      if (status.ok())
      {
        disk_io_start_time.push_back(degraded_read_reply.disk_io_start_time());
        disk_io_end_time.push_back(degraded_read_reply.disk_io_end_time());
        decode_start_time.push_back(degraded_read_reply.decode_start_time());
        decode_end_time.push_back(degraded_read_reply.decode_end_time());
        network_start_time.push_back(degraded_read_reply.network_start_time());
        network_end_time.push_back(degraded_read_reply.network_end_time());
        cross_rack_network_time = degraded_read_reply.cross_rack_time();
        cross_rack_xor_time = degraded_read_reply.cross_rack_xor_time();
        grpc_start_time.push_back(degraded_read_reply.grpc_start_time());
        data_node_grpc_notify_time.push_back(degraded_read_reply.data_node_grpc_notify_time());
        data_node_grpc_start_time.push_back(degraded_read_reply.data_node_grpc_start_time());
        std::cout << "[Coordinator] degraded read of " << stripe_id << "_" << failed_block_id << " success!" << std::endl;
        return true;
      }
      else
      {
        std::cout << "[Coordinator] degraded read of " << stripe_id << "_" << failed_block_id << " failed!" << std::endl;
        return false;
      }
    }
    else
    {
      int dest_group_id = t_stripe.blocks[failed_block_id]->map2group;
      int dest_cluster_id = get_cluster_id_by_group_id(t_stripe, dest_group_id);
      std::string dest_proxy_ip = m_cluster_table[dest_cluster_id].proxy_ip;
      int dest_proxy_port = m_cluster_table[dest_cluster_id].proxy_port;
      std::vector<int> chosen_cluster_ids;
      for(int i = 0; i < recovery_group_ids.size(); i++){
        chosen_cluster_ids.push_back(get_cluster_id_by_group_id(t_stripe, recovery_group_ids[i]));
      }
      std::vector<std::string> chosen_proxies;
      for(int i = 0; i < chosen_cluster_ids.size(); i++){
        chosen_proxies.push_back(m_cluster_table[chosen_cluster_ids[i]].proxy_ip + ":" + std::to_string(m_cluster_table[chosen_cluster_ids[i]].proxy_port));
      }
      std::vector<std::thread> threads;
      for(int i = 0; i < recovery_group_ids.size(); i++){
        if(recovery_group_ids[i] == dest_group_id){
          continue;
        }
        threads.push_back(std::thread([&t_stripe, &chosen_proxies, &recovery_group_ids, i, failed_block_id, dest_proxy_ip, dest_proxy_port, this, 
          &disk_io_start_time, &disk_io_end_time, &decode_start_time, &decode_end_time, &network_start_time, &network_end_time, 
          &grpc_notify_time, &grpc_start_time, &data_node_grpc_notify_time, &data_node_grpc_start_time](){
          grpc::ClientContext degraded_read_context;
          proxy_proto::DegradedReadRequest degraded_read_request;
          proxy_proto::DegradedReadReply degraded_read_reply;
          degraded_read_request.set_clientip(dest_proxy_ip);
          degraded_read_request.set_clientport(dest_proxy_port + ECProject::PROXY_PORT_SHIFT);
          degraded_read_request.set_failed_block_id(failed_block_id);
          degraded_read_request.set_failed_block_key(t_stripe.blocks[failed_block_id]->block_key);
          std::vector<int> blockids = t_stripe.group_to_blocks[recovery_group_ids[i]];
          for (int j = 0; j < int(blockids.size()); j++)
          {
            if(m_sys_config->CodeType == "AzureLRC" && degraded_read_request.blockids_size() == (m_sys_config->k / m_sys_config->z))
              break;

            if ((m_sys_config->CodeType == "AzureLRC" && blockids[j] >= m_sys_config->k + m_sys_config->r) || blockids[j] == failed_block_id)
              continue;

            Block *t_block = t_stripe.blocks[blockids[j]];
            degraded_read_request.add_datanodeip(this->m_node_table[t_block->map2node].node_ip);
            degraded_read_request.add_datanodeport(this->m_node_table[t_block->map2node].node_port);
            degraded_read_request.add_blockkeys(t_block->block_key);
            degraded_read_request.add_blockids(t_block->block_id);
          }
          std::cout << "[Coordinator] start partial degraded read of " << failed_block_id << std::endl;
          std::chrono::high_resolution_clock::time_point grpc_notify = std::chrono::high_resolution_clock::now();
          grpc_notify_time.push_back(std::chrono::duration_cast<std::chrono::duration<double>>(grpc_notify.time_since_epoch()).count());
          grpc::Status status = this->m_proxy_ptrs[chosen_proxies[i]]->degradedReadBreakdown(&degraded_read_context, degraded_read_request, &degraded_read_reply);
          if (status.ok())
          {
            disk_io_start_time.push_back(degraded_read_reply.disk_io_start_time());
            disk_io_end_time.push_back(degraded_read_reply.disk_io_end_time());
            decode_start_time.push_back(degraded_read_reply.decode_start_time());
            decode_end_time.push_back(degraded_read_reply.decode_end_time());
            network_start_time.push_back(degraded_read_reply.network_start_time());
            network_end_time.push_back(degraded_read_reply.network_end_time());
            grpc_start_time.push_back(degraded_read_reply.grpc_start_time());
            data_node_grpc_notify_time.push_back(degraded_read_reply.data_node_grpc_notify_time());
            data_node_grpc_start_time.push_back(degraded_read_reply.data_node_grpc_start_time());
            std::cout << "[Coordinator] partial degraded read of " << failed_block_id << " success!" << std::endl;
          }
          else
          {
            std::cout << "[Coordinator] partial degraded read of " << failed_block_id << " failed!" << std::endl;
          }
        }));

      }
      int cross_rack_num = recovery_group_ids.size() - 1;
      threads.push_back(std::thread([this, &t_stripe, cross_rack_num, dest_group_id, dest_cluster_id, dest_proxy_ip, dest_proxy_port, stripe_id, failed_block_id, client_ip, client_port, 
        &disk_io_start_time, &disk_io_end_time, &decode_start_time, &decode_end_time, &network_start_time, &network_end_time, &cross_rack_network_time, &cross_rack_xor_time,
        &grpc_notify_time, &grpc_start_time, &data_node_grpc_notify_time, &data_node_grpc_start_time](){
        grpc::ClientContext recovery_context;
        proxy_proto::RecoveryRequest recovery_request;
        proxy_proto::DegradedReadReply recovery_reply;
        recovery_request.set_failed_block_id(failed_block_id);
        recovery_request.set_failed_block_key(t_stripe.blocks[failed_block_id]->block_key);
        recovery_request.set_replaced_node_ip(client_ip);
        recovery_request.set_replaced_node_port(client_port);
        recovery_request.set_cross_rack_num(cross_rack_num);
        std::vector<int> blockids = t_stripe.group_to_blocks[dest_group_id];
        for (int i = 0; i < int(blockids.size()); i++)
        {
          if(m_sys_config->CodeType == "AzureLRC" && recovery_request.blockids_size() == (m_sys_config->k / m_sys_config->z))
            break;

          if (blockids[i] == failed_block_id)
            continue;

          Block *t_block = t_stripe.blocks[blockids[i]];
          recovery_request.add_datanodeip(this->m_node_table[t_block->map2node].node_ip);
          recovery_request.add_datanodeport(this->m_node_table[t_block->map2node].node_port);
          recovery_request.add_blockkeys(t_block->block_key);
          recovery_request.add_blockids(t_block->block_id);
        }
        std::cout << "[Coordinator] start recovery of " << stripe_id << "_" << failed_block_id << std::endl;
        std::chrono::high_resolution_clock::time_point grpc_notify = std::chrono::high_resolution_clock::now();
        grpc_notify_time.push_back(std::chrono::duration_cast<std::chrono::duration<double>>(grpc_notify.time_since_epoch()).count());
        grpc::Status status = this->m_proxy_ptrs[dest_proxy_ip + ":" + std::to_string(dest_proxy_port)]->degradedRead2ClientBreakdown(&recovery_context, recovery_request, &recovery_reply);
        if (status.ok())
        {
          disk_io_start_time.push_back(recovery_reply.disk_io_start_time());
          disk_io_end_time.push_back(recovery_reply.disk_io_end_time());
          decode_start_time.push_back(recovery_reply.decode_start_time());
          decode_end_time.push_back(recovery_reply.decode_end_time());
          network_start_time.push_back(recovery_reply.network_start_time());
          network_end_time.push_back(recovery_reply.network_end_time());
          cross_rack_network_time = recovery_reply.cross_rack_time();
          cross_rack_xor_time = recovery_reply.cross_rack_xor_time();
          grpc_start_time.push_back(recovery_reply.grpc_start_time());
          data_node_grpc_notify_time.push_back(recovery_reply.data_node_grpc_notify_time());
          data_node_grpc_start_time.push_back(recovery_reply.data_node_grpc_start_time());
          std::cout << "[Coordinator] degraded read of " << stripe_id << "_" << failed_block_id << " success!" << std::endl;
        }
        else
        {
          std::cout << "[Coordinator] degraded read of " << stripe_id << "_" << failed_block_id << " failed!" << std::endl;
        }
      }
      ));
      for(int i = 0; i < threads.size(); i++){
        threads[i].join();
      }
    }
    return true;
  }

  bool CoordinatorImpl::degraded_read_one_block(int stripe_id, int failed_block_id, std::string client_ip, int client_port)
  {
    Stripe &t_stripe = m_stripe_table[stripe_id];
    auto plan = ECProject::get_recovery_group_and_block_ids(m_sys_config->CodeType, m_sys_config->k, m_sys_config->r, m_sys_config->z, failed_block_id);
    grpc::Status status;

    if (!plan.empty())
    {
      if (plan.size() == 1)
      {
        int group_id = plan[0].first;
        const std::vector<int> &block_ids = plan[0].second;
        int chosen_cluster_id = get_cluster_id_by_group_id(t_stripe, group_id);
        std::string chosen_proxy = m_cluster_table[chosen_cluster_id].proxy_ip + ":" + std::to_string(m_cluster_table[chosen_cluster_id].proxy_port);
        grpc::ClientContext recovery_context;
        proxy_proto::RecoveryRequest recovery_request;
        proxy_proto::DegradedReadReply degraded_read_reply;
        recovery_request.set_failed_block_id(failed_block_id);
        recovery_request.set_failed_block_key(t_stripe.blocks[failed_block_id]->block_key);
        recovery_request.set_replaced_node_ip(client_ip);
        recovery_request.set_replaced_node_port(client_port);
        recovery_request.set_cross_rack_num(0);
        add_block_list_to_recovery_request(t_stripe, block_ids, &recovery_request);
        status = m_proxy_ptrs[chosen_proxy]->degradedRead2Client(&recovery_context, recovery_request, &degraded_read_reply);
        if (status.ok())
        {
          std::cout << "[Coordinator] degraded read of " << stripe_id << "_" << failed_block_id << " success!" << std::endl;
          return true;
        }
        std::cout << "[Coordinator] degraded read of " << stripe_id << "_" << failed_block_id << " failed!" << std::endl;
        return false;
      }
      int dest_group_id = t_stripe.blocks[failed_block_id]->map2group;
      int dest_cluster_id = get_cluster_id_by_group_id(t_stripe, dest_group_id);
      std::string dest_proxy_ip = m_cluster_table[dest_cluster_id].proxy_ip;
      int dest_proxy_port = m_cluster_table[dest_cluster_id].proxy_port;
      std::vector<std::string> chosen_proxies;
      for (size_t i = 0; i < plan.size(); i++)
        chosen_proxies.push_back(m_cluster_table[get_cluster_id_by_group_id(t_stripe, plan[i].first)].proxy_ip + ":" + std::to_string(m_cluster_table[get_cluster_id_by_group_id(t_stripe, plan[i].first)].proxy_port));
      std::vector<std::thread> threads;
      int cross_rack_num = 0;
      for (size_t i = 0; i < plan.size(); i++)
      {
        if (plan[i].first == dest_group_id)
          continue;
        std::vector<int> block_ids = plan[i].second;
        std::string proxy_key = chosen_proxies[i];
        threads.push_back(std::thread([this, &t_stripe, proxy_key, block_ids, failed_block_id, dest_proxy_ip, dest_proxy_port]() {
          grpc::ClientContext degraded_read_context;
          proxy_proto::DegradedReadRequest degraded_read_request;
          proxy_proto::DegradedReadReply degraded_read_reply;
          degraded_read_request.set_clientip(dest_proxy_ip);
          degraded_read_request.set_clientport(dest_proxy_port + ECProject::PROXY_PORT_SHIFT);
          degraded_read_request.set_failed_block_id(failed_block_id);
          degraded_read_request.set_failed_block_key(t_stripe.blocks[failed_block_id]->block_key);
          add_block_list_to_degraded_read_request(t_stripe, block_ids, &degraded_read_request);
          grpc::Status st = m_proxy_ptrs[proxy_key]->degradedRead(&degraded_read_context, degraded_read_request, &degraded_read_reply);
          if (st.ok())
            std::cout << "[Coordinator] partial degraded read of " << failed_block_id << " success!" << std::endl;
          else
            std::cout << "[Coordinator] partial degraded read of " << failed_block_id << " failed!" << std::endl;
        }));
        cross_rack_num++;
      }
      std::vector<int> dest_block_ids;
      for (size_t i = 0; i < plan.size(); i++)
        if (plan[i].first == dest_group_id)
        {
          dest_block_ids = plan[i].second;
          break;
        }
      threads.push_back(std::thread([this, &t_stripe, dest_proxy_ip, dest_proxy_port, stripe_id, failed_block_id, client_ip, client_port, cross_rack_num, dest_group_id, dest_block_ids, &plan]() {
        grpc::ClientContext recovery_context;
        proxy_proto::RecoveryRequest recovery_request;
        proxy_proto::DegradedReadReply recovery_reply;
        recovery_request.set_failed_block_id(failed_block_id);
        recovery_request.set_failed_block_key(t_stripe.blocks[failed_block_id]->block_key);
        recovery_request.set_replaced_node_ip(client_ip);
        recovery_request.set_replaced_node_port(client_port);
        recovery_request.set_cross_rack_num(cross_rack_num);
        for (size_t i = 0; i < plan.size(); i++)
          if (plan[i].first != dest_group_id)
          {
            int cid = get_cluster_id_by_group_id(t_stripe, plan[i].first);
            recovery_request.add_proxyip(m_cluster_table[cid].proxy_ip);
            recovery_request.add_proxyport(m_cluster_table[cid].proxy_port);
          }
        add_block_list_to_recovery_request(t_stripe, dest_block_ids, &recovery_request);
        grpc::Status st = m_proxy_ptrs[dest_proxy_ip + ":" + std::to_string(dest_proxy_port)]->degradedRead2Client(&recovery_context, recovery_request, &recovery_reply);
        if (st.ok())
          std::cout << "[Coordinator] degraded read of " << stripe_id << "_" << failed_block_id << " success!" << std::endl;
        else
          std::cout << "[Coordinator] degraded read of " << stripe_id << "_" << failed_block_id << " failed!" << std::endl;
      }));
      for (size_t i = 0; i < threads.size(); i++)
        threads[i].join();
      return true;
    }
    std::cout << "[Coordinator] degraded_read_one_block: get_recovery_group_and_block_ids returned empty" << std::endl;
    return false;
  }

  bool CoordinatorImpl::degraded_read_one_block_for_workload(int stripe_id, int failed_block_id, std::string client_ip, int client_port, int block_id)
  {
    Stripe &t_stripe = m_stripe_table[stripe_id];
    auto plan = ECProject::get_recovery_group_and_block_ids(m_sys_config->CodeType, m_sys_config->k, m_sys_config->r, m_sys_config->z, failed_block_id);
    grpc::Status status;

    if (!plan.empty())
    {
      if (plan.size() == 1)
      {
        int group_id = plan[0].first;
        const std::vector<int> &block_ids = plan[0].second;
        int chosen_cluster_id = get_cluster_id_by_group_id(t_stripe, group_id);
        std::string chosen_proxy = m_cluster_table[chosen_cluster_id].proxy_ip + ":" + std::to_string(m_cluster_table[chosen_cluster_id].proxy_port);
        grpc::ClientContext recovery_context;
        proxy_proto::RecoveryRequest recovery_request;
        proxy_proto::DegradedReadReply degraded_read_reply;
        recovery_request.set_failed_block_id(failed_block_id);
        recovery_request.set_failed_block_key(t_stripe.blocks[failed_block_id]->block_key);
        recovery_request.set_replaced_node_ip(client_ip);
        recovery_request.set_replaced_node_port(client_port);
        recovery_request.set_cross_rack_num(0);
        recovery_request.set_is_to_send_block_id(true);
        recovery_request.set_block_id_to_send(block_id);
        add_block_list_to_recovery_request(t_stripe, block_ids, &recovery_request);
        status = m_proxy_ptrs[chosen_proxy]->degradedRead2Client(&recovery_context, recovery_request, &degraded_read_reply);
        if (status.ok())
        {
          std::cout << "[Coordinator] degraded read of " << stripe_id << "_" << failed_block_id << " success!" << std::endl;
          return true;
        }
        std::cout << "[Coordinator] degraded read of " << stripe_id << "_" << failed_block_id << " failed!" << std::endl;
        return false;
      }
      int dest_group_id = t_stripe.blocks[failed_block_id]->map2group;
      int dest_cluster_id = get_cluster_id_by_group_id(t_stripe, dest_group_id);
      std::string dest_proxy_ip = m_cluster_table[dest_cluster_id].proxy_ip;
      int dest_proxy_port = m_cluster_table[dest_cluster_id].proxy_port;
      std::vector<std::string> chosen_proxies;
      for (size_t i = 0; i < plan.size(); i++)
        chosen_proxies.push_back(m_cluster_table[get_cluster_id_by_group_id(t_stripe, plan[i].first)].proxy_ip + ":" + std::to_string(m_cluster_table[get_cluster_id_by_group_id(t_stripe, plan[i].first)].proxy_port));
      std::vector<std::thread> threads;
      for (size_t i = 0; i < plan.size(); i++)
      {
        if (plan[i].first == dest_group_id)
          continue;
        std::vector<int> block_ids = plan[i].second;
        std::string proxy_key = chosen_proxies[i];
        threads.push_back(std::thread([this, &t_stripe, proxy_key, block_ids, failed_block_id, dest_proxy_ip, dest_proxy_port]() {
          grpc::ClientContext degraded_read_context;
          proxy_proto::DegradedReadRequest degraded_read_request;
          proxy_proto::DegradedReadReply degraded_read_reply;
          degraded_read_request.set_clientip(dest_proxy_ip);
          degraded_read_request.set_clientport(dest_proxy_port + ECProject::PROXY_PORT_SHIFT);
          degraded_read_request.set_failed_block_id(failed_block_id);
          degraded_read_request.set_failed_block_key(t_stripe.blocks[failed_block_id]->block_key);
          add_block_list_to_degraded_read_request(t_stripe, block_ids, &degraded_read_request);
          grpc::Status st = m_proxy_ptrs[proxy_key]->degradedRead(&degraded_read_context, degraded_read_request, &degraded_read_reply);
          if (st.ok())
            std::cout << "[Coordinator] partial degraded read of " << failed_block_id << " success!" << std::endl;
          else
            std::cout << "[Coordinator] partial degraded read of " << failed_block_id << " failed!" << std::endl;
        }));
      }
      int cross_rack_num = (int)plan.size() - 1;
      std::vector<int> dest_block_ids;
      for (size_t i = 0; i < plan.size(); i++)
        if (plan[i].first == dest_group_id)
        {
          dest_block_ids = plan[i].second;
          break;
        }
      threads.push_back(std::thread([this, &t_stripe, dest_proxy_ip, dest_proxy_port, stripe_id, failed_block_id, client_ip, client_port, block_id, cross_rack_num, dest_group_id, dest_block_ids, &plan]() {
        grpc::ClientContext recovery_context;
        proxy_proto::RecoveryRequest recovery_request;
        proxy_proto::DegradedReadReply recovery_reply;
        recovery_request.set_failed_block_id(failed_block_id);
        recovery_request.set_failed_block_key(t_stripe.blocks[failed_block_id]->block_key);
        recovery_request.set_replaced_node_ip(client_ip);
        recovery_request.set_replaced_node_port(client_port);
        recovery_request.set_cross_rack_num(cross_rack_num);
        recovery_request.set_is_to_send_block_id(true);
        recovery_request.set_block_id_to_send(block_id);
        for (size_t i = 0; i < plan.size(); i++)
          if (plan[i].first != dest_group_id)
          {
            int cid = get_cluster_id_by_group_id(t_stripe, plan[i].first);
            recovery_request.add_proxyip(m_cluster_table[cid].proxy_ip);
            recovery_request.add_proxyport(m_cluster_table[cid].proxy_port);
          }
        add_block_list_to_recovery_request(t_stripe, dest_block_ids, &recovery_request);
        grpc::Status st = m_proxy_ptrs[dest_proxy_ip + ":" + std::to_string(dest_proxy_port)]->degradedRead2Client(&recovery_context, recovery_request, &recovery_reply);
        if (st.ok())
          std::cout << "[Coordinator] degraded read of " << stripe_id << "_" << failed_block_id << " success!" << std::endl;
        else
          std::cout << "[Coordinator] degraded read of " << stripe_id << "_" << failed_block_id << " failed!" << std::endl;
      }));
      for (size_t i = 0; i < threads.size(); i++)
        threads[i].join();
      return true;
    }
    std::cout << "[Coordinator] degraded_read_one_block_for_workload: get_recovery_group_and_block_ids returned empty" << std::endl;
    return false;
  }


  grpc::Status CoordinatorImpl::getDegradedReadBlockBreakdown(
    grpc::ServerContext *context,
    const coordinator_proto::KeyAndClientIP *keyClient,
    coordinator_proto::DegradedReadReply *degradedReadReply)
  {
    std::chrono::high_resolution_clock::time_point start = std::chrono::high_resolution_clock::now();
    double start_time = std::chrono::duration_cast<std::chrono::duration<double>>(start.time_since_epoch()).count();
    degradedReadReply->set_grpc_start_time(start_time);
    std::cout << start_time << std::endl;
    int stripe_id = std::stoi(keyClient->key().substr(0, keyClient->key().find('_')));
    int failed_block_id = std::stoi(keyClient->key().substr(keyClient->key().find('_') + 1));
    std::string client_ip = keyClient->clientip();
    int client_port = keyClient->clientport();
    std::vector<double> disk_io_start_time;
    std::vector<double> disk_io_end_time;
    std::vector<double> decode_start_time;
    std::vector<double> decode_end_time;
    std::vector<double> network_start_time;
    std::vector<double> network_end_time;
    std::vector<double> grpc_notify_time;
    std::vector<double> grpc_start_time;
    std::vector<double> data_node_grpc_notify_time;
    std::vector<double> data_node_grpc_start_time;
    double cross_rack_network_time;
    double cross_rack_xor_time;

    /*std::thread t(&CoordinatorImpl::degraded_read_one_block, this, stripe_id, failed_block_id, client_ip, client_port);
    t.join();*/
    bool if_success = degraded_read_one_block_breakdown(stripe_id, failed_block_id, client_ip, client_port,
      disk_io_start_time, disk_io_end_time, decode_start_time, decode_end_time,
      network_start_time, network_end_time, cross_rack_network_time, cross_rack_xor_time,
      grpc_notify_time, grpc_start_time, data_node_grpc_notify_time, data_node_grpc_start_time); 
    if (if_success)
    {
      double max_disk_io_time = *std::max_element(disk_io_end_time.begin(), disk_io_end_time.end()) - *std::min_element(disk_io_start_time.begin(), disk_io_start_time.end());
      double max_decode_time = *std::max_element(decode_end_time.begin(), decode_end_time.end()) - *std::min_element(decode_start_time.begin(), decode_start_time.end()) + cross_rack_xor_time;
      double max_network_time = *std::max_element(network_end_time.begin(), network_end_time.end()) - *std::min_element(network_start_time.begin(), network_start_time.end()) + cross_rack_network_time;
      max_network_time += (*std::max_element(grpc_start_time.begin(), grpc_start_time.end()) - *std::min_element(grpc_notify_time.begin(), grpc_notify_time.end()));
      max_network_time += (*std::max_element(data_node_grpc_start_time.begin(), data_node_grpc_start_time.end()) - *std::min_element(data_node_grpc_notify_time.begin(), data_node_grpc_notify_time.end()));

      degradedReadReply->set_disk_io_time(max_disk_io_time);
      degradedReadReply->set_decode_time(max_decode_time);
      degradedReadReply->set_network_time(max_network_time);
      return grpc::Status::OK;
    }
    else
    {
      return grpc::Status(grpc::StatusCode::INTERNAL, "Degraded read failed!");
    }
  }


  grpc::Status CoordinatorImpl::getDegradedReadBlock(
      grpc::ServerContext *context,
      const coordinator_proto::KeyAndClientIP *keyClient,
      coordinator_proto::DegradedReadReply *degradedReadReply)
  {
    int stripe_id = std::stoi(keyClient->key().substr(0, keyClient->key().find('_')));
    int failed_block_id = std::stoi(keyClient->key().substr(keyClient->key().find('_') + 1));
    std::string client_ip = keyClient->clientip();
    int client_port = keyClient->clientport();

    double dest_proxy_network_time;
    bool if_success = degraded_read_one_block(stripe_id, failed_block_id, client_ip, client_port);
    if (if_success)
    {
      return grpc::Status::OK;
    }
    else
    {
      return grpc::Status(grpc::StatusCode::INTERNAL, "Degraded read failed!");
    }
  }

  namespace {
    int block_to_placement_group_id(const std::string &code, int k, int r, int z, int bid)
    {
      try {
        if (code == "AzureLRC")   return ECProject::get_azurelrc_block_id_to_group_id(k, r, z).at(bid);
        if (code == "OptimalLRC") return ECProject::get_optimal_lrc_block_id_to_group_id(k, r, z).at(bid);
        if (code == "UniformLRC") return ECProject::get_uniform_lrc_block_id_to_group_id(k, r, z).at(bid);
        if (code == "LotusLRC")   return ECProject::get_lotuslrc_block_id_to_group_id(k, r, z).at(bid);
      } catch (const std::exception &) {
        return -1;
      }
      return -1;
    }

    int recovery_dest_cluster(const ECProject::Config *cfg, int stripe_id, int failed_block_id)
    {
      const std::string &code = cfg->CodeType;
      const int k = cfg->k, r = cfg->r, z = cfg->z;
      const int CN = cfg->ClusterNum;
      if (CN <= 0)
        return -1;
      int fg = block_to_placement_group_id(code, k, r, z, failed_block_id);
      if (fg < 0)
        return -1;
      return ((stripe_id + fg) % CN + CN) % CN;
    }
  } // namespace

  bool CoordinatorImpl::stripe_recovery_for_failed_blocks(int stripe_id,
                                                          const std::vector<int> &failed_blocks)
  {
    const int n = static_cast<int>(failed_blocks.size());
    if (n == 0)
      return true;
    if (n == 1)
      return recovery_one_block(stripe_id, failed_blocks[0]);

    if (n != 2) {
      std::cout << "[Coordinator] stripe " << stripe_id << ": " << n
                << " failed blocks on two nodes (>2), serial single-block recovery" << std::endl;
      bool ok = true;
      for (int bid : failed_blocks)
        ok = recovery_one_block(stripe_id, bid) && ok;
      return ok;
    }

    const int f0 = failed_blocks[0];
    const int f1 = failed_blocks[1];
    const std::string &code_type = m_sys_config->CodeType;
    const int k = m_sys_config->k;
    const int r = m_sys_config->r;
    const int z = m_sys_config->z;

    ECProject::TwoBlockRecoveryMode mode =
        ECProject::select_two_block_recovery_mode(code_type, k, r, z, f0, f1);

    switch (mode) {
    case ECProject::TwoBlockRecoveryMode::TwoSingleBlock: {
      int d0 = recovery_dest_cluster(m_sys_config, stripe_id, f0);
      int d1 = recovery_dest_cluster(m_sys_config, stripe_id, f1);
      if (d0 >= 0 && d1 >= 0 && d0 != d1) {
        std::cout << "[Coordinator] stripe " << stripe_id
                  << " two-block: different local groups, PARALLEL (dest " << d0 << "," << d1
                  << ")" << std::endl;
        bool ok0 = false, ok1 = false;
        std::thread t0([&]() { ok0 = recovery_one_block(stripe_id, f0); });
        std::thread t1([&]() { ok1 = recovery_one_block(stripe_id, f1); });
        t0.join();
        t1.join();
        return ok0 && ok1;
      }
      std::cout << "[Coordinator] stripe " << stripe_id
                << " two-block: different local groups, SERIAL (d0=" << d0 << ", d1=" << d1
                << ")" << std::endl;
      return recovery_one_block(stripe_id, f0) && recovery_one_block(stripe_id, f1);
    }

    case ECProject::TwoBlockRecoveryMode::GlobalThenSingle: {
      const int first = std::min(f0, f1);
      const int second = std::max(f0, f1);
      std::cout << "[Coordinator] stripe " << stripe_id
                << " two-block: same local group (non-Lotus), global " << first << " then single "
                << second << std::endl;
      return execute_global_recovery(stripe_id, {f0, f1}, {first}) &&
             recovery_one_block(stripe_id, second);
    }

    case ECProject::TwoBlockRecoveryMode::LotusSameGroupPlanBased:
      std::cout << "[Coordinator] stripe " << stripe_id
                << " two-block: Lotus same local group, one-round local recovery" << std::endl;
      return execute_global_recovery(stripe_id, {f0, f1}, {});
    }
    return false;
  }

  grpc::Status CoordinatorImpl::twoNodeRecovery(
      grpc::ServerContext *context,
      const coordinator_proto::TwoNodeIdsFromClient *request,
      coordinator_proto::RepBlockNum *response)
  {
    const int node_id_0 = request->node_id_0();
    const int node_id_1 = request->node_id_1();
    if (m_node_table.find(node_id_0) == m_node_table.end() ||
        m_node_table.find(node_id_1) == m_node_table.end()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "invalid node id");
    }

    int total_blocks = 0;
    std::vector<bool> stripe_results;
    for (auto it = m_stripe_table.begin(); it != m_stripe_table.end(); ++it) {
      const int stripe_id = it->first;
      Stripe &stripe = it->second;
      std::vector<int> failed_on_nodes;
      for (size_t i = 0; i < stripe.blocks.size(); ++i) {
        const int nid = stripe.blocks[i]->map2node;
        if (nid == node_id_0 || nid == node_id_1)
          failed_on_nodes.push_back(stripe.blocks[i]->block_id);
      }
      if (failed_on_nodes.empty())
        continue;

      total_blocks += static_cast<int>(failed_on_nodes.size());
      std::cout << "[Coordinator] two-node recovery stripe " << stripe_id << ": "
                << failed_on_nodes.size() << " block(s) on nodes " << node_id_0 << "," << node_id_1
                << std::endl;
      stripe_results.push_back(stripe_recovery_for_failed_blocks(stripe_id, failed_on_nodes));
    }

    if (total_blocks == 0) {
      std::cout << "[Coordinator] no blocks on nodes " << node_id_0 << "," << node_id_1 << std::endl;
      response->set_block_num(0);
      return grpc::Status::OK;
    }

    response->set_block_num(total_blocks);
    const bool all_success =
        stripe_results.empty() ||
        std::all_of(stripe_results.begin(), stripe_results.end(), [](bool ok) { return ok; });
    std::cout << "[Coordinator] two-node recovery of " << node_id_0 << "," << node_id_1
              << " containing " << total_blocks << " blocks, "
              << (all_success ? "all succeeded" : "some failed") << std::endl;
    return grpc::Status::OK;
  }

  grpc::Status CoordinatorImpl::fullNodeRecovery(
    grpc::ServerContext *context,
    const coordinator_proto::NodeIdFromClient *request,
    coordinator_proto::RepBlockNum* response)
  {
    int node_id = request->node_id();
    std::string node_ip = m_node_table[node_id].node_ip;
    int node_port = m_node_table[node_id].node_port;
    std::vector<int> stripe_ids;
    std::vector<int> block_ids;
    for (auto it = m_stripe_table.begin(); it != m_stripe_table.end(); it++)
    {
      for (int i = 0; i < int(it->second.blocks.size()); i++)
      {
        if (it->second.blocks[i]->map2node == node_id)
        {
          stripe_ids.push_back(it->first);
          block_ids.push_back(it->second.blocks[i]->block_id);
        }
      }
    }
    if(stripe_ids.size() == 0){
      std::cout << "[Coordinator] no blocks on node " << node_id << std::endl;
      return grpc::Status::OK;
    }
    std::cout << "[Coordinator] start full node recovery of " << node_id << " containing " << block_ids.size() << " blocks" << std::endl;
    response->set_block_num(block_ids.size());
    //recovery_full_node(stripe_ids, block_ids);
    //std::vector<std::thread> recovery_threads;
    std::vector<bool> recovery_results(stripe_ids.size(), false);
    for (int i = 0; i < stripe_ids.size(); i++) {
        bool result = this->recovery_one_block(stripe_ids[i], block_ids[i]);
        recovery_results[i] = result; // 保存结果
    }
  
        
    // 检查结果
    bool all_success = std::all_of(recovery_results.begin(), recovery_results.end(), [](bool res) { return res; });
    if (all_success) {
        std::cout << "All recovery operations succeeded!" << std::endl;
    } else {
        std::cout << "Some recovery operations failed!" << std::endl;
    }


    /*bool ifSuccess = recovery_full_node(stripe_ids, block_ids);
    if (ifSuccess)
    {
      return grpc::Status::OK;
    }
    else
    {
      return grpc::Status(grpc::StatusCode::INTERNAL, "Full node recovery failed!");
    }*/
    return grpc::Status::OK;
  } 

  bool CoordinatorImpl::execute_global_recovery(int stripe_id,
                                                const std::vector<int> &all_failed,
                                                const std::vector<int> &recovery_block_ids)
  {
    const int all_failed_num = static_cast<int>(all_failed.size());
    std::vector<int> recover;
    if (!recovery_block_ids.empty()) {
      std::unordered_set<int> all_failed_set(all_failed.begin(), all_failed.end());
      recover.reserve(recovery_block_ids.size());
      for (int bid : recovery_block_ids) {
        if (!all_failed_set.count(bid)) {
          std::cout << "[Coordinator] execute_global_recovery: recovery_block_id " << bid
                    << " not in all_failed" << std::endl;
          return false;
        }
        recover.push_back(bid);
      }
    } else {
      recover = all_failed;
    }
    const int recover_num = static_cast<int>(recover.size());
    if (recover_num == 0)
      return false;

    // LotusLRC two failed blocks in the same local group: the global decode plan below
    // automatically uses a one-round local-group plan (each helper returns 2 x BlockSize,
    // dest combines both blocks). Probe here only to log which path is taken; if the
    // local span is insufficient it falls back to the generic global k x k recovery.
    if (all_failed_num == 2 && recover_num == 2 &&
        m_sys_config->CodeType == "LotusLRC" &&
        ECProject::blocks_same_local_group(m_sys_config->CodeType, m_sys_config->k, m_sys_config->r,
                                           m_sys_config->z, all_failed[0], all_failed[1])) {
      std::vector<int> probe_sources;
      std::vector<unsigned char> probe_coeffs;
      bool local_oneround = ECProject::get_lotus_two_block_local_plan(
          m_sys_config->k, m_sys_config->r, m_sys_config->z, all_failed, recover,
          probe_sources, probe_coeffs);
      if (local_oneround) {
        std::cout << "[Coordinator] LotusLRC same-local-group two-block: ONE-ROUND LOCAL recovery, "
                  << "reading " << probe_sources.size() << " local-group source blocks" << std::endl;
      } else {
        std::cout << "[Coordinator] LotusLRC same-local-group two-block: local span insufficient, "
                  << "FALLBACK to GLOBAL recovery" << std::endl;
      }
    }

    std::vector<int> decode_block_ids;
    int rows = 0, cols = 0;
    std::cout << "[Coordinator] get global decode plan start" << std::endl;
    bool ifGetDecodePlanSuccess = ECProject::get_global_decode_plan(
        m_sys_config->k, m_sys_config->r, m_sys_config->z, m_sys_config->CodeType,
        all_failed, decode_block_ids, nullptr, nullptr, rows, cols, nullptr);
    if (!ifGetDecodePlanSuccess) {
      std::cout << "[Coordinator] get multi decode plan failed!" << std::endl;
      return false;
    }
    std::cout << "[Coordinator] get multi decode plan success! " << decode_block_ids.size() << " blocks to decode" << std::endl;

    Stripe &t_stripe = m_stripe_table[stripe_id];
    // Group decode blocks by cluster (cluster_id -> list of block ids in that cluster)
    std::vector<int> clusters_with_blocks;
    std::vector<std::vector<int>> decode_blocks_per_cluster;
    for (size_t i = 0; i < decode_block_ids.size(); i++)
    {
      int cid = t_stripe.blocks[decode_block_ids[i]]->map2cluster;
      auto it = std::find(clusters_with_blocks.begin(), clusters_with_blocks.end(), cid);
      if (it == clusters_with_blocks.end())
      {
        clusters_with_blocks.push_back(cid);
        decode_blocks_per_cluster.push_back(std::vector<int>(1, decode_block_ids[i]));
      }
      else
      {
        size_t idx = std::distance(clusters_with_blocks.begin(), it);
        decode_blocks_per_cluster[idx].push_back(decode_block_ids[i]);
      }
    }
    // Dest cluster: first block to recover this round
    int dest_cluster_id = t_stripe.blocks[recover[0]]->map2cluster;
    std::string dest_proxy_ip = m_cluster_table[dest_cluster_id].proxy_ip;
    int dest_proxy_port = m_cluster_table[dest_cluster_id].proxy_port;
    std::string dest_proxy_key = dest_proxy_ip + ":" + std::to_string(dest_proxy_port);
    int cross_rack_num = 0;
    for (size_t i = 0; i < clusters_with_blocks.size(); i++)
      if (clusters_with_blocks[i] != dest_cluster_id)
        cross_rack_num++;

    // Replaced node and key per recovered block (for dest recovery request)
    std::vector<std::string> replaced_ips(recover_num);
    std::vector<int> replaced_ports(recover_num);
    std::vector<std::string> failed_keys(recover_num);
    for (int f = 0; f < recover_num; f++)
    {
      int bid = recover[f];
      // For globalRecovery, write each recovered block back to its original node.
      int node_id = t_stripe.blocks[bid]->map2node;
      replaced_ips[f] = m_node_table[node_id].node_ip;
      replaced_ports[f] = m_node_table[node_id].node_port;
      failed_keys[f] = t_stripe.blocks[bid]->block_key;
    }

    // Run dest recovery in a thread (it will block on accept), then call degradedRead on each non-dest source
    std::vector<std::thread> threads;
    grpc::Status dest_status;
    std::mutex dest_status_mutex;
    threads.push_back(std::thread([this, &t_stripe, dest_proxy_key, dest_cluster_id, stripe_id, recover_num, &all_failed, &recover, &decode_block_ids, &failed_keys, &replaced_ips, &replaced_ports, cross_rack_num, &decode_blocks_per_cluster, &clusters_with_blocks, &dest_status, &dest_status_mutex]() {
      grpc::ClientContext recovery_context;
      proxy_proto::RecoveryRequest recovery_request;
      proxy_proto::RecoveryReply recovery_reply;
      recovery_request.set_cross_rack_num(cross_rack_num);
      std::cout << "[Coordinator] globalRecovery dest_cluster_id: " << dest_cluster_id << std::endl;
      std::cout << "[Coordinator] globalRecovery cross_rack_num: " << cross_rack_num << std::endl;
      for (size_t i = 0; i < all_failed.size(); i++)
        recovery_request.add_all_failed_block_ids(all_failed[i]);
      for (int i = 0; i < recover_num; i++)
      {
        recovery_request.add_failed_block_ids(recover[i]);
        recovery_request.add_failed_block_keys(failed_keys[i]);
        recovery_request.add_replaced_node_ips(replaced_ips[i]);
        recovery_request.add_replaced_node_ports(replaced_ports[i]);
      }
      for (size_t i = 0; i < decode_block_ids.size(); i++)
        recovery_request.add_decode_block_ids(decode_block_ids[i]);
      size_t dest_idx = 0;
      for (; dest_idx < clusters_with_blocks.size(); dest_idx++)
        if (clusters_with_blocks[dest_idx] == dest_cluster_id)
          break;
      if (dest_idx < clusters_with_blocks.size())
        add_block_list_to_recovery_request(t_stripe, decode_blocks_per_cluster[dest_idx], &recovery_request);
      grpc::Status st = m_proxy_ptrs[dest_proxy_key]->recovery(&recovery_context, recovery_request, &recovery_reply);
      std::lock_guard<std::mutex> lock(dest_status_mutex);
      dest_status = st;
    }));

    for (size_t i = 0; i < clusters_with_blocks.size(); i++)
    {
      if (clusters_with_blocks[i] == dest_cluster_id)
        continue;
      std::string proxy_key = m_cluster_table[clusters_with_blocks[i]].proxy_ip + ":" + std::to_string(m_cluster_table[clusters_with_blocks[i]].proxy_port);
      std::cout << "[Coordinator] globalRecovery proxy_key: " << proxy_key << std::endl;
      std::cout << "[Coordinator] globalRecovery clusters_with_blocks[i] number: " << clusters_with_blocks[i] << std::endl;
      threads.push_back(std::thread([this, &t_stripe, proxy_key, dest_proxy_ip, dest_proxy_port, stripe_id, recover_num, &all_failed, &recover, &decode_block_ids, &decode_blocks_per_cluster, i]() {
        grpc::ClientContext degraded_context;
        proxy_proto::DegradedReadRequest degraded_request;
        proxy_proto::DegradedReadReply degraded_reply;
        degraded_request.set_clientip(dest_proxy_ip);
        degraded_request.set_clientport(dest_proxy_port + ECProject::PROXY_PORT_SHIFT);
        for (size_t j = 0; j < all_failed.size(); j++)
          degraded_request.add_all_failed_block_ids(all_failed[j]);
        for (int j = 0; j < recover_num; j++)
          degraded_request.add_failed_block_ids(recover[j]);
        for (size_t j = 0; j < decode_block_ids.size(); j++)
          degraded_request.add_decode_block_ids(decode_block_ids[j]);
        add_block_list_to_degraded_read_request(t_stripe, decode_blocks_per_cluster[i], &degraded_request);
        grpc::Status st = m_proxy_ptrs[proxy_key]->degradedRead(&degraded_context, degraded_request, &degraded_reply);
        if (!st.ok())
          std::cout << "[Coordinator] globalRecovery degradedRead from proxy " << proxy_key << " failed: " << st.error_message() << std::endl;
      }));
    }

    for (size_t i = 1; i < threads.size(); i++)
      threads[i].detach();
    threads[0].join();

    {
      std::lock_guard<std::mutex> lock(dest_status_mutex);
      if (!dest_status.ok()) {
        std::cout << "[Coordinator] globalRecovery recovery on dest failed: "
                  << dest_status.error_message() << std::endl;
        return false;
      }
    }
    std::cout << "[Coordinator] globalRecovery success for stripe " << stripe_id
              << " recovered " << recover_num << " / " << all_failed_num << " failed blocks"
              << std::endl;
    return true;
  }

  bool CoordinatorImpl::execute_global_recovery_breakdown(int stripe_id,
                                                          const std::vector<int> &all_failed,
                                                          const std::vector<int> &recovery_block_ids,
                                                          coordinator_proto::RecoveryReply *breakdown_reply)
  {
    const int all_failed_num = static_cast<int>(all_failed.size());
    std::vector<int> recover;
    if (!recovery_block_ids.empty()) {
      std::unordered_set<int> all_failed_set(all_failed.begin(), all_failed.end());
      recover.reserve(recovery_block_ids.size());
      for (int bid : recovery_block_ids) {
        if (!all_failed_set.count(bid)) {
          std::cout << "[Coordinator] execute_global_recovery_breakdown: recovery_block_id " << bid
                    << " not in all_failed" << std::endl;
          return false;
        }
        recover.push_back(bid);
      }
    } else {
      recover = all_failed;
    }
    const int recover_num = static_cast<int>(recover.size());
    if (recover_num == 0)
      return false;

    std::vector<int> decode_block_ids;
    int rows = 0, cols = 0;
    if (!ECProject::get_global_decode_plan(m_sys_config->k, m_sys_config->r, m_sys_config->z,
                                           m_sys_config->CodeType, all_failed, decode_block_ids,
                                           nullptr, nullptr, rows, cols, nullptr)) {
      std::cout << "[Coordinator] execute_global_recovery_breakdown: get multi decode plan failed!"
                << std::endl;
      return false;
    }

    Stripe &t_stripe = m_stripe_table[stripe_id];
    std::vector<int> clusters_with_blocks;
    std::vector<std::vector<int>> decode_blocks_per_cluster;
    for (size_t i = 0; i < decode_block_ids.size(); i++)
    {
      int cid = t_stripe.blocks[decode_block_ids[i]]->map2cluster;
      auto it = std::find(clusters_with_blocks.begin(), clusters_with_blocks.end(), cid);
      if (it == clusters_with_blocks.end())
      {
        clusters_with_blocks.push_back(cid);
        decode_blocks_per_cluster.push_back(std::vector<int>(1, decode_block_ids[i]));
      }
      else
      {
        size_t idx = std::distance(clusters_with_blocks.begin(), it);
        decode_blocks_per_cluster[idx].push_back(decode_block_ids[i]);
      }
    }

    int dest_cluster_id = t_stripe.blocks[recover[0]]->map2cluster;
    std::string dest_proxy_ip = m_cluster_table[dest_cluster_id].proxy_ip;
    int dest_proxy_port = m_cluster_table[dest_cluster_id].proxy_port;
    std::string dest_proxy_key = dest_proxy_ip + ":" + std::to_string(dest_proxy_port);
    int cross_rack_num = 0;
    for (size_t i = 0; i < clusters_with_blocks.size(); i++)
      if (clusters_with_blocks[i] != dest_cluster_id)
        cross_rack_num++;

    std::vector<std::string> replaced_ips(recover_num);
    std::vector<int> replaced_ports(recover_num);
    std::vector<std::string> failed_keys(recover_num);
    for (int f = 0; f < recover_num; f++)
    {
      int bid = recover[f];
      int node_id = t_stripe.blocks[bid]->map2node;
      replaced_ips[f] = m_node_table[node_id].node_ip;
      replaced_ports[f] = m_node_table[node_id].node_port;
      failed_keys[f] = t_stripe.blocks[bid]->block_key;
    }

    RecoveryBreakdownSamples samples;
    std::mutex samples_mutex;
    bool dest_success = false;
    std::string dest_error_message;
    std::vector<std::thread> threads;

    threads.push_back(std::thread([this, &t_stripe, dest_proxy_key, dest_cluster_id, recover_num, &all_failed,
                                   &recover, &decode_block_ids, &failed_keys, &replaced_ips, &replaced_ports,
                                   cross_rack_num, &decode_blocks_per_cluster, &clusters_with_blocks, &samples,
                                   &samples_mutex, &dest_success, &dest_error_message]() {
      grpc::ClientContext recovery_context;
      proxy_proto::RecoveryRequest recovery_request;
      proxy_proto::RecoveryReply recovery_reply;
      recovery_request.set_cross_rack_num(cross_rack_num);
      for (size_t i = 0; i < all_failed.size(); i++)
        recovery_request.add_all_failed_block_ids(all_failed[i]);
      for (int i = 0; i < recover_num; i++)
      {
        recovery_request.add_failed_block_ids(recover[i]);
        recovery_request.add_failed_block_keys(failed_keys[i]);
        recovery_request.add_replaced_node_ips(replaced_ips[i]);
        recovery_request.add_replaced_node_ports(replaced_ports[i]);
      }
      for (size_t i = 0; i < decode_block_ids.size(); i++)
        recovery_request.add_decode_block_ids(decode_block_ids[i]);
      size_t dest_idx = 0;
      for (; dest_idx < clusters_with_blocks.size(); dest_idx++)
        if (clusters_with_blocks[dest_idx] == dest_cluster_id)
          break;
      if (dest_idx < clusters_with_blocks.size())
        add_block_list_to_recovery_request(t_stripe, decode_blocks_per_cluster[dest_idx], &recovery_request);
      std::chrono::high_resolution_clock::time_point grpc_notify = std::chrono::high_resolution_clock::now();
      double grpc_notify_time =
          std::chrono::duration_cast<std::chrono::duration<double>>(grpc_notify.time_since_epoch()).count();
      grpc::Status st = m_proxy_ptrs[dest_proxy_key]->recoveryBreakdown(&recovery_context, recovery_request,
                                                                        &recovery_reply);
      std::lock_guard<std::mutex> lock(samples_mutex);
      dest_success = st.ok();
      if (!st.ok())
        dest_error_message = st.error_message();
      else
        samples.append_recovery_reply(recovery_reply, grpc_notify_time);
    }));

    for (size_t i = 0; i < clusters_with_blocks.size(); i++)
    {
      if (clusters_with_blocks[i] == dest_cluster_id)
        continue;
      std::string proxy_key = m_cluster_table[clusters_with_blocks[i]].proxy_ip + ":" +
                              std::to_string(m_cluster_table[clusters_with_blocks[i]].proxy_port);
      threads.push_back(std::thread([this, &t_stripe, proxy_key, dest_proxy_ip, dest_proxy_port, recover_num,
                                     &all_failed, &recover, &decode_block_ids, &decode_blocks_per_cluster, i,
                                     &samples, &samples_mutex]() {
        grpc::ClientContext degraded_context;
        proxy_proto::DegradedReadRequest degraded_request;
        proxy_proto::DegradedReadReply degraded_reply;
        degraded_request.set_clientip(dest_proxy_ip);
        degraded_request.set_clientport(dest_proxy_port + ECProject::PROXY_PORT_SHIFT);
        for (size_t j = 0; j < all_failed.size(); j++)
          degraded_request.add_all_failed_block_ids(all_failed[j]);
        for (int j = 0; j < recover_num; j++)
          degraded_request.add_failed_block_ids(recover[j]);
        for (size_t j = 0; j < decode_block_ids.size(); j++)
          degraded_request.add_decode_block_ids(decode_block_ids[j]);
        add_block_list_to_degraded_read_request(t_stripe, decode_blocks_per_cluster[i], &degraded_request);
        std::chrono::high_resolution_clock::time_point grpc_notify = std::chrono::high_resolution_clock::now();
        double grpc_notify_time =
            std::chrono::duration_cast<std::chrono::duration<double>>(grpc_notify.time_since_epoch()).count();
        grpc::Status st = m_proxy_ptrs[proxy_key]->degradedReadBreakdown(&degraded_context, degraded_request,
                                                                       &degraded_reply);
        if (st.ok())
        {
          std::lock_guard<std::mutex> lock(samples_mutex);
          samples.append_degraded_read_reply(degraded_reply, grpc_notify_time);
        }
      }));
    }

    threads[0].join();
    for (size_t i = 1; i < threads.size(); i++)
      threads[i].join();

    if (!dest_success)
    {
      std::cout << "[Coordinator] execute_global_recovery_breakdown: dest recoveryBreakdown failed: "
                << dest_error_message << std::endl;
      return false;
    }
    samples.fill_recovery_reply(breakdown_reply);
    return true;
  }

  // Maintenance-robust read: reconstruct every failed data block (all_failed) at the dest proxy
  // in a single in-memory global-decode round, then stream each reconstructed block straight to
  // the client (no disk write-back). Mirrors execute_global_recovery but redirects the dest output
  // to the client. The cross-group global batch (N-1/N-2) and the in-memory local fill (本地补齐)
  // are both produced by this one decode round at the dest proxy.
  bool CoordinatorImpl::execute_global_degraded_read_to_client(int stripe_id,
                                                               const std::vector<int> &all_failed,
                                                               std::string client_ip, int client_port)
  {
    const int all_failed_num = static_cast<int>(all_failed.size());
    if (all_failed_num == 0)
      return true;
    const std::vector<int> &recover = all_failed; // reconstruct all failed data blocks
    const int recover_num = all_failed_num;

    std::vector<int> decode_block_ids;
    int rows = 0, cols = 0;
    bool ifGetDecodePlanSuccess = ECProject::get_global_decode_plan(
        m_sys_config->k, m_sys_config->r, m_sys_config->z, m_sys_config->CodeType,
        all_failed, decode_block_ids, nullptr, nullptr, rows, cols, nullptr);
    if (!ifGetDecodePlanSuccess) {
      std::cout << "[Coordinator] maintenance read: get global decode plan failed!" << std::endl;
      return false;
    }

    Stripe &t_stripe = m_stripe_table[stripe_id];
    std::vector<int> clusters_with_blocks;
    std::vector<std::vector<int>> decode_blocks_per_cluster;
    for (size_t i = 0; i < decode_block_ids.size(); i++)
    {
      int cid = t_stripe.blocks[decode_block_ids[i]]->map2cluster;
      auto it = std::find(clusters_with_blocks.begin(), clusters_with_blocks.end(), cid);
      if (it == clusters_with_blocks.end())
      {
        clusters_with_blocks.push_back(cid);
        decode_blocks_per_cluster.push_back(std::vector<int>(1, decode_block_ids[i]));
      }
      else
      {
        size_t idx = std::distance(clusters_with_blocks.begin(), it);
        decode_blocks_per_cluster[idx].push_back(decode_block_ids[i]);
      }
    }
    // Dest cluster: the failed rack (where the failed data blocks live).
    int dest_cluster_id = t_stripe.blocks[recover[0]]->map2cluster;
    std::string dest_proxy_ip = m_cluster_table[dest_cluster_id].proxy_ip;
    int dest_proxy_port = m_cluster_table[dest_cluster_id].proxy_port;
    std::string dest_proxy_key = dest_proxy_ip + ":" + std::to_string(dest_proxy_port);
    int cross_rack_num = 0;
    for (size_t i = 0; i < clusters_with_blocks.size(); i++)
      if (clusters_with_blocks[i] != dest_cluster_id)
        cross_rack_num++;

    std::vector<std::string> failed_keys(recover_num);
    for (int f = 0; f < recover_num; f++)
      failed_keys[f] = t_stripe.blocks[recover[f]]->block_key;

    std::vector<std::thread> threads;
    grpc::Status dest_status;
    std::mutex dest_status_mutex;
    threads.push_back(std::thread([this, &t_stripe, dest_proxy_key, dest_cluster_id, stripe_id, recover_num, &all_failed, &recover, &decode_block_ids, &failed_keys, client_ip, client_port, cross_rack_num, &decode_blocks_per_cluster, &clusters_with_blocks, &dest_status, &dest_status_mutex]() {
      grpc::ClientContext recovery_context;
      proxy_proto::RecoveryRequest recovery_request;
      proxy_proto::RecoveryReply recovery_reply;
      recovery_request.set_cross_rack_num(cross_rack_num);
      // Stream every reconstructed block straight to the client (no disk write-back).
      recovery_request.set_send_to_client(true);
      recovery_request.set_replaced_node_ip(client_ip);
      recovery_request.set_replaced_node_port(client_port);
      for (size_t i = 0; i < all_failed.size(); i++)
        recovery_request.add_all_failed_block_ids(all_failed[i]);
      for (int i = 0; i < recover_num; i++)
      {
        recovery_request.add_failed_block_ids(recover[i]);
        recovery_request.add_failed_block_keys(failed_keys[i]);
        recovery_request.add_replaced_node_ips(client_ip);
        recovery_request.add_replaced_node_ports(client_port);
      }
      for (size_t i = 0; i < decode_block_ids.size(); i++)
        recovery_request.add_decode_block_ids(decode_block_ids[i]);
      size_t dest_idx = 0;
      for (; dest_idx < clusters_with_blocks.size(); dest_idx++)
        if (clusters_with_blocks[dest_idx] == dest_cluster_id)
          break;
      if (dest_idx < clusters_with_blocks.size())
        add_block_list_to_recovery_request(t_stripe, decode_blocks_per_cluster[dest_idx], &recovery_request);
      grpc::Status st = m_proxy_ptrs[dest_proxy_key]->recovery(&recovery_context, recovery_request, &recovery_reply);
      std::lock_guard<std::mutex> lock(dest_status_mutex);
      dest_status = st;
    }));

    for (size_t i = 0; i < clusters_with_blocks.size(); i++)
    {
      if (clusters_with_blocks[i] == dest_cluster_id)
        continue;
      std::string proxy_key = m_cluster_table[clusters_with_blocks[i]].proxy_ip + ":" + std::to_string(m_cluster_table[clusters_with_blocks[i]].proxy_port);
      threads.push_back(std::thread([this, &t_stripe, proxy_key, dest_proxy_ip, dest_proxy_port, stripe_id, recover_num, &all_failed, &recover, &decode_block_ids, &decode_blocks_per_cluster, i]() {
        grpc::ClientContext degraded_context;
        proxy_proto::DegradedReadRequest degraded_request;
        proxy_proto::DegradedReadReply degraded_reply;
        degraded_request.set_clientip(dest_proxy_ip);
        degraded_request.set_clientport(dest_proxy_port + ECProject::PROXY_PORT_SHIFT);
        for (size_t j = 0; j < all_failed.size(); j++)
          degraded_request.add_all_failed_block_ids(all_failed[j]);
        for (int j = 0; j < recover_num; j++)
          degraded_request.add_failed_block_ids(recover[j]);
        for (size_t j = 0; j < decode_block_ids.size(); j++)
          degraded_request.add_decode_block_ids(decode_block_ids[j]);
        add_block_list_to_degraded_read_request(t_stripe, decode_blocks_per_cluster[i], &degraded_request);
        grpc::Status st = m_proxy_ptrs[proxy_key]->degradedRead(&degraded_context, degraded_request, &degraded_reply);
        if (!st.ok())
          std::cout << "[Coordinator] maintenance read degradedRead from proxy " << proxy_key << " failed: " << st.error_message() << std::endl;
      }));
    }

    for (size_t i = 1; i < threads.size(); i++)
      threads[i].detach();
    threads[0].join();

    {
      std::lock_guard<std::mutex> lock(dest_status_mutex);
      if (!dest_status.ok()) {
        std::cout << "[Coordinator] maintenance read global decode on dest failed: "
                  << dest_status.error_message() << std::endl;
        return false;
      }
    }
    std::cout << "[Coordinator] maintenance read global decode success for stripe " << stripe_id
              << " reconstructed " << recover_num << " data block(s) to client" << std::endl;
    return true;
  }

  // Decide whether the leftover block(s) can be locally filled in phase 2. Requires that all
  // leftover blocks share one local group whose local parity block(s) survive (are NOT on the
  // failed rack). Fills local_parity_ids and surviving_siblings (surviving data members of that
  // local group, on other racks).
  bool CoordinatorImpl::plan_maintenance_local_fill(int stripe_id,
                                                    const std::vector<int> &failed_data,
                                                    const std::vector<int> &leftover,
                                                    std::vector<int> &local_parity_ids,
                                                    std::vector<int> &surviving_siblings,
                                                    std::string &note)
  {
    local_parity_ids.clear();
    surviving_siblings.clear();
    if (leftover.empty()) {
      note = "no leftover block to local-fill";
      return false;
    }
    const int k = m_sys_config->k;
    const int r = m_sys_config->r;
    const int z = m_sys_config->z;
    const std::string &code = m_sys_config->CodeType;
    Stripe &t_stripe = m_stripe_table[stripe_id];

    // All leftover blocks must belong to a single local group.
    int lg = -1;
    try {
      lg = ECProject::get_block_id_to_local_group_id(code, k, r, z, leftover[0]);
      for (int bid : leftover)
        if (ECProject::get_block_id_to_local_group_id(code, k, r, z, bid) != lg) {
          note = "leftover blocks span multiple local groups";
          return false;
        }
    } catch (const std::exception &e) {
      note = std::string("local group lookup failed: ") + e.what();
      return false;
    }

    // All local parity block id(s) of this local group.
    std::vector<int> all_local_parity_ids;
    if (code == "LotusLRC") {
      all_local_parity_ids.push_back(k + r + 2 * lg);
      all_local_parity_ids.push_back(k + r + 2 * lg + 1);
    } else {
      all_local_parity_ids.push_back(k + r + lg);
    }

    const int dest_cluster_id = t_stripe.blocks[leftover[0]]->map2cluster;
    // N-1/N-2 split (as in single-rack repair): of the group's failed blocks, x stay as the
    // leftover (x = local-parity count). Those x slots first absorb the group's failed local
    // parities (which the read never solves), so only the surviving local parities are available to
    // local-fill the DATA leftover. Hence keep only the SURVIVING local parities here, and the
    // leftover is feasible iff #surviving local parities >= #leftover (data) blocks. A co-located
    // local parity on the failed rack just shrinks the data leftover instead of forcing a fallback.
    for (int pid : all_local_parity_ids) {
      if (pid < 0 || pid >= static_cast<int>(t_stripe.blocks.size())) {
        note = "local parity id out of range";
        return false;
      }
      if (t_stripe.blocks[pid]->map2cluster != dest_cluster_id)
        local_parity_ids.push_back(pid);
    }
    if (local_parity_ids.size() < leftover.size()) {
      note = "not enough surviving local parities for the leftover count (need " +
             std::to_string(leftover.size()) + ", have " + std::to_string(local_parity_ids.size()) + ")";
      return false;
    }

    // Surviving data members of the local group (other racks).
    std::unordered_set<int> failed_set(failed_data.begin(), failed_data.end());
    for (int bid = 0; bid < k; bid++) {
      if (failed_set.count(bid))
        continue;
      int bid_lg = -1;
      try { bid_lg = ECProject::get_block_id_to_local_group_id(code, k, r, z, bid); }
      catch (...) { continue; }
      if (bid_lg == lg)
        surviving_siblings.push_back(bid);
    }
    return true;
  }

  // Two-phase maintenance read: reconstruct the global batch (N-1/N-2) in dest memory and, in the
  // same round, in-memory local-fill the leftover block(s); stream all failed-rack data blocks to
  // the client. Phase 2 adds only the local parity read at the dest; the surviving siblings arrive
  // as tiny tag=1 partials produced by the same helpers that serve the global batch.
  bool CoordinatorImpl::execute_two_phase_degraded_read_to_client(int stripe_id,
                                                                  const std::vector<int> &failed_data,
                                                                  const std::vector<int> &global_batch,
                                                                  const std::vector<int> &leftover,
                                                                  std::string client_ip, int client_port)
  {
    if (global_batch.empty()) {
      // Pure local recovery (failed count <= local-parity count): no cross-group batch needed.
      // Reuse the all-global path for simplicity (rare; the leftover set is tiny).
      return execute_global_degraded_read_to_client(stripe_id, failed_data, client_ip, client_port);
    }

    std::vector<int> local_parity_ids;
    std::vector<int> surviving_siblings;
    std::string note;
    if (!plan_maintenance_local_fill(stripe_id, failed_data, leftover, local_parity_ids, surviving_siblings, note)) {
      std::cout << "[Coordinator] maintenance two-phase not feasible (" << note
                << "), falling back to all-global" << std::endl;
      return execute_global_degraded_read_to_client(stripe_id, failed_data, client_ip, client_port);
    }

    const std::vector<int> &recover = global_batch;
    const int recover_num = static_cast<int>(recover.size());

    std::vector<int> decode_block_ids;
    int rows = 0, cols = 0;
    bool ok = ECProject::get_global_decode_plan(
        m_sys_config->k, m_sys_config->r, m_sys_config->z, m_sys_config->CodeType,
        failed_data, decode_block_ids, nullptr, nullptr, rows, cols, &recover);
    if (!ok) {
      std::cout << "[Coordinator] maintenance two-phase: get global decode plan failed!" << std::endl;
      return false;
    }

    Stripe &t_stripe = m_stripe_table[stripe_id];
    std::vector<int> clusters_with_blocks;
    std::vector<std::vector<int>> decode_blocks_per_cluster;
    for (size_t i = 0; i < decode_block_ids.size(); i++)
    {
      int cid = t_stripe.blocks[decode_block_ids[i]]->map2cluster;
      auto it = std::find(clusters_with_blocks.begin(), clusters_with_blocks.end(), cid);
      if (it == clusters_with_blocks.end())
      {
        clusters_with_blocks.push_back(cid);
        decode_blocks_per_cluster.push_back(std::vector<int>(1, decode_block_ids[i]));
      }
      else
      {
        size_t idx = std::distance(clusters_with_blocks.begin(), it);
        decode_blocks_per_cluster[idx].push_back(decode_block_ids[i]);
      }
    }

    int dest_cluster_id = t_stripe.blocks[recover[0]]->map2cluster;
    std::string dest_proxy_ip = m_cluster_table[dest_cluster_id].proxy_ip;
    int dest_proxy_port = m_cluster_table[dest_cluster_id].proxy_port;
    std::string dest_proxy_key = dest_proxy_ip + ":" + std::to_string(dest_proxy_port);

    const int k = m_sys_config->k;
    const int r = m_sys_config->r;
    const int z = m_sys_config->z;
    const std::string &code = m_sys_config->CodeType;
    const int nrows = k + r + z;
    const int lg = ECProject::get_block_id_to_local_group_id(code, k, r, z, leftover[0]);

    // ---- Strict GF local-fill source partition over the leftover's local group ----
    // Candidate sources = all members of the local group except the leftover block(s). Each is
    // owned by exactly one party so it contributes exactly once to the GF solve:
    //   - reconstructed_lg_members: failed DATA members reconstructed by the global batch (res_buf).
    //   - helper-owned: surviving members already read by a helper for the global batch
    //     (carried in decode_block_ids) -> contributed as weighted tag=1 partials.
    //   - dest_read_source_ids: surviving members NOT read by any helper (e.g. the local parity
    //     block(s)) -> read directly by the dest.
    // Members on the failed rack that are neither leftover nor reconstructed are unavailable.
    std::unordered_set<int> decode_set(decode_block_ids.begin(), decode_block_ids.end());
    std::unordered_set<int> recon_set(global_batch.begin(), global_batch.end());
    std::unordered_set<int> leftover_set(leftover.begin(), leftover.end());
    std::vector<int> reconstructed_lg_members;
    std::vector<int> source_block_ids;
    std::vector<int> dest_read_source_ids;
    for (int b = 0; b < nrows; b++)
    {
      if (ECProject::get_block_id_to_local_group_id(code, k, r, z, b) != lg)
        continue;
      if (leftover_set.count(b))
        continue;
      if (recon_set.count(b))
      {
        reconstructed_lg_members.push_back(b);
        source_block_ids.push_back(b);
        continue;
      }
      if (t_stripe.blocks[b]->map2cluster == dest_cluster_id)
        continue; // on the failed rack and not reconstructed -> unavailable
      source_block_ids.push_back(b);
      if (!decode_set.count(b))
        dest_read_source_ids.push_back(b); // not read by a helper -> dest reads it directly
    }
    std::sort(source_block_ids.begin(), source_block_ids.end());

    // Feasibility: the available sources must span the leftover block(s); else fall back.
    {
      std::vector<unsigned char> coeffs_check;
      if (!ECProject::get_local_fill_plan(k, r, z, code, leftover, source_block_ids, coeffs_check))
      {
        std::cout << "[Coordinator] maintenance local fill not GF-solvable from available sources"
                  << " (stripe " << stripe_id << ", lg " << lg << "), falling back to all-global"
                  << std::endl;
        return execute_global_degraded_read_to_client(stripe_id, failed_data, client_ip, client_port);
      }
    }

    std::unordered_set<int> source_set(source_block_ids.begin(), source_block_ids.end());
    int cross_rack_num = 0;
    int local_fill_sender_num = 0;
    for (size_t i = 0; i < clusters_with_blocks.size(); i++)
    {
      if (clusters_with_blocks[i] == dest_cluster_id)
        continue;
      cross_rack_num++;
      for (int bid : decode_blocks_per_cluster[i])
        if (source_set.count(bid)) { local_fill_sender_num++; break; }
    }

    std::vector<std::string> failed_keys(recover_num);
    for (int f = 0; f < recover_num; f++)
      failed_keys[f] = t_stripe.blocks[recover[f]]->block_key;

    std::vector<std::thread> threads;
    grpc::Status dest_status;
    std::mutex dest_status_mutex;
    threads.push_back(std::thread([this, &t_stripe, dest_proxy_key, dest_cluster_id, recover_num, &failed_data, &recover, &decode_block_ids, &failed_keys, client_ip, client_port, cross_rack_num, local_fill_sender_num, &decode_blocks_per_cluster, &clusters_with_blocks, &leftover, &dest_read_source_ids, &source_block_ids, &reconstructed_lg_members, &dest_status, &dest_status_mutex]() {
      grpc::ClientContext recovery_context;
      proxy_proto::RecoveryRequest recovery_request;
      proxy_proto::RecoveryReply recovery_reply;
      recovery_request.set_cross_rack_num(cross_rack_num);
      recovery_request.set_send_to_client(true);
      recovery_request.set_maintenance_header(true);
      recovery_request.set_local_fill_sender_num(local_fill_sender_num);
      recovery_request.set_replaced_node_ip(client_ip);
      recovery_request.set_replaced_node_port(client_port);
      for (int bid : failed_data)
        recovery_request.add_all_failed_block_ids(bid);
      for (int i = 0; i < recover_num; i++)
      {
        recovery_request.add_failed_block_ids(recover[i]);
        recovery_request.add_failed_block_keys(failed_keys[i]);
      }
      for (int bid : decode_block_ids)
        recovery_request.add_decode_block_ids(bid);
      for (int bid : leftover)
        recovery_request.add_leftover_block_ids(bid);
      for (int bid : reconstructed_lg_members)
        recovery_request.add_reconstructed_lg_member_ids(bid);
      for (int bid : source_block_ids)
        recovery_request.add_local_fill_source_ids(bid);
      // Dest-directly-read sources (the surviving local parity block(s) plus any group member not
      // read by a helper). Carried in the local_parity_* fields; the dest reads each and applies
      // its GF coefficient from the local-fill plan.
      for (int sid : dest_read_source_ids)
      {
        Block *pb = t_stripe.blocks[sid];
        recovery_request.add_local_parity_block_ids(sid);
        recovery_request.add_local_parity_datanodeip(m_node_table[pb->map2node].node_ip);
        recovery_request.add_local_parity_datanodeport(m_node_table[pb->map2node].node_port);
        recovery_request.add_local_parity_blockkeys(pb->block_key);
      }
      size_t dest_idx = 0;
      for (; dest_idx < clusters_with_blocks.size(); dest_idx++)
        if (clusters_with_blocks[dest_idx] == dest_cluster_id)
          break;
      if (dest_idx < clusters_with_blocks.size())
        add_block_list_to_recovery_request(t_stripe, decode_blocks_per_cluster[dest_idx], &recovery_request);
      grpc::Status st = m_proxy_ptrs[dest_proxy_key]->recovery(&recovery_context, recovery_request, &recovery_reply);
      std::lock_guard<std::mutex> lock(dest_status_mutex);
      dest_status = st;
    }));

    for (size_t i = 0; i < clusters_with_blocks.size(); i++)
    {
      if (clusters_with_blocks[i] == dest_cluster_id)
        continue;
      std::string proxy_key = m_cluster_table[clusters_with_blocks[i]].proxy_ip + ":" + std::to_string(m_cluster_table[clusters_with_blocks[i]].proxy_port);
      threads.push_back(std::thread([this, &t_stripe, proxy_key, dest_proxy_ip, dest_proxy_port, recover_num, &failed_data, &recover, &decode_block_ids, &decode_blocks_per_cluster, &leftover, &source_block_ids, i]() {
        grpc::ClientContext degraded_context;
        proxy_proto::DegradedReadRequest degraded_request;
        proxy_proto::DegradedReadReply degraded_reply;
        degraded_request.set_clientip(dest_proxy_ip);
        degraded_request.set_clientport(dest_proxy_port + ECProject::PROXY_PORT_SHIFT);
        degraded_request.set_maintenance_header(true);
        for (int bid : failed_data)
          degraded_request.add_all_failed_block_ids(bid);
        for (int j = 0; j < recover_num; j++)
          degraded_request.add_failed_block_ids(recover[j]);
        for (int bid : decode_block_ids)
          degraded_request.add_decode_block_ids(bid);
        // Strict GF local fill: every helper recomputes the same coefficient matrix from
        // (leftover_block_ids, local_fill_source_ids) and weights the sources it holds.
        for (int bid : leftover)
          degraded_request.add_leftover_block_ids(bid);
        for (int bid : source_block_ids)
          degraded_request.add_local_fill_source_ids(bid);
        add_block_list_to_degraded_read_request(t_stripe, decode_blocks_per_cluster[i], &degraded_request);
        grpc::Status st = m_proxy_ptrs[proxy_key]->degradedRead(&degraded_context, degraded_request, &degraded_reply);
        if (!st.ok())
          std::cout << "[Coordinator] maintenance two-phase degradedRead from proxy " << proxy_key << " failed: " << st.error_message() << std::endl;
      }));
    }

    for (size_t i = 1; i < threads.size(); i++)
      threads[i].detach();
    threads[0].join();

    {
      std::lock_guard<std::mutex> lock(dest_status_mutex);
      if (!dest_status.ok()) {
        std::cout << "[Coordinator] maintenance two-phase decode on dest failed: "
                  << dest_status.error_message() << std::endl;
        return false;
      }
    }
    std::cout << "[Coordinator] maintenance two-phase success for stripe " << stripe_id
              << ": global batch " << recover_num << " block(s), in-memory local fill "
              << leftover.size() << " block(s) (cross_rack=" << cross_rack_num
              << ", local_fill_senders=" << local_fill_sender_num << ")" << std::endl;
    return true;
  }

  void CoordinatorImpl::maintenance_read_driver(int stripe_id,
                                                std::vector<int> failed_data_block_ids,
                                                std::vector<int> global_batch_block_ids,
                                                bool fell_back,
                                                std::string client_ip, int client_port)
  {
    Stripe &t_stripe = m_stripe_table[stripe_id];
    const int k = m_sys_config->k;
    std::unordered_set<int> failed_set(failed_data_block_ids.begin(), failed_data_block_ids.end());

    // 1) Read every surviving data block directly (skip the failed rack's data blocks),
    //    grouped by placement group, streamed to the client with their block-id prefix.
    std::vector<int> data_block_num_per_group =
        ECProject::get_data_block_num_per_group(k, m_sys_config->r, m_sys_config->z, m_sys_config->CodeType);
    const int num_data_groups = static_cast<int>(data_block_num_per_group.size());
    std::vector<std::thread> survivor_threads;
    for (int g = 0; g < num_data_groups; g++)
    {
      if (g >= static_cast<int>(t_stripe.group_to_blocks.size()))
        break;
      std::vector<int> grp_survivors;
      for (int bid : t_stripe.group_to_blocks[g])
        if (bid < k && !failed_set.count(bid))
          grp_survivors.push_back(bid);
      if (grp_survivors.empty())
        continue;
      int cluster_id = t_stripe.blocks[t_stripe.group_to_blocks[g][0]]->map2cluster;
      std::string proxy_ip = m_cluster_table[cluster_id].proxy_ip;
      int proxy_port = m_cluster_table[cluster_id].proxy_port;
      survivor_threads.push_back(std::thread(&CoordinatorImpl::getStripeFromProxy, this,
                                             client_ip, client_port, proxy_ip, proxy_port,
                                             stripe_id, g, grp_survivors));
    }
    for (auto &th : survivor_threads)
      th.detach();

    // 2) Reconstruct the failed rack's data blocks in memory and stream them to the client.
    //    (No disk write-back.) Phase 1 reconstructs the cross-group global batch (N-1/N-2) in the
    //    dest proxy's memory; phase 2 in-memory local-fills the leftover block(s), reading only the
    //    local parity anew and reusing the surviving siblings' partials from the same round.
    std::vector<int> leftover;
    {
      std::unordered_set<int> batch_set(global_batch_block_ids.begin(), global_batch_block_ids.end());
      for (int bid : failed_data_block_ids)
        if (!batch_set.count(bid))
          leftover.push_back(bid);
    }
    std::cout << "[Coordinator] maintenance read stripe " << stripe_id << ": "
              << failed_data_block_ids.size() << " failed data block(s); global batch "
              << global_batch_block_ids.size() << ", local fill " << leftover.size()
              << (fell_back ? " [FELL BACK to all-global]" : " [two-phase]") << std::endl;

    bool success;
    if (fell_back)
      success = execute_global_degraded_read_to_client(stripe_id, failed_data_block_ids, client_ip, client_port);
    else
      success = execute_two_phase_degraded_read_to_client(stripe_id, failed_data_block_ids,
                                                          global_batch_block_ids, leftover,
                                                          client_ip, client_port);
    if (!success)
      std::cout << "[Coordinator] maintenance read stripe " << stripe_id << " reconstruction failed!"
                << std::endl;
  }

  grpc::Status CoordinatorImpl::maintenanceReadStripe(
      grpc::ServerContext *context,
      const coordinator_proto::MaintenanceReadRequest *request,
      coordinator_proto::RecoveryReply *replyClient)
  {
    const int stripe_id = request->stripe_id();
    std::string client_ip = request->clientip();
    int client_port = request->clientport();
    std::vector<int> failed_data;
    failed_data.reserve(static_cast<size_t>(request->failed_data_block_ids_size()));
    for (int i = 0; i < request->failed_data_block_ids_size(); i++)
      failed_data.push_back(request->failed_data_block_ids(i));
    std::vector<int> global_batch;
    global_batch.reserve(static_cast<size_t>(request->global_batch_block_ids_size()));
    for (int i = 0; i < request->global_batch_block_ids_size(); i++)
      global_batch.push_back(request->global_batch_block_ids(i));

    // Decide synchronously whether the two-phase local fill is feasible, so the client can report
    // (in the RPC reply) whether the measured run used the two-phase scheme or fell back.
    std::vector<int> leftover;
    {
      std::unordered_set<int> batch_set(global_batch.begin(), global_batch.end());
      for (int bid : failed_data)
        if (!batch_set.count(bid))
          leftover.push_back(bid);
    }
    bool fell_back = false;
    std::string note;
    {
      std::vector<int> local_parity_ids, surviving_siblings;
      if (global_batch.empty()) {
        fell_back = true;
        note = "no global batch (pure local recovery); using all-global path";
      } else if (!plan_maintenance_local_fill(stripe_id, failed_data, leftover, local_parity_ids,
                                              surviving_siblings, note)) {
        fell_back = true;
      }
    }
    replyClient->set_maintenance_fell_back(fell_back);
    replyClient->set_maintenance_note(note);

    // Dispatch the work and return immediately so the client can start accepting the streamed
    // blocks (the proxies connect back to the client and would otherwise block on large writes).
    std::thread driver(&CoordinatorImpl::maintenance_read_driver, this, stripe_id,
                       failed_data, global_batch, fell_back, client_ip, client_port);
    driver.detach();
    return grpc::Status::OK;
  }

  grpc::Status CoordinatorImpl::globalRecovery(
      grpc::ServerContext *context,
      const coordinator_proto::StripeIdAndBlockIDsFromClient *request,
      coordinator_proto::RecoveryReply *replyClient)
  {
    const int stripe_id = request->stripe_id();
    const int all_failed_num = request->block_ids_size();
    std::vector<int> all_failed;
    all_failed.reserve(static_cast<size_t>(all_failed_num));
    for (int i = 0; i < all_failed_num; i++)
      all_failed.push_back(request->block_ids(i));

    std::unordered_set<int> all_failed_set(all_failed.begin(), all_failed.end());
    std::vector<int> recover;
    if (request->recovery_block_ids_size() > 0) {
      recover.reserve(static_cast<size_t>(request->recovery_block_ids_size()));
      for (int i = 0; i < request->recovery_block_ids_size(); i++) {
        const int bid = request->recovery_block_ids(i);
        if (!all_failed_set.count(bid)) {
          std::cout << "[Coordinator] globalRecovery: recovery_block_id " << bid
                    << " not in block_ids" << std::endl;
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                              "recovery_block_ids must be subset of block_ids");
        }
        recover.push_back(bid);
      }
    }

    if (!execute_global_recovery(stripe_id, all_failed, recover)) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "globalRecovery failed");
    }
    return grpc::Status::OK;
  }

  grpc::Status CoordinatorImpl::globalRecoveryBreakdown(
      grpc::ServerContext *context,
      const coordinator_proto::StripeIdAndBlockIDsFromClient *request,
      coordinator_proto::RecoveryReply *replyClient)
  {
    std::chrono::time_point<std::chrono::high_resolution_clock> start = std::chrono::high_resolution_clock::now();
    replyClient->set_grpc_start_time(
        std::chrono::duration_cast<std::chrono::duration<double>>(start.time_since_epoch()).count());
    const int stripe_id = request->stripe_id();
    std::vector<int> all_failed;
    all_failed.reserve(static_cast<size_t>(request->block_ids_size()));
    for (int i = 0; i < request->block_ids_size(); i++)
      all_failed.push_back(request->block_ids(i));

    std::unordered_set<int> all_failed_set(all_failed.begin(), all_failed.end());
    std::vector<int> recover;
    if (request->recovery_block_ids_size() > 0) {
      recover.reserve(static_cast<size_t>(request->recovery_block_ids_size()));
      for (int i = 0; i < request->recovery_block_ids_size(); i++) {
        const int bid = request->recovery_block_ids(i);
        if (!all_failed_set.count(bid)) {
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                              "recovery_block_ids must be subset of block_ids");
        }
        recover.push_back(bid);
      }
    }

    if (!execute_global_recovery_breakdown(stripe_id, all_failed, recover, replyClient))
      return grpc::Status(grpc::StatusCode::INTERNAL, "globalRecoveryBreakdown failed");
    return grpc::Status::OK;
  }

  grpc::Status CoordinatorImpl::delByKey(
      grpc::ServerContext *context,
      const coordinator_proto::KeyFromClient *del_key,
      coordinator_proto::RepIfDeling *delReplyClient)
  {
    try
    {
      std::string key = del_key->key();
      ObjectInfo object_info;
      m_mutex.lock();
      object_info = m_object_commit_table.at(key);
      m_object_updating_table[key] = m_object_commit_table[key];
      m_mutex.unlock();

      grpc::ClientContext context;
      proxy_proto::NodeAndBlock node_block;
      grpc::Status status;
      proxy_proto::DelReply del_reply;
      Stripe &t_stripe = m_stripe_table[object_info.map2stripe];
      std::unordered_set<int> t_cluster_set;
      for (int i = 0; i < int(t_stripe.blocks.size()); i++)
      {
        if (t_stripe.blocks[i]->map2key == key)
        {
          node_block.add_datanodeip(m_node_table[t_stripe.blocks[i]->map2node].node_ip);
          node_block.add_datanodeport(m_node_table[t_stripe.blocks[i]->map2node].node_port);
          node_block.add_blockkeys(t_stripe.blocks[i]->block_key);
          t_cluster_set.insert(t_stripe.blocks[i]->map2cluster);
        }
      }
      node_block.set_stripe_id(-1); // as a flag to distinguish delete key or stripe
      node_block.set_key(key);
      // randomly select a cluster
      int idx = rand_num(int(t_cluster_set.size()));
      int r_cluster_id = *(std::next(t_cluster_set.begin(), idx));
      std::string chosen_proxy = m_cluster_table[r_cluster_id].proxy_ip + ":" + std::to_string(m_cluster_table[r_cluster_id].proxy_port);
      status = m_proxy_ptrs[chosen_proxy]->deleteBlock(&context, node_block, &del_reply);
      delReplyClient->set_ifdeling(true);
      if (status.ok())
      {
        std::cout << "[DEL] deleting value of " << key << std::endl;
      }
    }
    catch (const std::exception &e)
    {
      std::cout << "deleteByKey exception" << std::endl;
      std::cout << e.what() << std::endl;
    }
    return grpc::Status::OK;
  }

  grpc::Status CoordinatorImpl::delByStripe(
      grpc::ServerContext *context,
      const coordinator_proto::StripeIdFromClient *stripeid,
      coordinator_proto::RepIfDeling *delReplyClient)
  {
    try
    {
      int t_stripe_id = stripeid->stripe_id();
      m_mutex.lock();
      m_stripe_deleting_table.push_back(t_stripe_id);
      m_mutex.unlock();

      grpc::ClientContext context;
      proxy_proto::NodeAndBlock node_block;
      grpc::Status status;
      proxy_proto::DelReply del_reply;
      Stripe &t_stripe = m_stripe_table[t_stripe_id];
      std::unordered_set<int> t_cluster_set;
      for (int i = 0; i < int(t_stripe.blocks.size()); i++)
      {
        if (t_stripe.blocks[i]->map2stripe == t_stripe_id)
        {
          node_block.add_datanodeip(m_node_table[t_stripe.blocks[i]->map2node].node_ip);
          node_block.add_datanodeport(m_node_table[t_stripe.blocks[i]->map2node].node_port);
          node_block.add_blockkeys(t_stripe.blocks[i]->block_key);
          t_cluster_set.insert(t_stripe.blocks[i]->map2cluster);
        }
      }
      node_block.set_stripe_id(t_stripe_id);
      node_block.set_key("");
      // randomly select a cluster
      int idx = rand_num(int(t_cluster_set.size()));
      int r_cluster_id = *(std::next(t_cluster_set.begin(), idx));
      std::string chosen_proxy = m_cluster_table[r_cluster_id].proxy_ip + ":" + std::to_string(m_cluster_table[r_cluster_id].proxy_port);
      status = m_proxy_ptrs[chosen_proxy]->deleteBlock(&context, node_block, &del_reply);
      delReplyClient->set_ifdeling(true);
      if (status.ok())
      {
        std::cout << "[DEL] deleting value of Stripe " << t_stripe_id << std::endl;
      }
    }
    catch (const std::exception &e)
    {
      std::cout << "deleteByStripe exception" << std::endl;
      std::cout << e.what() << std::endl;
    }
    return grpc::Status::OK;
  }

  grpc::Status CoordinatorImpl::listStripes(
      grpc::ServerContext *context,
      const coordinator_proto::RequestToCoordinator *req,
      coordinator_proto::RepStripeIds *listReplyClient)
  {
    try
    {
      for (auto it = m_stripe_table.begin(); it != m_stripe_table.end(); it++)
      {
        listReplyClient->add_stripe_ids(it->first);
      }
    }
    catch (const std::exception &e)
    {
      std::cerr << e.what() << '\n';
    }

    return grpc::Status::OK;
  }

  grpc::Status CoordinatorImpl::checkalive(
      grpc::ServerContext *context,
      const coordinator_proto::RequestToCoordinator *helloRequestToCoordinator,
      coordinator_proto::ReplyFromCoordinator *helloReplyFromCoordinator)
  {

    std::cout << "[Coordinator Check] alive " << helloRequestToCoordinator->name() << std::endl;
    return grpc::Status::OK;
  }
  grpc::Status CoordinatorImpl::reportCommitAbort(
      grpc::ServerContext *context,
      const coordinator_proto::CommitAbortKey *commit_abortkey,
      coordinator_proto::ReplyFromCoordinator *helloReplyFromCoordinator)
  {
    std::string key = commit_abortkey->key();
    ECProject::OpperateType opp = (ECProject::OpperateType)commit_abortkey->opp();
    int stripe_id = commit_abortkey->stripe_id();
    std::unique_lock<std::mutex> lck(m_mutex);
    try
    {
      if (commit_abortkey->ifcommitmetadata())
      {
        if (opp == SET || opp == APPEND)
        {
          m_object_commit_table[key] = m_object_updating_table[key];
          cv.notify_all();
          m_object_updating_table.erase(key);
        }
        else if (opp == DEL) // delete the metadata
        {
          if (stripe_id < 0) // delete key
          {
            if (IF_DEBUG)
            {
              std::cout << "[DEL] Proxy report delete key finish!" << std::endl;
            }
            ObjectInfo object_info = m_object_commit_table.at(key);
            stripe_id = object_info.map2stripe;
            m_object_commit_table.erase(key); // update commit table
            cv.notify_all();
            m_object_updating_table.erase(key);
            Stripe &t_stripe = m_stripe_table[stripe_id];
            std::vector<Block *>::iterator it1;
            for (it1 = t_stripe.blocks.begin(); it1 != t_stripe.blocks.end();)
            {
              if ((*it1)->map2key == key)
              {
                it1 = t_stripe.blocks.erase(it1);
              }
              else
              {
                it1++;
              }
            }
            if (t_stripe.blocks.empty()) // update stripe table
            {
              m_stripe_table.erase(stripe_id);
            }
            std::map<int, Cluster>::iterator it2; // update cluster table
            for (it2 = m_cluster_table.begin(); it2 != m_cluster_table.end(); it2++)
            {
              Cluster &t_cluster = it2->second;
              for (it1 = t_cluster.blocks.begin(); it1 != t_cluster.blocks.end();)
              {
                if ((*it1)->map2key == key)
                {
                  update_stripe_info_in_node(false, (*it1)->map2node, (*it1)->map2stripe); // update node table
                  it1 = t_cluster.blocks.erase(it1);
                }
                else
                {
                  it1++;
                }
              }
            }
          } // delete stripe
          else
          {
            if (IF_DEBUG)
            {
              std::cout << "[DEL] Proxy report delete stripe finish!" << std::endl;
            }
            auto its = std::find(m_stripe_deleting_table.begin(), m_stripe_deleting_table.end(), stripe_id);
            if (its != m_stripe_deleting_table.end())
            {
              m_stripe_deleting_table.erase(its);
            }
            cv.notify_all();
            // update stripe table
            m_stripe_table.erase(stripe_id);
            std::unordered_set<std::string> object_keys_set;
            // update cluster table
            std::map<int, Cluster>::iterator it2;
            for (it2 = m_cluster_table.begin(); it2 != m_cluster_table.end(); it2++)
            {
              Cluster &t_cluster = it2->second;
              for (auto it1 = t_cluster.blocks.begin(); it1 != t_cluster.blocks.end();)
              {
                if ((*it1)->map2stripe == stripe_id)
                {
                  object_keys_set.insert((*it1)->map2key);
                  it1 = t_cluster.blocks.erase(it1);
                }
                else
                {
                  it1++;
                }
              }
            }
            // update node table
            for (auto it3 = m_node_table.begin(); it3 != m_node_table.end(); it3++)
            {
              Node &t_node = it3->second;
              auto it4 = t_node.stripes.find(stripe_id);
              if (it4 != t_node.stripes.end())
              {
                t_node.stripes.erase(stripe_id);
              }
            }
            // update commit table
            for (auto it5 = object_keys_set.begin(); it5 != object_keys_set.end(); it5++)
            {
              auto it6 = m_object_commit_table.find(*it5);
              if (it6 != m_object_commit_table.end())
              {
                m_object_commit_table.erase(it6);
              }
            }
            // merge group
          }
          // if (IF_DEBUG)
          // {
          //   std::cout << "[DEL] Data placement after delete:" << std::endl;
          //   for (int i = 0; i < m_num_of_Clusters; i++)
          //   {
          //     Cluster &t_cluster = m_cluster_table[i];
          //     if (int(t_cluster.blocks.size()) > 0)
          //     {
          //       std::cout << "Cluster " << i << ": ";
          //       for (auto it = t_cluster.blocks.begin(); it != t_cluster.blocks.end(); it++)
          //       {
          //         std::cout << "[" << (*it)->block_key << ":S" << (*it)->map2stripe << "G" << (*it)->map2group << "N" << (*it)->map2node << "] ";
          //       }
          //       std::cout << std::endl;
          //     }
          //   }
          //   std::cout << std::endl;
          // }
        }
      }
      else
      {
        m_object_updating_table.erase(key);
      }
    }
    catch (std::exception &e)
    {
      std::cout << "reportCommitAbort exception" << std::endl;
      std::cout << e.what() << std::endl;
    }
    return grpc::Status::OK;
  }

  grpc::Status
  CoordinatorImpl::checkCommitAbort(grpc::ServerContext *context,
                                    const coordinator_proto::AskIfSuccess *key_opp,
                                    coordinator_proto::RepIfSuccess *reply)
  {
    std::unique_lock<std::mutex> lck(m_mutex);
    std::string key = key_opp->key();
    ECProject::OpperateType opp = (ECProject::OpperateType)key_opp->opp();
    int stripe_id = key_opp->stripe_id();
    if (opp == SET || opp == APPEND)
    {
      while (m_object_commit_table.find(key) == m_object_commit_table.end())
      {
        cv.wait(lck);
      }
    }
    else if (opp == DEL)
    {
      if (stripe_id < 0)
      {
        while (m_object_commit_table.find(key) != m_object_commit_table.end())
        {
          cv.wait(lck);
        }
      }
      else
      {
        auto it = std::find(m_stripe_deleting_table.begin(), m_stripe_deleting_table.end(), stripe_id);
        while (it != m_stripe_deleting_table.end())
        {
          cv.wait(lck);
          it = std::find(m_stripe_deleting_table.begin(), m_stripe_deleting_table.end(), stripe_id);
        }
      }
    }
    reply->set_ifcommit(true);
    return grpc::Status::OK;
  }

  // Check the connnection to all proxies of all clusters
  bool CoordinatorImpl::init_proxyinfo()
  {
    for (auto cur = m_cluster_table.begin(); cur != m_cluster_table.end(); cur++)
    {
      std::string proxy_ip_and_port = cur->second.proxy_ip + ":" + std::to_string(cur->second.proxy_port);
      auto _stub = proxy_proto::proxyService::NewStub(grpc::CreateChannel(proxy_ip_and_port, grpc::InsecureChannelCredentials()));
      proxy_proto::CheckaliveCMD Cmd;
      proxy_proto::RequestResult result;
      grpc::ClientContext clientContext;
      Cmd.set_name("coordinator");
      grpc::Status status;
      status = _stub->checkalive(&clientContext, Cmd, &result);
      if (status.ok())
      {
        std::cout << "[Proxy Check] ok from " << proxy_ip_and_port << std::endl;
      }
      else
      {
        std::cout << "[Proxy Check] failed to connect " << proxy_ip_and_port << std::endl;
      }
      m_proxy_ptrs.insert(std::make_pair(proxy_ip_and_port, std::move(_stub)));
    }
    return true;
  }
  bool CoordinatorImpl::init_clusterinfo(std::string m_clusterinfo_path)
  {
    std::cout << "Cluster_information_path:" << m_clusterinfo_path << std::endl;
    tinyxml2::XMLDocument xml;
    xml.LoadFile(m_clusterinfo_path.c_str());
    tinyxml2::XMLElement *root = xml.RootElement();
    int node_id = 0;
    m_num_of_Clusters = 0;
    for (tinyxml2::XMLElement *cluster = root->FirstChildElement(); cluster != nullptr; cluster = cluster->NextSiblingElement())
    {
      std::string cluster_id(cluster->Attribute("id"));
      std::string proxy(cluster->Attribute("proxy"));
      std::cout << "cluster_id: " << cluster_id << " , proxy: " << proxy << std::endl;
      Cluster t_cluster;
      m_cluster_table[std::stoi(cluster_id)] = t_cluster;
      m_cluster_table[std::stoi(cluster_id)].cluster_id = std::stoi(cluster_id);
      auto pos = proxy.find(':');
      m_cluster_table[std::stoi(cluster_id)].proxy_ip = proxy.substr(0, pos);
      m_cluster_table[std::stoi(cluster_id)].proxy_port = std::stoi(proxy.substr(pos + 1, proxy.size()));
      for (tinyxml2::XMLElement *node = cluster->FirstChildElement()->FirstChildElement(); node != nullptr; node = node->NextSiblingElement())
      {
        std::string node_uri(node->Attribute("uri"));
        std::cout << "____node: " << node_uri << std::endl;
        m_cluster_table[std::stoi(cluster_id)].nodes.push_back(node_id);
        m_node_table[node_id].node_id = node_id;
        auto pos = node_uri.find(':');
        m_node_table[node_id].node_ip = node_uri.substr(0, pos);
        m_node_table[node_id].node_port = std::stoi(node_uri.substr(pos + 1, node_uri.size()));
        m_node_table[node_id].cluster_id = std::stoi(cluster_id);
        node_id++;
      }
      m_num_of_Clusters++;
    }
    return true;
  }

  int CoordinatorImpl::randomly_select_a_cluster(int stripe_id)
  {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis_cluster(0, m_num_of_Clusters - 1);
    int r_cluster_id = dis_cluster(gen);
    while (m_cluster_table[r_cluster_id].stripes.find(stripe_id) != m_cluster_table[r_cluster_id].stripes.end())
    {
      r_cluster_id = dis_cluster(gen);
    }
    return r_cluster_id;
  }

  // randomly select a node in the selected cluster
  // with the constraint that the node has not been selected for the same stripe
  int CoordinatorImpl::randomly_select_a_node(int cluster_id, int stripe_id)
  {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis_node(0, m_cluster_table[cluster_id].nodes.size() - 1);
    int r_node_id = m_cluster_table[cluster_id].nodes[dis_node(gen)];
    while (m_node_table[r_node_id].stripes.find(stripe_id) != m_node_table[r_node_id].stripes.end())
    {
      r_node_id = m_cluster_table[cluster_id].nodes[dis_node(gen)];
    }
    return r_node_id;
  }

  void CoordinatorImpl::update_stripe_info_in_node(int t_node_id, int stripe_id, int index)
  {
    assert(m_node_table[t_node_id].stripes.find(stripe_id) == m_node_table[t_node_id].stripes.end() && "The node has been selected for the stripe");
    m_node_table[t_node_id].stripes[stripe_id] = index;
  }

  // maintain the block number of the stripe in the node
  // TODO: Still don't konw why the stripe_block_num is start from 1
  void
  CoordinatorImpl::update_stripe_info_in_node(bool add_or_sub, int t_node_id, int stripe_id)
  {
    int stripe_block_num = 1;
    if (m_node_table[t_node_id].stripes.find(stripe_id) != m_node_table[t_node_id].stripes.end())
    {
      stripe_block_num = m_node_table[t_node_id].stripes[stripe_id];
    }
    if (add_or_sub)
    {
      m_node_table[t_node_id].stripes[stripe_id] = stripe_block_num + 1;
    }
    else
    {
      if (stripe_block_num == 1)
      {
        m_node_table[t_node_id].stripes.erase(stripe_id);
      }
      else
      {
        m_node_table[t_node_id].stripes[stripe_id] = stripe_block_num - 1;
      }
    }
  }

  int CoordinatorImpl::generate_placement(int stripe_id, int block_size)
  {
    Stripe &stripe_info = m_stripe_table[stripe_id];
    int k = stripe_info.k;
    int l = stripe_info.l;
    int g_m = stripe_info.g_m;
    int b = m_encode_parameters.b_datapergroup;
    ECProject::EncodeType encode_type = m_encode_parameters.encodetype;
    ECProject::SingleStripePlacementType s_placement_type = m_encode_parameters.s_stripe_placementtype;
    ECProject::MultiStripesPlacementType m_placement_type = m_encode_parameters.m_stripe_placementtype;

    // generate stripe information
    int index = stripe_info.object_keys.size() - 1;
    std::string object_key = stripe_info.object_keys[index];
    Block *blocks_info = new Block[k + g_m + l];
    for (int i = 0; i < k + g_m + l; i++)
    {
      blocks_info[i].block_size = block_size;
      blocks_info[i].map2stripe = stripe_id;
      blocks_info[i].map2key = object_key;
      if (i < k)
      {
        std::string tmp = "_D";
        if (i < 10)
          tmp = "_D0";
        blocks_info[i].block_key = object_key + tmp + std::to_string(i);
        blocks_info[i].block_id = i;
        blocks_info[i].block_type = 'D';
        blocks_info[i].map2group = int(i / b);
        stripe_info.blocks.push_back(&blocks_info[i]);
      }
      else if (i >= k && i < k + g_m)
      {
        blocks_info[i].block_key = "Stripe" + std::to_string(stripe_id) + "_G" + std::to_string(i - k);
        blocks_info[i].block_id = i;
        blocks_info[i].block_type = 'G';
        blocks_info[i].map2group = l;
        stripe_info.blocks.push_back(&blocks_info[i]);
      }
      else
      {
        blocks_info[i].block_key = "Stripe" + std::to_string(stripe_id) + "_L" + std::to_string(i - k - g_m);
        blocks_info[i].block_id = i;
        blocks_info[i].block_type = 'L';
        blocks_info[i].map2group = i - k - g_m;
        stripe_info.blocks.push_back(&blocks_info[i]);
      }
    }

    if (encode_type == Azure_LRC || encode_type == Optimal_Cauchy_LRC)
    {
      if (s_placement_type == Optimal)
      {
        if (m_placement_type == Ran)
        {
          int idx = m_merge_groups.size() - 1;
          if (idx < 0 || int(m_merge_groups[idx].size()) == m_encode_parameters.x_stripepermergegroup)
          {
            std::vector<int> temp;
            temp.push_back(stripe_id);
            m_merge_groups.push_back(temp);
          }
          else
          {
            m_merge_groups[idx].push_back(stripe_id);
          }

          int g_cluster_id = -1;
          for (int i = 0; i < l; i++)
          {
            for (int j = i * b; j < (i + 1) * b; j += g_m + 1)
            {
              bool flag = false;
              if (j + g_m + 1 >= (i + 1) * b)
                flag = true;
              // randomly select a cluster
              int t_cluster_id = randomly_select_a_cluster(stripe_id);
              Cluster &t_cluster = m_cluster_table[t_cluster_id];
              // place every g+1 data blocks from each group to a single cluster
              for (int o = j; o < j + g_m + 1 && o < (i + 1) * b; o++)
              {
                // randomly select a node in the selected cluster
                int t_node_id = randomly_select_a_node(t_cluster_id, stripe_id);
                blocks_info[o].map2cluster = t_cluster_id;
                blocks_info[o].map2node = t_node_id;
                update_stripe_info_in_node(true, t_node_id, stripe_id);
                t_cluster.blocks.push_back(&blocks_info[o]);
                t_cluster.stripes.insert(stripe_id);
                stripe_info.place2clusters.insert(t_cluster_id);
              }
              // place local parity blocks
              if (flag)
              {
                if (j + g_m + 1 != (i + 1) * b) // b % (g + 1) != 0
                {
                  // randomly select a node in the selected cluster
                  int t_node_id = randomly_select_a_node(t_cluster_id, stripe_id);
                  blocks_info[k + g_m + i].map2cluster = t_cluster_id;
                  blocks_info[k + g_m + i].map2node = t_node_id;
                  update_stripe_info_in_node(true, t_node_id, stripe_id);
                  t_cluster.blocks.push_back(&blocks_info[k + g_m + i]);
                  t_cluster.stripes.insert(stripe_id);
                  stripe_info.place2clusters.insert(t_cluster_id);
                }
                else // place the local parity blocks together with global ones
                {
                  if (g_cluster_id == -1) // randomly select a new cluster
                  {
                    g_cluster_id = randomly_select_a_cluster(stripe_id);
                  }
                  Cluster &g_cluster = m_cluster_table[g_cluster_id];
                  int t_node_id = randomly_select_a_node(g_cluster_id, stripe_id);
                  blocks_info[k + g_m + i].map2cluster = g_cluster_id;
                  blocks_info[k + g_m + i].map2node = t_node_id;
                  update_stripe_info_in_node(true, t_node_id, stripe_id);
                  g_cluster.blocks.push_back(&blocks_info[k + g_m + i]);
                  g_cluster.stripes.insert(stripe_id);
                  stripe_info.place2clusters.insert(g_cluster_id);
                }
              }
            }
          }
          if (g_cluster_id == -1) // randomly select a new cluster
          {
            g_cluster_id = randomly_select_a_cluster(stripe_id);
          }
          Cluster &g_cluster = m_cluster_table[g_cluster_id];
          // place the global parity blocks to the selected cluster
          for (int i = 0; i < g_m; i++)
          {
            int t_node_id = randomly_select_a_node(g_cluster_id, stripe_id);
            blocks_info[k + i].map2cluster = g_cluster_id;
            blocks_info[k + i].map2node = t_node_id;
            update_stripe_info_in_node(true, t_node_id, stripe_id);
            g_cluster.blocks.push_back(&blocks_info[k + i]);
            g_cluster.stripes.insert(stripe_id);
            stripe_info.place2clusters.insert(g_cluster_id);
          }
        }
        else if (m_placement_type == DIS)
        {
          int required_cluster_num = ceil(b + 1, g_m + 1) * l + 1;
          int idx = m_merge_groups.size() - 1;
          if (b % (g_m + 1) == 0)
            required_cluster_num -= l;
          if (int(m_free_clusters.size()) < required_cluster_num || m_free_clusters.empty() || idx < 0 ||
              int(m_merge_groups[idx].size()) == m_encode_parameters.x_stripepermergegroup)
          {
            m_free_clusters.clear();
            m_free_clusters.shrink_to_fit();
            for (int i = 0; i < m_num_of_Clusters; i++)
            {
              m_free_clusters.push_back(i);
            }
            std::vector<int> temp;
            temp.push_back(stripe_id);
            m_merge_groups.push_back(temp);
          }
          else
          {
            m_merge_groups[idx].push_back(stripe_id);
          }

          int g_cluster_id = -1;
          for (int i = 0; i < l; i++)
          {
            for (int j = i * b; j < (i + 1) * b; j += g_m + 1)
            {
              bool flag = false;
              if (j + g_m + 1 >= (i + 1) * b)
                flag = true;
              // randomly select a cluster
              int t_cluster_id = m_free_clusters[rand_num(int(m_free_clusters.size()))];
              auto iter = std::find(m_free_clusters.begin(), m_free_clusters.end(), t_cluster_id);
              if (iter != m_free_clusters.end())
              {
                m_free_clusters.erase(iter);
              } // remove the selected cluster from the free list
              Cluster &t_cluster = m_cluster_table[t_cluster_id];
              // place every g+1 data blocks from each group to a single cluster
              for (int o = j; o < j + g_m + 1 && o < (i + 1) * b; o++)
              {
                // randomly select a node in the selected cluster
                int t_node_id = randomly_select_a_node(t_cluster_id, stripe_id);
                blocks_info[o].map2cluster = t_cluster_id;
                blocks_info[o].map2node = t_node_id;
                update_stripe_info_in_node(true, t_node_id, stripe_id);
                t_cluster.blocks.push_back(&blocks_info[o]);
                t_cluster.stripes.insert(stripe_id);
                stripe_info.place2clusters.insert(t_cluster_id);
              }
              // place local parity blocks
              if (flag)
              {
                if (j + g_m + 1 != (i + 1) * b) // b % (g + 1) != 0
                {
                  // randomly select a node in the selected cluster
                  int t_node_id = randomly_select_a_node(t_cluster_id, stripe_id);
                  blocks_info[k + g_m + i].map2cluster = t_cluster_id;
                  blocks_info[k + g_m + i].map2node = t_node_id;
                  update_stripe_info_in_node(true, t_node_id, stripe_id);
                  t_cluster.blocks.push_back(&blocks_info[k + g_m + i]);
                  t_cluster.stripes.insert(stripe_id);
                  stripe_info.place2clusters.insert(t_cluster_id);
                }
                else // place the local parity blocks together with global ones
                {
                  if (g_cluster_id == -1) // randomly select a new cluster
                  {
                    g_cluster_id = m_free_clusters[rand_num(int(m_free_clusters.size()))];
                    auto iter = std::find(m_free_clusters.begin(), m_free_clusters.end(), g_cluster_id);
                    if (iter != m_free_clusters.end())
                    {
                      m_free_clusters.erase(iter);
                    }
                  }
                  Cluster &g_cluster = m_cluster_table[g_cluster_id];
                  int t_node_id = randomly_select_a_node(g_cluster_id, stripe_id);
                  blocks_info[k + g_m + i].map2cluster = g_cluster_id;
                  blocks_info[k + g_m + i].map2node = t_node_id;
                  update_stripe_info_in_node(true, t_node_id, stripe_id);
                  g_cluster.blocks.push_back(&blocks_info[k + g_m + i]);
                  g_cluster.stripes.insert(stripe_id);
                  stripe_info.place2clusters.insert(g_cluster_id);
                }
              }
            }
          }
          if (g_cluster_id == -1) // randomly select a new cluster
          {
            g_cluster_id = m_free_clusters[rand_num(int(m_free_clusters.size()))];
            auto iter = std::find(m_free_clusters.begin(), m_free_clusters.end(), g_cluster_id);
            if (iter != m_free_clusters.end())
            {
              m_free_clusters.erase(iter);
            }
          }
          Cluster &g_cluster = m_cluster_table[g_cluster_id];
          // place the global parity blocks to the selected cluster
          for (int i = 0; i < g_m; i++)
          {
            int t_node_id = randomly_select_a_node(g_cluster_id, stripe_id);
            blocks_info[k + i].map2cluster = g_cluster_id;
            blocks_info[k + i].map2node = t_node_id;
            update_stripe_info_in_node(true, t_node_id, stripe_id);
            g_cluster.blocks.push_back(&blocks_info[k + i]);
            g_cluster.stripes.insert(stripe_id);
            stripe_info.place2clusters.insert(g_cluster_id);
          }
        }
        else if (m_placement_type == AGG)
        {
          int agg_clusters_num = ceil(b + 1, g_m + 1) * l + 1;
          if (b % (g_m + 1) == 0)
          {
            agg_clusters_num -= l;
          }
          int idx = m_merge_groups.size() - 1;
          if (idx < 0 || int(m_merge_groups[idx].size()) == m_encode_parameters.x_stripepermergegroup)
          {
            std::vector<int> temp;
            temp.push_back(stripe_id);
            m_merge_groups.push_back(temp);
            m_agg_start_cid = rand_num(m_num_of_Clusters - agg_clusters_num);
          }
          else
          {
            m_merge_groups[idx].push_back(stripe_id);
          }
          int t_cluster_id = m_agg_start_cid - 1;
          int g_cluster_id = -1;
          for (int i = 0; i < l; i++)
          {
            for (int j = i * b; j < (i + 1) * b; j += g_m + 1)
            {
              bool flag = false;
              if (j + g_m + 1 >= (i + 1) * b)
                flag = true;
              t_cluster_id += 1;
              Cluster &t_cluster = m_cluster_table[t_cluster_id];
              // place every g+1 data blocks from each group to a single cluster
              for (int o = j; o < j + g_m + 1 && o < (i + 1) * b; o++)
              {
                // randomly select a node in the selected cluster
                int t_node_id = randomly_select_a_node(t_cluster_id, stripe_id);
                blocks_info[o].map2cluster = t_cluster_id;
                blocks_info[o].map2node = t_node_id;
                update_stripe_info_in_node(true, t_node_id, stripe_id);
                t_cluster.blocks.push_back(&blocks_info[o]);
                t_cluster.stripes.insert(stripe_id);
                stripe_info.place2clusters.insert(t_cluster_id);
              }
              // place local parity blocks
              if (flag)
              {
                if (j + g_m + 1 != (i + 1) * b) // b % (g + 1) != 0
                {
                  // randomly select a node in the selected cluster
                  int t_node_id = randomly_select_a_node(t_cluster_id, stripe_id);
                  blocks_info[k + g_m + i].map2cluster = t_cluster_id;
                  blocks_info[k + g_m + i].map2node = t_node_id;
                  update_stripe_info_in_node(true, t_node_id, stripe_id);
                  t_cluster.blocks.push_back(&blocks_info[k + g_m + i]);
                  t_cluster.stripes.insert(stripe_id);
                  stripe_info.place2clusters.insert(t_cluster_id);
                }
                else // place the local parity blocks together with global ones
                {
                  if (g_cluster_id == -1)
                  {
                    g_cluster_id = t_cluster_id + 1;
                    t_cluster_id++;
                  }
                  Cluster &g_cluster = m_cluster_table[g_cluster_id];
                  int t_node_id = randomly_select_a_node(g_cluster_id, stripe_id);
                  blocks_info[k + g_m + i].map2cluster = g_cluster_id;
                  blocks_info[k + g_m + i].map2node = t_node_id;
                  update_stripe_info_in_node(true, t_node_id, stripe_id);
                  g_cluster.blocks.push_back(&blocks_info[k + g_m + i]);
                  g_cluster.stripes.insert(stripe_id);
                  stripe_info.place2clusters.insert(g_cluster_id);
                }
              }
            }
          }
          if (g_cluster_id == -1)
          {
            g_cluster_id = t_cluster_id + 1;
          }
          Cluster &g_cluster = m_cluster_table[g_cluster_id];
          // place the global parity blocks to the selected cluster
          for (int i = 0; i < g_m; i++)
          {
            int t_node_id = randomly_select_a_node(g_cluster_id, stripe_id);
            blocks_info[k + i].map2cluster = g_cluster_id;
            blocks_info[k + i].map2node = t_node_id;
            update_stripe_info_in_node(true, t_node_id, stripe_id);
            g_cluster.blocks.push_back(&blocks_info[k + i]);
            g_cluster.stripes.insert(stripe_id);
            stripe_info.place2clusters.insert(g_cluster_id);
          }
        }
        else if (m_placement_type == OPT)
        {
          int required_cluster_num = ceil(b + 1, g_m + 1) * l + 1;
          int agg_clusters_num = l + 1;
          if (b % (g_m + 1) == 0)
          {
            agg_clusters_num = 1;
            required_cluster_num -= l;
          }
          int idx = m_merge_groups.size() - 1;
          if (int(m_free_clusters.size()) < required_cluster_num - agg_clusters_num || m_free_clusters.empty() ||
              idx < 0 || int(m_merge_groups[idx].size()) == m_encode_parameters.x_stripepermergegroup)
          {
            m_agg_start_cid = rand_num(m_num_of_Clusters - agg_clusters_num);
            m_free_clusters.clear();
            m_free_clusters.shrink_to_fit();
            for (int i = 0; i < m_agg_start_cid; i++)
            {
              m_free_clusters.push_back(i);
            }
            for (int i = m_agg_start_cid + agg_clusters_num; i < m_num_of_Clusters; i++)
            {
              m_free_clusters.push_back(i);
            }
            std::vector<int> temp;
            temp.push_back(stripe_id);
            m_merge_groups.push_back(temp);
          }
          else
          {
            m_merge_groups[idx].push_back(stripe_id);
          }

          int agg_cluster_id = m_agg_start_cid - 1;
          int t_cluster_id = -1;
          int g_cluster_id = m_agg_start_cid + agg_clusters_num - 1;
          for (int i = 0; i < l; i++)
          {
            for (int j = i * b; j < (i + 1) * b; j += g_m + 1)
            {
              bool flag = false;
              if (j + g_m + 1 >= (i + 1) * b)
                flag = true;
              if (flag && j + g_m + 1 != (i + 1) * b)
              {
                t_cluster_id = ++agg_cluster_id;
              }
              else
              {
                t_cluster_id = m_free_clusters[rand_num(int(m_free_clusters.size()))];
                auto iter = std::find(m_free_clusters.begin(), m_free_clusters.end(), t_cluster_id);
                if (iter != m_free_clusters.end())
                {
                  m_free_clusters.erase(iter);
                }
              }
              Cluster &t_cluster = m_cluster_table[t_cluster_id];
              // place every g+1 data blocks from each group to a single cluster
              for (int o = j; o < j + g_m + 1 && o < (i + 1) * b; o++)
              {
                // randomly select a node in the selected cluster
                int t_node_id = randomly_select_a_node(t_cluster_id, stripe_id);
                blocks_info[o].map2cluster = t_cluster_id;
                blocks_info[o].map2node = t_node_id;
                update_stripe_info_in_node(true, t_node_id, stripe_id);
                t_cluster.blocks.push_back(&blocks_info[o]);
                t_cluster.stripes.insert(stripe_id);
                stripe_info.place2clusters.insert(t_cluster_id);
              }
              // place local parity blocks
              if (flag)
              {
                if (j + g_m + 1 != (i + 1) * b) // b % (g + 1) != 0
                {
                  // randomly select a node in the selected cluster
                  int t_node_id = randomly_select_a_node(t_cluster_id, stripe_id);
                  blocks_info[k + g_m + i].map2cluster = t_cluster_id;
                  blocks_info[k + g_m + i].map2node = t_node_id;
                  update_stripe_info_in_node(true, t_node_id, stripe_id);
                  t_cluster.blocks.push_back(&blocks_info[k + g_m + i]);
                  t_cluster.stripes.insert(stripe_id);
                  stripe_info.place2clusters.insert(t_cluster_id);
                }
                else // place the local parity blocks together with global ones
                {
                  Cluster &g_cluster = m_cluster_table[g_cluster_id];
                  int t_node_id = randomly_select_a_node(g_cluster_id, stripe_id);
                  blocks_info[k + g_m + i].map2cluster = g_cluster_id;
                  blocks_info[k + g_m + i].map2node = t_node_id;
                  update_stripe_info_in_node(true, t_node_id, stripe_id);
                  g_cluster.blocks.push_back(&blocks_info[k + g_m + i]);
                  g_cluster.stripes.insert(stripe_id);
                  stripe_info.place2clusters.insert(g_cluster_id);
                }
              }
            }
          }
          Cluster &g_cluster = m_cluster_table[g_cluster_id];
          // place the global parity blocks to the selected cluster
          for (int i = 0; i < g_m; i++)
          {
            int t_node_id = randomly_select_a_node(g_cluster_id, stripe_id);
            blocks_info[k + i].map2cluster = g_cluster_id;
            blocks_info[k + i].map2node = t_node_id;
            update_stripe_info_in_node(true, t_node_id, stripe_id);
            g_cluster.blocks.push_back(&blocks_info[k + i]);
            g_cluster.stripes.insert(stripe_id);
            stripe_info.place2clusters.insert(g_cluster_id);
          }
        }
      }
    }

    if (IF_DEBUG)
    {
      std::cout << std::endl;
      std::cout << "Data placement result:" << std::endl;
      for (int i = 0; i < m_num_of_Clusters; i++)
      {
        Cluster &t_cluster = m_cluster_table[i];
        if (int(t_cluster.blocks.size()) > 0)
        {
          std::cout << "Cluster " << i << ": ";
          for (auto it = t_cluster.blocks.begin(); it != t_cluster.blocks.end(); it++)
          {
            std::cout << "[" << (*it)->block_key << ":S" << (*it)->map2stripe << "G" << (*it)->map2group << "N" << (*it)->map2node << "] ";
          }
          std::cout << std::endl;
        }
      }
      std::cout << std::endl;
      std::cout << "Merge Group: ";
      for (auto it1 = m_merge_groups.begin(); it1 != m_merge_groups.end(); it1++)
      {
        std::cout << "[ ";
        for (auto it2 = (*it1).begin(); it2 != (*it1).end(); it2++)
        {
          std::cout << (*it2) << " ";
        }
        std::cout << "] ";
      }
      std::cout << std::endl;
    }

    // randomly select a cluster
    int r_idx = rand_num(int(stripe_info.place2clusters.size()));
    int selected_cluster_id = *(std::next(stripe_info.place2clusters.begin(), r_idx));
    if (IF_DEBUG)
    {
      std::cout << "[SET] Select the proxy in cluster " << selected_cluster_id << " to encode and set!" << std::endl;
    }
    return selected_cluster_id;
  }

  void CoordinatorImpl::blocks_in_cluster(std::map<char, std::vector<ECProject::Block *>> &block_info, int cluster_id, int stripe_id)
  {
    std::vector<ECProject::Block *> tt, td, tl, tg;
    Cluster &cluster = m_cluster_table[cluster_id];
    std::vector<Block *>::iterator it;
    for (it = cluster.blocks.begin(); it != cluster.blocks.end(); it++)
    {
      Block *block = *it;
      if (block->map2stripe == stripe_id)
      {
        tt.push_back(block);
        if (block->block_type == 'D')
        {
          td.push_back(block);
        }
        else if (block->block_type == 'L')
        {
          tl.push_back(block);
        }
        else
        {
          tg.push_back(block);
        }
      }
    }
    block_info['T'] = tt;
    block_info['D'] = td;
    block_info['L'] = tl;
    block_info['G'] = tg;
  }

  void CoordinatorImpl::find_max_group(int &max_group_id, int &max_group_num, int cluster_id, int stripe_id)
  {
    int group_cnt[5] = {0};
    Cluster &cluster = m_cluster_table[cluster_id];
    std::vector<Block *>::iterator it;
    for (it = cluster.blocks.begin(); it != cluster.blocks.end(); it++)
    {
      if ((*it)->map2stripe == stripe_id)
      {
        group_cnt[(*it)->map2group]++;
      }
    }
    for (int i = 0; i <= m_encode_parameters.l_localparityblock; i++)
    {
      if (group_cnt[i] > max_group_num)
      {
        max_group_id = i;
        max_group_num = group_cnt[i];
      }
    }
  }

  int CoordinatorImpl::count_block_num(char type, int cluster_id, int stripe_id, int group_id)
  {
    int cnt = 0;
    Cluster &cluster = m_cluster_table[cluster_id];
    std::vector<Block *>::iterator it;
    for (it = cluster.blocks.begin(); it != cluster.blocks.end(); it++)
    {
      Block *block = *it;
      if (block->map2stripe == stripe_id)
      {
        if (group_id == -1)
        {
          if (type == 'T')
          {
            cnt++;
          }
          else if (block->block_type == type)
          {
            cnt++;
          }
        }
        else if (int(block->map2group) == group_id)
        {
          if (type == 'T')
          {
            cnt++;
          }
          else if (block->block_type == type)
          {
            cnt++;
          }
        }
      }
    }
    if (cnt == 0)
    {
      cluster.stripes.erase(stripe_id);
    }
    return cnt;
  }

  // find out if any specific type of block from the stripe exists in the cluster
  bool CoordinatorImpl::find_block(char type, int cluster_id, int stripe_id)
  {
    Cluster &cluster = m_cluster_table[cluster_id];
    std::vector<Block *>::iterator it;
    for (it = cluster.blocks.begin(); it != cluster.blocks.end(); it++)
    {
      if (stripe_id != -1 && int((*it)->map2stripe) == stripe_id && (*it)->block_type == type)
      {
        return true;
      }
      else if (stripe_id == -1 && (*it)->block_type == type)
      {
        return true;
      }
    }
    return false;
  }
} // namespace ECProject
