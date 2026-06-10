#include <cassert>
#include "encoder.h"
#include <iostream>
#include <unordered_map>
#include <stdexcept>

extern "C" {
    void gf_vect_dot_prod_avx2(int len, int vec, unsigned char *g_tbls, unsigned char **buffs, unsigned char*dests);
    void gf_2vect_dot_prod_avx2(int len, int vec, unsigned char *g_tbls, unsigned char **buffs, unsigned char**dests);
    void gf_3vect_dot_prod_avx2(int len, int vec, unsigned char *g_tbls, unsigned char **buffs, unsigned char**dests);
    void gf_4vect_dot_prod_avx2(int len, int vec, unsigned char *g_tbls, unsigned char **buffs, unsigned char**dests);
    void gf_5vect_dot_prod_avx2(int len, int vec, unsigned char *g_tbls, unsigned char **buffs, unsigned char**dests);
    void gf_6vect_dot_prod_avx2(int len, int vec, unsigned char *g_tbls, unsigned char **buffs, unsigned char**dests);
    int xor_gen_avx(int vects, int len, void **array);
}

int ECProject::xor_avx(int vects, int len, void **array)
{
    return xor_gen_avx(vects, len, array);
}

unsigned char
ECProject::gf_inv(unsigned char a)
{
    if (a == 0)
        return 0;
    return ECProject::gff_base[255 - ECProject::gflog_base[a]];
}

void ECProject::gf_gen_local_vector(unsigned char *a, int k, int p)
{
    int i;

    for (i = 0; i < k; i++)
    {
        a[i] = ECProject::gf_inv(i ^ (k + p));
    }
}

void
ECProject::gf_gen_rs_matrix1(unsigned char *a, int m, int k)
{
        int i, j;
        unsigned char p, gen = 2;

        memset(a, 0, k * m);
        for (i = 0; i < k; i++)
                a[k * i + i] = 1;

        for (i = k; i < m; i++) {
                p = 1;
                for (j = 0; j < k; j++) {
                        a[k * i + j] = p;
                        p = gf_mul(p, gen);
                }
                gen = gf_mul(gen, 2);
        }
}

void
ECProject::gf_gen_cauchy_matrix1(unsigned char *a, int m, int k)
{
        int i, j;
        unsigned char *p;

        // Identity matrix in high position
        memset(a, 0, k * m);
        for (i = 0; i < k; i++)
                a[k * i + i] = 1;

        // For the rest choose 1/(i + j) | i != j
        p = &a[k * k];
        for (i = k; i < m; i++)
                for (j = 0; j < k; j++)
                        *p++ = gf_inv(i ^ j);
}

unsigned char
ECProject::gf_mul(unsigned char a, unsigned char b)
{
#ifndef GF_LARGE_TABLES
        int i;

        if ((a == 0) || (b == 0))
                return 0;

        return gff_base[(i = gflog_base[a] + gflog_base[b]) > 254 ? i - 255 : i];
#else
        return gf_mul_table_base[b * 256 + a];
#endif
}

int
ECProject::gf_invert_matrix(unsigned char *in_mat, unsigned char *out_mat, const int n)
{
        int i, j, k;
        unsigned char temp;

        // Set out_mat[] to the identity matrix
        for (i = 0; i < n * n; i++) // memset(out_mat, 0, n*n)
                out_mat[i] = 0;

        for (i = 0; i < n; i++)
                out_mat[i * n + i] = 1;

        // Inverse
        for (i = 0; i < n; i++) {
                // Check for 0 in pivot element
                if (in_mat[i * n + i] == 0) {
                        // Find a row with non-zero in current column and swap
                        for (j = i + 1; j < n; j++)
                                if (in_mat[j * n + i])
                                        break;

                        if (j == n) // Couldn't find means it's singular
                                return -1;

                        for (k = 0; k < n; k++) { // Swap rows i,j
                                temp = in_mat[i * n + k];
                                in_mat[i * n + k] = in_mat[j * n + k];
                                in_mat[j * n + k] = temp;

                                temp = out_mat[i * n + k];
                                out_mat[i * n + k] = out_mat[j * n + k];
                                out_mat[j * n + k] = temp;
                        }
                }

                temp = gf_inv(in_mat[i * n + i]); // 1/pivot
                for (j = 0; j < n; j++) {         // Scale row i by 1/pivot
                        in_mat[i * n + j] = gf_mul(in_mat[i * n + j], temp);
                        out_mat[i * n + j] = gf_mul(out_mat[i * n + j], temp);
                }

                for (j = 0; j < n; j++) {
                        if (j == i)
                                continue;

                        temp = in_mat[j * n + i];
                        for (k = 0; k < n; k++) {
                                out_mat[j * n + k] ^= gf_mul(temp, out_mat[i * n + k]);
                                in_mat[j * n + k] ^= gf_mul(temp, in_mat[i * n + k]);
                        }
                }
        }
        return 0;
}


void ECProject::encode_lotuslrc(int k, int r, int z, unsigned char **data_ptrs, unsigned char **parity_ptrs, int block_size)
{
    for(int i = 0; i < r + z; i++){
        memset(parity_ptrs[i], 0, block_size);
    }
    int m = k + r;
    unsigned char *encode_matrix = new unsigned char[(m + z) * k];
    gen_lotuslrc_matrix(encode_matrix, k, r, z);

    unsigned char *g_tbls = new unsigned char[k * (r + z) * 32];
    ec_init_tables(k, r + z, &encode_matrix[k * k], g_tbls);
    ec_encode_data_avx2(block_size, k, r + z, g_tbls, data_ptrs, parity_ptrs);
    delete[] encode_matrix;
    delete[] g_tbls;
}

void ECProject::encode_unilrc(int k, int r, int z, unsigned char **data_ptrs, unsigned char **parity_ptrs, int block_size)
{
    for(int i = 0; i < r + z; i++){
        memset(parity_ptrs[i], 0, block_size);
    }
    int m = k + r;
    unsigned char *encode_matrix = new unsigned char[(m + z) * k];
    gen_unilrc_matrix(encode_matrix, k, r, z);

    unsigned char *g_tbls = new unsigned char[k * (r + z) * 32];
    ec_init_tables(k, r + z, &encode_matrix[k * k], g_tbls);
    ec_encode_data_avx2(block_size, k, r + z, g_tbls, data_ptrs, parity_ptrs);

    delete[] encode_matrix;
    delete[] g_tbls;
}

void ECProject::partial_encode_unilrc(int k, int r, int z, int data_block_num, unsigned char **data_ptrs, unsigned char **parity_ptrs, int block_size)
{
    for(int i = 0; i < r + z; i++){
        memset(parity_ptrs[i], 0, block_size);
    }
    int m = k + r;
    unsigned char *encode_matrix = new unsigned char[(m + z) * k];
    gen_unilrc_matrix(encode_matrix, k, r, z);

    unsigned char *sub_matrix = new unsigned char[(r + z) * data_block_num];
    for (int i = 0; i < r + z; i++) {
        memcpy(sub_matrix + i * data_block_num, 
               encode_matrix + (k + i) * k,      
               data_block_num);                   
    }

    unsigned char *g_tbls = new unsigned char[data_block_num * (r + z) * 32];
    ec_init_tables(data_block_num, r + z, sub_matrix, g_tbls);

    ec_encode_data_avx2(block_size, 
                        data_block_num,  
                        r + z,           
                        g_tbls, 
                        data_ptrs,       
                        parity_ptrs);

    delete[] encode_matrix;
    delete[] sub_matrix;
    delete[] g_tbls;
}

void ECProject::encode_azure_lrc(int k, int r, int z, unsigned char **data_ptrs, unsigned char **parity_ptrs, int block_size)
{
    for(int i = 0; i < r + z; i++){
        memset(parity_ptrs[i], 0, block_size);
    }
    int m = k + r;
    unsigned char *encode_matrix = new unsigned char[(m + z)* k];
    gen_azure_lrc_matrix(encode_matrix, k, r, z);

    unsigned char *g_tbls = new unsigned char[k * (r + z) * 32];
    ec_init_tables(k, r + z, &encode_matrix[k * k], g_tbls);
    ec_encode_data_avx2(block_size, k, r + z, g_tbls, data_ptrs, parity_ptrs);

    delete[] encode_matrix;
    delete[] g_tbls;
}

void ECProject::partial_encode_azure_lrc(int k, int r, int z, int data_block_num, unsigned char **data_ptrs, unsigned char **parity_ptrs, int block_size)
{
    for(int i = 0; i < r + z; i++){
        memset(parity_ptrs[i], 0, block_size);
    }
    int m = k + r;
    unsigned char *encode_matrix = new unsigned char[(m + z)* k];
    gen_azure_lrc_matrix(encode_matrix, k, r, z);

    unsigned char *sub_matrix = new unsigned char[(r + z) * data_block_num];
    for (int i = 0; i < r + z; i++) {
        memcpy(sub_matrix + i * data_block_num, 
               encode_matrix + (k + i) * k,      
               data_block_num);                   
    }

    unsigned char *g_tbls = new unsigned char[data_block_num * (r + z) * 32];
    ec_init_tables(data_block_num, r + z, sub_matrix, g_tbls);

    ec_encode_data_avx2(block_size, 
                        data_block_num,  
                        r + z,           
                        g_tbls, 
                        data_ptrs,       
                        parity_ptrs);

    delete[] encode_matrix;
    delete[] sub_matrix;
    delete[] g_tbls;
}

void ECProject::encode_optimal_lrc(int k, int r, int z, unsigned char **data_ptrs, unsigned char **parity_ptrs, int block_size)
{
    for(int i = 0; i < r + z; i++){
        memset(parity_ptrs[i], 0, block_size);
    }
    int m = k + r;
    unsigned char *encode_matrix = new unsigned char[(m + z) * k];
    gen_optimal_lrc_matrix(encode_matrix, k, r, z);

    unsigned char *g_tbls = new unsigned char[k * (r + z)* 32];
    ec_init_tables(k, r + z, &encode_matrix[k * k], g_tbls);
    ec_encode_data_avx2(block_size, k, r + z, g_tbls, data_ptrs, parity_ptrs);
    delete[] encode_matrix;
    delete[] g_tbls;
}

void ECProject::partial_encode_optimal_lrc(int k, int r, int z, int data_block_num, unsigned char **data_ptrs, unsigned char **parity_ptrs, int block_size)
{
    for(int i = 0; i < r + z; i++){
        memset(parity_ptrs[i], 0, block_size);
    }
    int m = k + r;
    unsigned char *encode_matrix = new unsigned char[(m + z) * k];
    gen_optimal_lrc_matrix(encode_matrix, k, r, z);

    unsigned char *sub_matrix = new unsigned char[(r + z) * data_block_num];
    for (int i = 0; i < r + z; i++) {
        memcpy(sub_matrix + i * data_block_num, 
               encode_matrix + (k + i) * k,      
               data_block_num);                   
    }

    unsigned char *g_tbls = new unsigned char[data_block_num * (r + z) * 32];
    ec_init_tables(data_block_num, r + z, sub_matrix, g_tbls);

    ec_encode_data_avx2(block_size, 
                        data_block_num,  
                        r + z,           
                        g_tbls, 
                        data_ptrs,       
                        parity_ptrs);

    delete[] encode_matrix;
    delete[] sub_matrix;
    delete[] g_tbls;
}

void ECProject::encode_uniform_lrc(int k, int r, int z, unsigned char **data_ptrs, unsigned char **parity_ptrs, int block_size)
{
    for(int i = 0; i < r + z; i++){
        memset(parity_ptrs[i], 0, block_size);
    }
    int m = k + r;
    unsigned char *encode_matrix = new unsigned char[(m + z) * k];
    gen_uniform_lrc_matrix(encode_matrix, k, r, z);

    unsigned char *g_tbls = new unsigned char[k * (r + z)* 32];
    ec_init_tables(k, r + z, &encode_matrix[k * k], g_tbls);
    ec_encode_data_avx2(block_size, k, r + z, g_tbls, data_ptrs, parity_ptrs);
    delete[] encode_matrix;
    delete[] g_tbls;
}

void ECProject::partial_encode_uniform_lrc(int k, int r, int z, int data_block_num, unsigned char **data_ptrs, unsigned char **parity_ptrs, int block_size)
{
    for(int i = 0; i < r + z; i++){
        memset(parity_ptrs[i], 0, block_size);
    }
    int m = k + r;
    unsigned char *encode_matrix = new unsigned char[(m + z) * k];
    gen_uniform_lrc_matrix(encode_matrix, k, r, z);

    unsigned char *sub_matrix = new unsigned char[(r + z) * data_block_num];
    for (int i = 0; i < r + z; i++) {
        memcpy(sub_matrix + i * data_block_num, 
               encode_matrix + (k + i) * k,      
               data_block_num);                   
    }

    unsigned char *g_tbls = new unsigned char[data_block_num * (r + z) * 32];
    ec_init_tables(data_block_num, r + z, sub_matrix, g_tbls);

    ec_encode_data_avx2(block_size, 
                        data_block_num,  
                        r + z,           
                        g_tbls, 
                        data_ptrs,       
                        parity_ptrs);

    delete[] encode_matrix;
    delete[] sub_matrix;
    delete[] g_tbls;
    
}

// ===== Matrix generation helpers implementations =====
void ECProject::gen_unilrc_matrix(unsigned char *encode_matrix, int k, int r, int z)
{
    int m = k + r;
    memset(encode_matrix, 0, (m + z) * k);
    gf_gen_rs_matrix1(encode_matrix, m, k);
    for(int i = 0; i < k; i++){
        int row = i / (k / z);
        encode_matrix[(m + row) * k + i] = 1;
    }
    for(int i = 0; i < z; i++){
        for(int j = 0; j < k; j++){
            for(int l = 0; l < r / z; l++){
                encode_matrix[(m + i) * k + j] ^= encode_matrix[(k + i * r / z + l) * k + j];
            }
        }
    }
}

void ECProject::gen_azure_lrc_matrix(unsigned char *encode_matrix, int k, int r, int z)
{
    int m = k + r;
    memset(encode_matrix, 0, (m + z) * k);
    gf_gen_rs_matrix1(encode_matrix, m, k);
    for(int i = 0; i < k; i++){
        int row = i / (k / z);
        encode_matrix[(m + row) * k + i] = 1;
    }
}

void ECProject::gen_optimal_lrc_matrix(unsigned char *encode_matrix, int k, int r, int z)
{
    int m = k + r;
    memset(encode_matrix, 0, (m + z) * k);
    gf_gen_cauchy_matrix1(encode_matrix, m, k);
    unsigned char *local_vector = new unsigned char[k];
    gf_gen_local_vector(local_vector, k, r);
    for(int i = 0; i < k; i++){
        int row = i / (k / z);
        encode_matrix[(m + row) * k + i] = local_vector[i];
    }
    for(int i = 0; i < z; i++){
        for(int j = 0; j < k; j++){
            for(int l = 0; l < r; l++){
                encode_matrix[(m + i) * k + j] ^= encode_matrix[(k + l) * k + j];
            }
        }
    }
    delete[] local_vector;
}

void ECProject::gen_uniform_lrc_matrix(unsigned char *encode_matrix, int k, int r, int z)
{
    int m = k + r;
    memset(encode_matrix, 0, (m + z) * k);
    gf_gen_cauchy_matrix1(encode_matrix, m, k);
    unsigned char *local_vector = new unsigned char[k];
    gf_gen_local_vector(local_vector, k, r);
    // The k data blocks and the r global-parity blocks (k+r "data-like" blocks) are split into z
    // contiguous local groups; each local group's local parity protects ALL of its members.
    // This partition MUST match get_uniform_lrc_block_id_to_local_group_id() in encoder_layout.cpp
    // (run sizes (k+r)/z, with the last (k+r)%z groups holding one extra), so that globals fall
    // into the last local group(s). A data member d contributes local_vector[d]*data[d]; a global
    // member gp contributes 1*global[gp] (its Cauchy row XORed into the group's local parity row).
    auto datalike_group = [&](int b) -> int {
        int base = (k + r) / z;
        int larger = (k + r) % z;          // last `larger` groups hold one extra data-like block
        int cur = 0;
        for (int g = 0; g < z; g++) {
            int run = base + (g >= z - larger ? 1 : 0);
            if (b < cur + run) return g;
            cur += run;
        }
        return z - 1;
    };
    for(int d = 0; d < k; d++){
        int row = datalike_group(d);
        encode_matrix[(m + row) * k + d] = local_vector[d];
    }
    for(int gp = k; gp < k + r; gp++){
        int row = datalike_group(gp);
        for(int j = 0; j < k; j++){
            encode_matrix[(m + row) * k + j] ^= encode_matrix[gp * k + j];
        }
    }
    delete[] local_vector;
}

void ECProject::gen_lotuslrc_matrix(unsigned char *encode_matrix, int k, int r, int z)
{
    assert(z % 2 == 0 && "z must be even");
    int group_num = z / 2;
    int m = k + r;
    memset(encode_matrix, 0, (m + z) * k);
    gf_gen_cauchy_matrix1(encode_matrix, m, k);
    
    int group_size = k / group_num;
    //std::vector<int> data_block_num_per_local_group = get_data_block_num_per_local_group_lotuslrc(k, r, z);
    //std::vector<int> global_parity_block_num_per_local_group = get_global_parity_block_num_per_local_group_lotuslrc(k, r, z);

    unsigned char *local_vector0 = new unsigned char[k];
    gf_gen_local_vector(local_vector0, k, r);
    unsigned char *local_vector1 = new unsigned char[k];
    gf_gen_local_vector(local_vector1, k, r + 1);
    
    for(int i = 0; i < group_num; i++){
        memcpy(encode_matrix + (m + i * 2) * k + i * group_size, local_vector0 + i * group_size, group_size * sizeof(unsigned char));
        memcpy(encode_matrix + (m + i * 2 + 1) * k + i * group_size, local_vector1 + i * group_size, group_size * sizeof(unsigned char));
    }
    delete[] local_vector0;
    delete[] local_vector1;

    // Split each global parity block (k + i) into the two local-parity rows of the
    // local group that holds it. The last r local groups hold one global parity each,
    // so global parity (k + i) belongs to local group (group_num - r + i), whose two
    // local-parity rows are at (m + 2*lg) and (m + 2*lg + 1).
    // assume one local group has at most one global parity block, may need to be
    // adjusted for more general cases.
    for(int i = 0; i < r; i++){
        int lg = group_num - r + i;
        for(int j = 0; j < k; j++){
            encode_matrix[(m + 2 * lg) * k + j] ^= encode_matrix[(k + i) * k + j];
            encode_matrix[(m + 2 * lg + 1) * k + j] ^= encode_matrix[(k + i) * k + j];
        }
    }

}

void ECProject::decode_unilrc(const int k, const int r, const int z, const int block_num,
                              const std::vector<int> *block_indexes, unsigned char **block_ptrs, unsigned char *res_ptr, int block_size)
{
    memset(res_ptr, 0, block_size);
    unsigned char *vect_ptrs[block_num + 1];
    for(int i = 0; i < block_num; i++){
        vect_ptrs[i] = block_ptrs[i];
    }
    vect_ptrs[block_num] = res_ptr;
    xor_gen_avx(block_num + 1, block_size, (void **)vect_ptrs);
}

namespace ECProject {
namespace {
// Defined below; exact generator-matrix single-block decode shared by Optimal/Lotus/Azure-global.
void decode_block_via_generator(const std::string &code, int k, int r, int z, int block_num,
                                const std::vector<int> *block_indexes, unsigned char **block_ptrs,
                                unsigned char *res_ptr, int block_size, int failed_block_id);
} // namespace
} // namespace ECProject

void ECProject::decode_azure_lrc(const int k, const int r, const int z, const int block_num,
                                 const std::vector<int> *block_indexes, unsigned char **block_ptrs, unsigned char *res_ptr, int block_size,
                                 int failed_block_id)
{
    if(block_num == 0){
        return;
    }
    memset(res_ptr, 0, block_size);
    if (failed_block_id < k || failed_block_id >= k + r){
        unsigned char *vect_ptrs[block_num + 1];
        for(int i = 0; i < block_num; i++){
            vect_ptrs[i] = block_ptrs[i];
        }
        vect_ptrs[block_num] = res_ptr;
        xor_gen_avx(block_num + 1, block_size, (void **)vect_ptrs);
    }
    else
    {
        // Global-parity recovery: the previous RS-inverse path used a generator matrix that did
        // not match gen_azure_lrc_matrix (and mis-mapped sources), producing wrong data. Use the
        // exact generator-matrix solve instead.
        decode_block_via_generator("AzureLRC", k, r, z, block_num, block_indexes, block_ptrs,
                                   res_ptr, block_size, failed_block_id);
    }
}

namespace ECProject {
namespace {

// Exact, code-type-agnostic single-block decode coefficients for the per-group XOR partial scheme.
//
// Single-block recovery is run as: each cross-rack group computes a weighted partial over ONLY the
// source blocks it holds, and the destination XORs the partials. For that decomposition to be
// correct, every source block's coefficient must be grouping-independent. We obtain those exact
// coefficients by solving, over the FULL deterministic recovery source set
// (= flatten(get_recovery_group_and_block_ids(failed))), the system
//   gen[failed] = sum_s  C_s * gen[s]   over GF(2^8)
// via get_local_fill_plan (generator-matrix based; validated for every code type). Each decode_*
// call then applies the C_s of exactly the blocks it was given.
//
// This is correct for data blocks, local-parity blocks, global-parity blocks and folded-global
// members alike -- no out-of-bounds local_vector indexing and no per-code coefficient special
// casing (the previous root cause of wrong recovered data for Optimal/Lotus and Azure globals).
void decode_block_via_generator(const std::string &code, int k, int r, int z, int block_num,
                                const std::vector<int> *block_indexes, unsigned char **block_ptrs,
                                unsigned char *res_ptr, int block_size, int failed_block_id)
{
    memset(res_ptr, 0, block_size);
    if (block_num == 0)
        return;
    std::vector<int> flat;
    try {
        auto plan = get_recovery_group_and_block_ids(code, k, r, z, failed_block_id);
        for (auto &p : plan)
            for (int b : p.second)
                flat.push_back(b);
    } catch (const std::exception &e) {
        std::cerr << "[decode] " << code << " plan error for block " << failed_block_id
                  << ": " << e.what() << std::endl;
        return;
    }
    std::vector<unsigned char> coeffs;
    if (flat.empty() || !get_local_fill_plan(k, r, z, code, {failed_block_id}, flat, coeffs)) {
        std::cerr << "[decode] " << code << " cannot derive exact coefficients for block "
                  << failed_block_id << std::endl;
        return;
    }
    std::unordered_map<int, int> pos;
    for (int i = 0; i < static_cast<int>(flat.size()); i++)
        pos[flat[i]] = i;
    std::vector<unsigned char> decode_vector(static_cast<size_t>(block_num), 0);
    for (int i = 0; i < block_num; i++) {
        auto it = pos.find(block_indexes->at(i));
        if (it != pos.end())
            decode_vector[i] = coeffs[it->second];
    }
    unsigned char *g_tbls = new unsigned char[static_cast<size_t>(block_num) * 32];
    ec_init_tables(block_num, 1, decode_vector.data(), g_tbls);
    unsigned char *res_ptr_ptr[1] = {res_ptr};
    ec_encode_data_avx2(block_size, block_num, 1, g_tbls, block_ptrs, res_ptr_ptr);
    delete[] g_tbls;
}

} // namespace
} // namespace ECProject

void ECProject::decode_optimal_lrc(const int k, const int r, const int z, const int block_num,
                                   const std::vector<int> *block_indexes, unsigned char **block_ptrs, unsigned char *res_ptr, int block_size, int failed_block_id)
{
    decode_block_via_generator("OptimalLRC", k, r, z, block_num, block_indexes, block_ptrs,
                               res_ptr, block_size, failed_block_id);
}
void ECProject::decode_uniform_lrc(const int k, const int r, const int z, const int block_num,
                                   const std::vector<int> *block_indexes, unsigned char **block_ptrs, unsigned char *res_ptr, int block_size, int failed_block_id)
{
    if(block_num == 0){
        return;
    }
    memset(res_ptr, 0, block_size);
    unsigned char *local_vector;
    local_vector = new unsigned char[k];
    gf_gen_local_vector(local_vector, k, r);
    unsigned char *decode_vector = new unsigned char[block_num];

    for(int i = 0; i < block_num; i++){
        if(block_indexes->at(i) < k){
            decode_vector[i] = local_vector[block_indexes->at(i)];
        }
        else{
            decode_vector[i] = 1;
        }
    }
    if(failed_block_id < k){
        unsigned char factor = gf_inv(local_vector[failed_block_id]);
        for(int i = 0; i < block_num; i++){
            decode_vector[i] = gf_mul(decode_vector[i], factor);
        }
    }

    unsigned char *g_tbls = new unsigned char[block_num * 32];
    unsigned char **res_ptr_ptr = new unsigned char *[1];
    res_ptr_ptr[0] = res_ptr;
    ec_init_tables(block_num, 1, decode_vector, g_tbls);
    ec_encode_data_avx2(block_size, block_num, 1, g_tbls, block_ptrs, res_ptr_ptr);

    delete[] local_vector;
    delete[] decode_vector;
    delete[] g_tbls;
    delete[] res_ptr_ptr;
}

void ECProject::decode_lotus_lrc(const int k, const int r, const int z, const int block_num,
                                 const std::vector<int> *block_indexes, unsigned char **block_ptrs, unsigned char *res_ptr, int block_size,
                                 int failed_block_id)
{
    decode_block_via_generator("LotusLRC", k, r, z, block_num, block_indexes, block_ptrs,
                               res_ptr, block_size, failed_block_id);
}

void
ECProject::ec_encode_data_avx2(int len, int k, int rows, unsigned char *g_tbls, unsigned char **data,
                    unsigned char **coding)
{

        if (len < 32) {
                ec_encode_data_base(len, k, rows, g_tbls, data, coding);
                return;
        }

        while (rows >= 6) {
                gf_6vect_dot_prod_avx2(len, k, g_tbls, data, coding);
                g_tbls += 6 * k * 32;
                coding += 6;
                rows -= 6;
        }
        switch (rows) {
        case 5:
                gf_5vect_dot_prod_avx2(len, k, g_tbls, data, coding);
                break;
        case 4:
                gf_4vect_dot_prod_avx2(len, k, g_tbls, data, coding);
                break;
        case 3:
                gf_3vect_dot_prod_avx2(len, k, g_tbls, data, coding);
                break;
        case 2:
                gf_2vect_dot_prod_avx2(len, k, g_tbls, data, coding);
                break;
        case 1:
                gf_vect_dot_prod_avx2(len, k, g_tbls, data, *coding);
                break;
        case 0:
                break;
        }
}

void
ECProject::ec_encode_data_base(int len, int srcs, int dests, unsigned char *v, unsigned char **src,
                    unsigned char **dest)
{
        int i, j, l;
        unsigned char s;

        for (l = 0; l < dests; l++) {
                for (i = 0; i < len; i++) {
                        s = 0;
                        for (j = 0; j < srcs; j++)
                                s ^= gf_mul(src[j][i], v[j * 32 + l * srcs * 32 + 1]);

                        dest[l][i] = s;
                }
        }
}

void
ECProject::encode_data(int len, int k, int rows, unsigned char *matrix, unsigned char **data,
                    unsigned char **coding)
{
        unsigned char *g_tbls = new unsigned char[k * rows * 32];

        ec_init_tables(k, rows, matrix, g_tbls);
        ec_encode_data_avx2(len, k, rows, g_tbls, data, coding);
        delete[] g_tbls;
}

void
ECProject::ec_init_tables(int k, int rows, unsigned char *a, unsigned char *g_tbls)
{
        int i, j;

        for (i = 0; i < rows; i++) {
                for (j = 0; j < k; j++) {
                        gf_vect_mul_init(*a++, g_tbls);
                        g_tbls += 32;
                }
        }
}

void
ECProject::gf_vect_mul_init(unsigned char c, unsigned char *tbl)
{
        unsigned char c2 = (c << 1) ^ ((c & 0x80) ? 0x1d : 0);   // Mult by GF{2}
        unsigned char c4 = (c2 << 1) ^ ((c2 & 0x80) ? 0x1d : 0); // Mult by GF{2}
        unsigned char c8 = (c4 << 1) ^ ((c4 & 0x80) ? 0x1d : 0); // Mult by GF{2}

#if (__WORDSIZE == 64 || _WIN64 || __x86_64__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        unsigned long long v1, v2, v4, v8, *t;
        unsigned long long v10, v20, v40, v80;
        unsigned char c17, c18, c20, c24;

        t = (unsigned long long *) tbl;

        v1 = c * 0x0100010001000100ull;
        v2 = c2 * 0x0101000001010000ull;
        v4 = c4 * 0x0101010100000000ull;
        v8 = c8 * 0x0101010101010101ull;

        v4 = v1 ^ v2 ^ v4;
        t[0] = v4;
        t[1] = v8 ^ v4;

        c17 = (c8 << 1) ^ ((c8 & 0x80) ? 0x1d : 0);   // Mult by GF{2}
        c18 = (c17 << 1) ^ ((c17 & 0x80) ? 0x1d : 0); // Mult by GF{2}
        c20 = (c18 << 1) ^ ((c18 & 0x80) ? 0x1d : 0); // Mult by GF{2}
        c24 = (c20 << 1) ^ ((c20 & 0x80) ? 0x1d : 0); // Mult by GF{2}

        v10 = c17 * 0x0100010001000100ull;
        v20 = c18 * 0x0101000001010000ull;
        v40 = c20 * 0x0101010100000000ull;
        v80 = c24 * 0x0101010101010101ull;

        v40 = v10 ^ v20 ^ v40;
        t[2] = v40;
        t[3] = v80 ^ v40;

#else // 32-bit or other
        unsigned char c3, c5, c6, c7, c9, c10, c11, c12, c13, c14, c15;
        unsigned char c17, c18, c19, c20, c21, c22, c23, c24, c25, c26, c27, c28, c29, c30, c31;

        c3 = c2 ^ c;
        c5 = c4 ^ c;
        c6 = c4 ^ c2;
        c7 = c4 ^ c3;

        c9 = c8 ^ c;
        c10 = c8 ^ c2;
        c11 = c8 ^ c3;
        c12 = c8 ^ c4;
        c13 = c8 ^ c5;
        c14 = c8 ^ c6;
        c15 = c8 ^ c7;

        tbl[0] = 0;
        tbl[1] = c;
        tbl[2] = c2;
        tbl[3] = c3;
        tbl[4] = c4;
        tbl[5] = c5;
        tbl[6] = c6;
        tbl[7] = c7;
        tbl[8] = c8;
        tbl[9] = c9;
        tbl[10] = c10;
        tbl[11] = c11;
        tbl[12] = c12;
        tbl[13] = c13;
        tbl[14] = c14;
        tbl[15] = c15;

        c17 = (c8 << 1) ^ ((c8 & 0x80) ? 0x1d : 0);   // Mult by GF{2}
        c18 = (c17 << 1) ^ ((c17 & 0x80) ? 0x1d : 0); // Mult by GF{2}
        c19 = c18 ^ c17;
        c20 = (c18 << 1) ^ ((c18 & 0x80) ? 0x1d : 0); // Mult by GF{2}
        c21 = c20 ^ c17;
        c22 = c20 ^ c18;
        c23 = c20 ^ c19;
        c24 = (c20 << 1) ^ ((c20 & 0x80) ? 0x1d : 0); // Mult by GF{2}
        c25 = c24 ^ c17;
        c26 = c24 ^ c18;
        c27 = c24 ^ c19;
        c28 = c24 ^ c20;
        c29 = c24 ^ c21;
        c30 = c24 ^ c22;
        c31 = c24 ^ c23;

        tbl[16] = 0;
        tbl[17] = c17;
        tbl[18] = c18;
        tbl[19] = c19;
        tbl[20] = c20;
        tbl[21] = c21;
        tbl[22] = c22;
        tbl[23] = c23;
        tbl[24] = c24;
        tbl[25] = c25;
        tbl[26] = c26;
        tbl[27] = c27;
        tbl[28] = c28;
        tbl[29] = c29;
        tbl[30] = c30;
        tbl[31] = c31;

#endif //__WORDSIZE == 64 || _WIN64 || __x86_64__
}

void
ECProject::gf_mul_vect_matrix(unsigned char* vect, unsigned char* matrix, unsigned char *dest, int k){
    for(int i = 0; i < k; i++){
        dest[i] = 0;
        for(int j = 0; j < k; j++){
            dest[i] ^= gf_mul(vect[j], matrix[j * k + i]);
        }
    }
}