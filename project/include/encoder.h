#ifndef encoder_H
#define encoder_H
#include <memory>
#include <vector>
#include <cstring>
#include <chrono>
#include <string>
#include <unordered_map>
#include <utility>
namespace ECProject
{
    static const unsigned char gff_base[] = {
        0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1d, 0x3a, 0x74, 0xe8, 0xcd, 0x87, 0x13,
        0x26, 0x4c, 0x98, 0x2d, 0x5a, 0xb4, 0x75, 0xea, 0xc9, 0x8f, 0x03, 0x06, 0x0c, 0x18, 0x30,
        0x60, 0xc0, 0x9d, 0x27, 0x4e, 0x9c, 0x25, 0x4a, 0x94, 0x35, 0x6a, 0xd4, 0xb5, 0x77, 0xee,
        0xc1, 0x9f, 0x23, 0x46, 0x8c, 0x05, 0x0a, 0x14, 0x28, 0x50, 0xa0, 0x5d, 0xba, 0x69, 0xd2,
        0xb9, 0x6f, 0xde, 0xa1, 0x5f, 0xbe, 0x61, 0xc2, 0x99, 0x2f, 0x5e, 0xbc, 0x65, 0xca, 0x89,
        0x0f, 0x1e, 0x3c, 0x78, 0xf0, 0xfd, 0xe7, 0xd3, 0xbb, 0x6b, 0xd6, 0xb1, 0x7f, 0xfe, 0xe1,
        0xdf, 0xa3, 0x5b, 0xb6, 0x71, 0xe2, 0xd9, 0xaf, 0x43, 0x86, 0x11, 0x22, 0x44, 0x88, 0x0d,
        0x1a, 0x34, 0x68, 0xd0, 0xbd, 0x67, 0xce, 0x81, 0x1f, 0x3e, 0x7c, 0xf8, 0xed, 0xc7, 0x93,
        0x3b, 0x76, 0xec, 0xc5, 0x97, 0x33, 0x66, 0xcc, 0x85, 0x17, 0x2e, 0x5c, 0xb8, 0x6d, 0xda,
        0xa9, 0x4f, 0x9e, 0x21, 0x42, 0x84, 0x15, 0x2a, 0x54, 0xa8, 0x4d, 0x9a, 0x29, 0x52, 0xa4,
        0x55, 0xaa, 0x49, 0x92, 0x39, 0x72, 0xe4, 0xd5, 0xb7, 0x73, 0xe6, 0xd1, 0xbf, 0x63, 0xc6,
        0x91, 0x3f, 0x7e, 0xfc, 0xe5, 0xd7, 0xb3, 0x7b, 0xf6, 0xf1, 0xff, 0xe3, 0xdb, 0xab, 0x4b,
        0x96, 0x31, 0x62, 0xc4, 0x95, 0x37, 0x6e, 0xdc, 0xa5, 0x57, 0xae, 0x41, 0x82, 0x19, 0x32,
        0x64, 0xc8, 0x8d, 0x07, 0x0e, 0x1c, 0x38, 0x70, 0xe0, 0xdd, 0xa7, 0x53, 0xa6, 0x51, 0xa2,
        0x59, 0xb2, 0x79, 0xf2, 0xf9, 0xef, 0xc3, 0x9b, 0x2b, 0x56, 0xac, 0x45, 0x8a, 0x09, 0x12,
        0x24, 0x48, 0x90, 0x3d, 0x7a, 0xf4, 0xf5, 0xf7, 0xf3, 0xfb, 0xeb, 0xcb, 0x8b, 0x0b, 0x16,
        0x2c, 0x58, 0xb0, 0x7d, 0xfa, 0xe9, 0xcf, 0x83, 0x1b, 0x36, 0x6c, 0xd8, 0xad, 0x47, 0x8e,
        0x01};

    static const unsigned char gflog_base[] = {
        0x00, 0xff, 0x01, 0x19, 0x02, 0x32, 0x1a, 0xc6, 0x03, 0xdf, 0x33, 0xee, 0x1b, 0x68, 0xc7,
        0x4b, 0x04, 0x64, 0xe0, 0x0e, 0x34, 0x8d, 0xef, 0x81, 0x1c, 0xc1, 0x69, 0xf8, 0xc8, 0x08,
        0x4c, 0x71, 0x05, 0x8a, 0x65, 0x2f, 0xe1, 0x24, 0x0f, 0x21, 0x35, 0x93, 0x8e, 0xda, 0xf0,
        0x12, 0x82, 0x45, 0x1d, 0xb5, 0xc2, 0x7d, 0x6a, 0x27, 0xf9, 0xb9, 0xc9, 0x9a, 0x09, 0x78,
        0x4d, 0xe4, 0x72, 0xa6, 0x06, 0xbf, 0x8b, 0x62, 0x66, 0xdd, 0x30, 0xfd, 0xe2, 0x98, 0x25,
        0xb3, 0x10, 0x91, 0x22, 0x88, 0x36, 0xd0, 0x94, 0xce, 0x8f, 0x96, 0xdb, 0xbd, 0xf1, 0xd2,
        0x13, 0x5c, 0x83, 0x38, 0x46, 0x40, 0x1e, 0x42, 0xb6, 0xa3, 0xc3, 0x48, 0x7e, 0x6e, 0x6b,
        0x3a, 0x28, 0x54, 0xfa, 0x85, 0xba, 0x3d, 0xca, 0x5e, 0x9b, 0x9f, 0x0a, 0x15, 0x79, 0x2b,
        0x4e, 0xd4, 0xe5, 0xac, 0x73, 0xf3, 0xa7, 0x57, 0x07, 0x70, 0xc0, 0xf7, 0x8c, 0x80, 0x63,
        0x0d, 0x67, 0x4a, 0xde, 0xed, 0x31, 0xc5, 0xfe, 0x18, 0xe3, 0xa5, 0x99, 0x77, 0x26, 0xb8,
        0xb4, 0x7c, 0x11, 0x44, 0x92, 0xd9, 0x23, 0x20, 0x89, 0x2e, 0x37, 0x3f, 0xd1, 0x5b, 0x95,
        0xbc, 0xcf, 0xcd, 0x90, 0x87, 0x97, 0xb2, 0xdc, 0xfc, 0xbe, 0x61, 0xf2, 0x56, 0xd3, 0xab,
        0x14, 0x2a, 0x5d, 0x9e, 0x84, 0x3c, 0x39, 0x53, 0x47, 0x6d, 0x41, 0xa2, 0x1f, 0x2d, 0x43,
        0xd8, 0xb7, 0x7b, 0xa4, 0x76, 0xc4, 0x17, 0x49, 0xec, 0x7f, 0x0c, 0x6f, 0xf6, 0x6c, 0xa1,
        0x3b, 0x52, 0x29, 0x9d, 0x55, 0xaa, 0xfb, 0x60, 0x86, 0xb1, 0xbb, 0xcc, 0x3e, 0x5a, 0xcb,
        0x59, 0x5f, 0xb0, 0x9c, 0xa9, 0xa0, 0x51, 0x0b, 0xf5, 0x16, 0xeb, 0x7a, 0x75, 0x2c, 0xd7,
        0x4f, 0xae, 0xd5, 0xe9, 0xe6, 0xe7, 0xad, 0xe8, 0x74, 0xd6, 0xf4, 0xea, 0xa8, 0x50, 0x58,
        0xaf};

    void encode_lotuslrc(int k, int r, int z, unsigned char **data_ptrs, unsigned char **parity_ptrs, int block_size);

    void gen_lotuslrc_matrix(unsigned char *encode_matrix, int k, int r, int z);

    void encode_unilrc(int k, int r, int z, unsigned char **data_ptrs, unsigned char **parity_ptrs, int block_size);

    void encode_azure_lrc(int k, int r, int z, unsigned char **data_ptrs, unsigned char **parity_ptrs, int block_size);

    void encode_optimal_lrc(int k, int r, int z, unsigned char **data_ptrs, unsigned char **parity_ptrs, int block_size);

    void encode_uniform_lrc(int k, int r, int z, unsigned char **data_ptrs, unsigned char **parity_ptrs, int block_size);

    void partial_encode_unilrc(int k, int r, int z, int data_block_num, unsigned char **data_ptrs, unsigned char **parity_ptrs, int block_size);

    void partial_encode_azure_lrc(int k, int r, int z, int data_block_num, unsigned char **data_ptrs, unsigned char **parity_ptrs, int block_size);

    void partial_encode_optimal_lrc(int k, int r, int z, int data_block_num, unsigned char **data_ptrs, unsigned char **parity_ptrs, int block_size);

    void partial_encode_uniform_lrc(int k, int r, int z, int data_block_num, unsigned char **data_ptrs, unsigned char **parity_ptrs, int block_size);

    /* Matrix generation helpers: generate full (m+z) x k encode matrix for each scheme
     * encode_matrix must point to an allocated buffer of size (k + r + z) * k bytes
     */
    void gen_unilrc_matrix(unsigned char *encode_matrix, int k, int r, int z);
    void gen_azure_lrc_matrix(unsigned char *encode_matrix, int k, int r, int z);
    void gen_optimal_lrc_matrix(unsigned char *encode_matrix, int k, int r, int z);
    void gen_uniform_lrc_matrix(unsigned char *encode_matrix, int k, int r, int z);

    void decode_unilrc(const int k, const int r, const int z, const int block_num,
                       const std::vector<int> *block_indexes, unsigned char **block_ptrs, unsigned char *res_ptr, int block_size);

    void decode_azure_lrc(const int k, const int r, const int z, const int block_num,
                          const std::vector<int> *block_indexes, unsigned char **block_ptrs, unsigned char *res_ptr, int block_size,
                          int failed_block_id);

    void decode_optimal_lrc(const int k, const int r, const int z, const int block_num,
                            const std::vector<int> *block_indexes, unsigned char **block_ptrs, unsigned char *res_ptr, int block_size,
                            int failed_block_id);

    void decode_uniform_lrc(const int k, const int r, const int z, const int block_num,
                            const std::vector<int> *block_indexes, unsigned char **block_ptrs, unsigned char *res_ptr, int block_size,
                            int failed_block_id);
    void decode_lotus_lrc(const int k, const int r, const int z, const int block_num,
                          const std::vector<int> *block_indexes, unsigned char **block_ptrs, unsigned char *res_ptr, int block_size,
                          int failed_block_id);

    int xor_avx(int vects, int len, void **array);

    unsigned char
    gf_inv(unsigned char a);

    void gf_gen_cauchy_matrix1(unsigned char *a, int m, int k);

    void gf_gen_local_vector(unsigned char *a, int k, int p);

    void
    gf_gen_rs_matrix1(unsigned char *a, int m, int k);

    void
    ec_encode_data_base(int len, int srcs, int dests, unsigned char *v, unsigned char **src,
                    unsigned char **dest);

    void
    ec_encode_data_avx2(int len, int k, int rows, unsigned char *g_tbls, unsigned char **data,
                    unsigned char **coding);

    void
    ec_init_tables(int k, int rows, unsigned char *a, unsigned char *g_tbls);

    void
    encode_data(int len, int k, int rows, unsigned char *matrix, unsigned char **data, unsigned char **coding);

    void
    gf_vect_mul_init(unsigned char c, unsigned char *tbl);

    unsigned char
    gf_mul(unsigned char a, unsigned char b);

    int
    gf_invert_matrix(unsigned char *in_mat, unsigned char *out_mat, const int n);

    void
    gf_mul_vect_matrix(unsigned char* vect, unsigned char* matrix, unsigned char *dest, int k);

    int xor_avx(int vects, int len, void **array);

    enum class TwoBlockRecoveryMode {
        TwoSingleBlock,
        GlobalThenSingle,
        LotusSameGroupPlanBased,
    };

    int get_block_id_to_local_group_id(const std::string &code_type, int k, int r, int z, int block_id);
    bool blocks_same_local_group(const std::string &code_type, int k, int r, int z, int block_id0, int block_id1);
    TwoBlockRecoveryMode select_two_block_recovery_mode(const std::string &code_type, int k, int r, int z,
                                                        int block_id0, int block_id1);

    enum class RecoveryPhaseKind {
        GlobalMulti,
        SingleBlock,
    };

    struct RecoveryPhase {
        RecoveryPhaseKind kind;
        std::vector<int> all_failed;
        std::vector<int> recover_ids;
    };

    // Plan multi-block recovery phases. all_failed in each GlobalMulti phase is the full
    // unavailable set; recover_ids is the N-1 (or N-2 for Lotus) global batch subset.
    std::vector<RecoveryPhase> plan_multi_block_recovery(const std::string &code_type, int k, int r,
                                                         int z, const std::vector<int> &failed);

    // Global multi-block decode plan:
    // - failed_block_indexes: ids of failed blocks (row indices in generator matrix)
    // - global_decode_block_indexes: chosen source block ids (columns to read)
    // - local_source_block_ids/local_matrix: optional. If both non-null, this function
    //   will also build a local coefficient matrix of size (rows x cols) where rows is
    //   the number of failed blocks and cols == local_source_block_ids->size().
    //   local_matrix is row-major: local_matrix[f * cols + i] corresponds to
    //   recovery_block_indexes[f] and (*local_source_block_ids)[i].
    // - failed_block_indexes: all unavailable blocks (decode plan input).
    // - recovery_block_indexes: optional; if null or empty, recover all failed in failed_block_indexes order.
    //   Otherwise must be a subset (same ids as in failed_block_indexes); rows = recovery size.
    bool get_global_decode_plan(int k, int r, int z, const std::string &code_type,
                                const std::vector<int> &failed_block_indexes,
                                std::vector<int> &global_decode_block_indexes,
                                const std::vector<int> *local_source_block_ids,
                                unsigned char *local_matrix,
                                int &rows, int &cols,
                                const std::vector<int> *recovery_block_indexes = nullptr);

    // LotusLRC true two-block same-local-group recovery plan (one round, local only).
    // Given exactly two failed blocks in the same local group, compute:
    // - chosen_sources: a minimal set of surviving local-group blocks (a basis of the
    //   local-group generator-row space) to read;
    // - full_coeffs (row-major, size recovery_order.size() * chosen_sources.size()):
    //   GF coefficients so that block(recovery_order[rr]) = sum_j full_coeffs[rr][j] * block(chosen_sources[j]).
    // Returns true on success; false if the two blocks are not in the same local group
    // or the local span cannot reconstruct a failed block (caller should fall back to
    // the global k x k plan).
    bool get_lotus_two_block_local_plan(int k, int r, int z,
                                        const std::vector<int> &failed_block_indexes,
                                        const std::vector<int> &recovery_order,
                                        std::vector<int> &chosen_sources,
                                        std::vector<unsigned char> &full_coeffs);

    // Maintenance-robust read leftover local fill: strict GF solve.
    // Given the leftover data block(s) to reconstruct and a fixed-order list of available
    // source blocks (members of the leftover's local group: local parity block(s), surviving
    // data siblings, group-internal global parity blocks, and already-reconstructed members),
    // compute coeffs (row-major, size leftover.size() * source_block_ids.size()) such that
    //   block(leftover[rr]) = sum_i coeffs[rr*S + i] * block(source_block_ids[i])   over GF(2^8).
    // Coefficients are exact (handle weighted local parities, folded globals, and the
    // inverse of the leftover's own coefficient). Returns false if any leftover block is not
    // in the span of the provided sources (caller should fall back to a global decode).
    bool get_local_fill_plan(int k, int r, int z, const std::string &code_type,
                             const std::vector<int> &leftover_block_ids,
                             const std::vector<int> &source_block_ids,
                             std::vector<unsigned char> &coeffs);

    /* Data layout / placement: per-group block counts for data, global parity, local parity (by code_type) */
    std::vector<int> get_data_block_num_per_group(int k, int r, int z, const std::string &code_type);
    std::vector<int> get_global_parity_block_num_per_group(int k, int r, int z, const std::string &code_type);
    std::vector<int> get_local_parity_block_num_per_group(int k, int r, int z, const std::string &code_type);

    /* Per-code-type layout functions */
    std::vector<int> get_data_block_num_per_group_azurelrc(int k, int r, int z);
    std::vector<int> get_global_parity_block_num_per_group_azurelrc(int k, int r, int z);
    std::vector<int> get_local_parity_block_num_per_group_azurelrc(int k, int r, int z);
    int get_azurelrc_block_id_to_local_group_id(int k, int r, int z, int block_id);
    std::unordered_map<int, int> get_azurelrc_block_id_to_group_id(int k, int r, int z);
    std::unordered_map<int, std::vector<int>> get_azurelrc_group_id_to_block_ids(int k, int r, int z);

    std::vector<int> get_data_block_num_per_group_optimal_lrc(int k, int r, int z);
    std::vector<int> get_global_parity_block_num_per_group_optimal_lrc(int k, int r, int z);
    std::vector<int> get_local_parity_block_num_per_group_optimal_lrc(int k, int r, int z);
    int get_optimal_lrc_block_id_to_local_group_id(int k, int r, int z, int block_id);
    std::unordered_map<int, int> get_optimal_lrc_block_id_to_group_id(int k, int r, int z);
    std::unordered_map<int, std::vector<int>> get_optimal_lrc_group_id_to_block_ids(int k, int r, int z);

    std::vector<int> get_data_block_num_per_group_uniform_lrc(int k, int r, int z);
    std::vector<int> get_global_parity_block_num_per_group_uniform_lrc(int k, int r, int z);
    std::vector<int> get_local_parity_block_num_per_group_uniform_lrc(int k, int r, int z);
    std::vector<int> get_uniform_lrc_local_group_sizes(int k, int r, int z);
    std::vector<int> get_uniform_lrc_group_sizes(int k, int r, int z);
    std::vector<int> get_uniform_lrc_group_num_per_local_group(int k, int r, int z);
    int get_uniform_lrc_block_id_to_local_group_id(int k, int r, int z, int block_id);
    std::unordered_map<int, int> get_uniform_lrc_block_id_to_group_id(int k, int r, int z);
    std::unordered_map<int, std::vector<int>> get_uniform_lrc_group_id_to_block_ids(int k, int r, int z);

    std::vector<int> get_data_block_num_per_group_unilrc(int k, int r, int z);
    std::vector<int> get_global_parity_block_num_per_group_unilrc(int k, int r, int z);
    std::vector<int> get_local_parity_block_num_per_group_unilrc(int k, int r, int z);
    int get_unilrc_block_id_to_local_group_id(int k, int r, int z, int block_id);

    /* LotusLRC layout */
    std::vector<int> get_data_block_num_per_group_lotuslrc(int k, int r, int z);
    std::vector<int> get_global_parity_block_num_per_group_lotuslrc(int k, int r, int z);
    std::vector<int> get_local_parity_block_num_per_group_lotuslrc(int k, int r, int z);

    std::unordered_map<int, int> get_lotuslrc_block_id_to_group_id(int k, int r, int z);
    std::unordered_map<int, std::vector<int>> get_lotuslrc_group_id_to_block_ids(int k, int r, int z);

    std::vector<int> get_data_block_num_per_local_group_lotuslrc(int k, int r, int z);
    std::vector<int> get_global_parity_block_num_per_local_group_lotuslrc(int k, int r, int z);
    std::vector<int> get_local_parity_block_num_per_local_group_lotuslrc(int k, int r, int z);

    int get_lotuslrc_local_group_id_to_block_id(int k, int r, int z, int local_group_id);
    int get_lotuslrc_block_id_to_local_group_id(int k, int r, int z, int block_id);

    std::vector<int> get_lotuslrc_group_sizes(int k, int r, int z);
    std::vector<int> get_lotuslrc_local_group_sizes(int k, int r, int z);
    std::vector<int> get_lotuslrc_group_num_per_local_group(int k, int r, int z);

    /* Single-block recovery / degraded read: (group_id, block_ids) per group. Caller uses when non-empty; otherwise fallback to get_recovery_group_ids + stripe.group_to_blocks. */
    std::vector<std::pair<int, std::vector<int>>> get_recovery_group_and_block_ids(const std::string &code_type, int k, int r, int z, int failed_block_id);

    std::vector<std::pair<int, std::vector<int>>> get_recovery_group_and_block_ids_optimal_lrc(int k, int r, int z, int failed_block_id);
    std::vector<std::pair<int, std::vector<int>>> get_recovery_group_and_block_ids_uniform_lrc(int k, int r, int z, int failed_block_id);
    std::vector<std::pair<int, std::vector<int>>> get_recovery_group_and_block_ids_unilrc(int k, int r, int z, int failed_block_id);
    std::vector<std::pair<int, std::vector<int>>> get_recovery_group_and_block_ids_azurelrc(int k, int r, int z, int failed_block_id);
    std::vector<std::pair<int, std::vector<int>>> get_recovery_group_and_block_ids_lotuslrc(int k, int r, int z, int failed_block_id);

    std::vector<std::pair<int, std::vector<int>>> get_recovery_group_and_block_ids_lotuslrc_2block_recovery(int k, int r, int z, int failed_block_id0, int failed_block_id1);
}

#endif