/**
 * Verify get_global_decode_plan coefficients reconstruct the EXACT original block values.
 *
 * This mirrors exactly what the proxy does in multi-block / rack / node recovery:
 *   1. get_global_decode_plan(..., failed, decode_ids, nullptr,...)  -> decode source ids
 *   2. get_global_decode_plan(..., failed, _, &decode_ids, local_matrix, rows, cols, &recover)
 *      -> rows x cols GF coefficient matrix projected onto the (here: full) source set
 *   3. recovered[rr] = XOR_j ( coeff[rr][j] * value(decode_ids[j]) )
 *      must equal value(recover[rr]).
 *
 * Pure GF math, no gRPC / cluster. Reports mismatches per pattern.
 */
#include "encoder.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using ECProject::gf_mul;

namespace {

void build_gen(const std::string &code, int k, int r, int z, std::vector<unsigned char> &G)
{
    G.assign(static_cast<size_t>(k + r + z) * k, 0);
    if (code == "UniLRC") ECProject::gen_unilrc_matrix(G.data(), k, r, z);
    else if (code == "AzureLRC") ECProject::gen_azure_lrc_matrix(G.data(), k, r, z);
    else if (code == "OptimalLRC") ECProject::gen_optimal_lrc_matrix(G.data(), k, r, z);
    else if (code == "UniformLRC") ECProject::gen_uniform_lrc_matrix(G.data(), k, r, z);
    else if (code == "LotusLRC") ECProject::gen_lotuslrc_matrix(G.data(), k, r, z);
    else throw std::runtime_error("unknown code");
}

int placement_group(const std::string &code, int k, int r, int z, int bid)
{
    if (code == "LotusLRC") return ECProject::get_lotuslrc_block_id_to_group_id(k, r, z).at(bid);
    if (code == "AzureLRC") return ECProject::get_azurelrc_block_id_to_group_id(k, r, z).at(bid);
    if (code == "OptimalLRC") return ECProject::get_optimal_lrc_block_id_to_group_id(k, r, z).at(bid);
    if (code == "UniformLRC") return ECProject::get_uniform_lrc_block_id_to_group_id(k, r, z).at(bid);
    return ECProject::get_azurelrc_block_id_to_group_id(k, r, z).at(bid);
}

// returns 0 if all recover blocks reconstructed correctly, else number of mismatches.
// prints a one-line result. fast_path_expected tells us if F triggers the placeholder path.
int verify(const std::string &code, int k, int r, int z,
           const std::vector<int> &failed, const std::vector<int> &recover,
           const std::vector<unsigned char> &val, const char *label)
{
    std::vector<int> decode_ids;
    int rows = 0, cols = 0;
    bool ok1 = ECProject::get_global_decode_plan(k, r, z, code, failed, decode_ids,
                                                 nullptr, nullptr, rows, cols, &recover);
    if (!ok1) {
        std::cout << "  [" << label << "] PLAN-FAILED failed=" << failed.size()
                  << " recover=" << recover.size() << "\n";
        return -1;
    }
    std::vector<unsigned char> mat(recover.size() * decode_ids.size(), 0);
    std::vector<int> decode_ids2;
    int rows2 = 0, cols2 = 0;
    bool ok2 = ECProject::get_global_decode_plan(k, r, z, code, failed, decode_ids2,
                                                 &decode_ids, mat.data(), rows2, cols2, &recover);
    if (!ok2 || cols2 != static_cast<int>(decode_ids.size())) {
        std::cout << "  [" << label << "] PROJECT-FAILED\n";
        return -1;
    }
    int mism = 0;
    for (size_t rr = 0; rr < recover.size(); ++rr) {
        unsigned char acc = 0;
        for (int j = 0; j < cols2; ++j)
            acc ^= gf_mul(mat[rr * cols2 + j], val[decode_ids[j]]);
        if (acc != val[recover[rr]]) ++mism;
    }
    std::cout << "  [" << label << "] failed=" << failed.size() << " recover=" << recover.size()
              << " sources=" << decode_ids.size() << " mismatch=" << mism
              << (mism == 0 ? "  OK" : "  *** WRONG ***") << "\n";
    return mism;
}

int run_code(const std::string &code, int k, int r, int z, int cluster_num)
{
    std::cout << "=== " << code << " k=" << k << " r=" << r << " z=" << z
              << " (cluster_num=" << cluster_num << ") ===\n";
    const int nrows = k + r + z;
    std::vector<unsigned char> G;
    build_gen(code, k, r, z, G);
    std::srand(31337);
    std::vector<unsigned char> msg(k);
    for (int i = 0; i < k; ++i) msg[i] = (unsigned char)(std::rand() & 0xff);
    std::vector<unsigned char> val(nrows, 0);
    for (int b = 0; b < nrows; ++b) {
        unsigned char a = 0;
        for (int c = 0; c < k; ++c) a ^= gf_mul(G[(size_t)b * k + c], msg[c]);
        val[b] = a;
    }
    int bad = 0;

    // Pattern A: two data blocks in the SAME local group, global stage recovers the first.
    {
        int a = -1, b = -1;
        for (int x = 0; x < k && b < 0; ++x) {
            int lgx = ECProject::get_block_id_to_local_group_id(code, k, r, z, x);
            if (a < 0) { a = x; continue; }
            if (ECProject::get_block_id_to_local_group_id(code, k, r, z, a) == lgx) b = x;
        }
        if (a >= 0 && b >= 0)
            bad += std::max(0, verify(code, k, r, z, {a, b}, {std::min(a,b)}, val, "2-block same-group global-first"));
    }
    // Pattern B: two data blocks in DIFFERENT local groups, recover both via one global plan.
    {
        int a = 0, b = -1, lga = ECProject::get_block_id_to_local_group_id(code, k, r, z, 0);
        for (int x = 1; x < k; ++x)
            if (ECProject::get_block_id_to_local_group_id(code, k, r, z, x) != lga) { b = x; break; }
        if (b >= 0)
            bad += std::max(0, verify(code, k, r, z, {a, b}, {a, b}, val, "2-block diff-group both"));
    }
    // Pattern C: rack-0 failed blocks (layout+placement), recover the global batch (all-1 leftover).
    {
        std::vector<int> failed;
        for (int bid = 0; bid < nrows; ++bid)
            if ((0 + placement_group(code, k, r, z, bid)) % cluster_num == 0)
                failed.push_back(bid);
        std::sort(failed.begin(), failed.end());
        if (failed.size() >= 2) {
            // recover all-but-one (the single-block leftover stays local) -- non-Lotus
            std::vector<int> recover(failed.begin(), failed.end() - 1);
            bad += std::max(0, verify(code, k, r, z, failed, recover, val, "rack-0 batch (N-1)"));
            // also recover ALL of them via one global plan
            bad += std::max(0, verify(code, k, r, z, failed, failed, val, "rack-0 batch (all N)"));
        }
    }
    // Pattern D: exactly r+1 erasures (the experimental fast-path trigger). Recover all of them.
    {
        std::vector<int> failed;
        for (int i = 0; i < r + 1 && i < nrows; ++i) failed.push_back(i);
        bad += std::max(0, verify(code, k, r, z, failed, failed, val, "F==r+1 (fast-path)"));
    }
    // Pattern E: exactly r erasures (max correctable, generic path). Recover all.
    {
        std::vector<int> failed;
        for (int i = 0; i < r && i < nrows; ++i) failed.push_back(i);
        if (!failed.empty())
            bad += std::max(0, verify(code, k, r, z, failed, failed, val, "F==r (generic)"));
    }
    return bad;
}

} // namespace

int main()
{
    int bad = 0;
    bad += run_code("AzureLRC", 48, 3, 4, 21);   // live config
    bad += run_code("AzureLRC", 30, 6, 6, 21);
    bad += run_code("OptimalLRC", 30, 10, 2, 21);
    bad += run_code("OptimalLRC", 48, 4, 4, 21);
    bad += run_code("UniformLRC", 30, 6, 6, 21);
    bad += run_code("UniformLRC", 48, 3, 4, 21);
    bad += run_code("UniformLRC", 72, 4, 8, 21);
    bad += run_code("LotusLRC", 48, 2, 8, 21);
    bad += run_code("LotusLRC", 72, 3, 8, 21);
    std::cout << (bad == 0 ? "\nALL PASS" : "\nFAILURES PRESENT")
              << " (total mismatches=" << bad << ")\n";
    return bad == 0 ? 0 : 1;
}
