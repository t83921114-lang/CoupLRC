/**
 * Verification harness for the maintenance-robust read "leftover local fill" strict GF solve.
 *
 * It validates ECProject::get_local_fill_plan for every code type: given a leftover block (or
 * a 2-block leftover in the same local group for LotusLRC) and the surviving members of its
 * local group as sources, the computed GF coefficients must reconstruct the EXACT block value
 *   block(leftover) = sum_i coeff_i * block(source_i)
 * against ground truth derived directly from the generator matrix.
 *
 * Pure GF math: no gRPC / cluster. Mirrors lotus_2block_verify.cpp's methodology.
 */
#include "encoder.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using ECProject::gf_mul;
using ECProject::get_local_fill_plan;
using ECProject::get_block_id_to_local_group_id;

namespace {

bool build_gen(const std::string &code, int k, int r, int z, std::vector<unsigned char> &G)
{
    G.assign(static_cast<size_t>(k + r + z) * k, 0);
    if (code == "UniLRC") ECProject::gen_unilrc_matrix(G.data(), k, r, z);
    else if (code == "AzureLRC") ECProject::gen_azure_lrc_matrix(G.data(), k, r, z);
    else if (code == "OptimalLRC") ECProject::gen_optimal_lrc_matrix(G.data(), k, r, z);
    else if (code == "UniformLRC") ECProject::gen_uniform_lrc_matrix(G.data(), k, r, z);
    else if (code == "LotusLRC") ECProject::gen_lotuslrc_matrix(G.data(), k, r, z);
    else return false;
    return true;
}

std::vector<int> local_group_members(const std::string &code, int k, int r, int z, int lg)
{
    std::vector<int> blocks;
    for (int b = 0; b < k + r + z; ++b)
        if (get_block_id_to_local_group_id(code, k, r, z, b) == lg) blocks.push_back(b);
    return blocks;
}

// Returns number of mismatches (0 == all good). leftover_count: 1, or 2 for Lotus same-group.
int run_case(const std::string &code, int k, int r, int z, int leftover_count)
{
    std::vector<unsigned char> G;
    if (!build_gen(code, k, r, z, G)) { std::cout << "  skip (no gen)\n"; return 0; }
    const int nrows = k + r + z;

    std::srand(2026);
    std::vector<unsigned char> msg(k);
    for (int i = 0; i < k; ++i) msg[i] = static_cast<unsigned char>(std::rand() & 0xff);
    std::vector<unsigned char> val(nrows, 0);
    for (int b = 0; b < nrows; ++b) {
        unsigned char acc = 0;
        for (int c = 0; c < k; ++c) acc ^= gf_mul(G[static_cast<size_t>(b) * k + c], msg[c]);
        val[b] = acc;
    }

    int total = 0, ok = 0, fb = 0, mism = 0;
    // Enumerate local groups; pick data-block leftover(s) within each group.
    std::vector<int> all_lg;
    for (int b = 0; b < nrows; ++b) {
        int lg = get_block_id_to_local_group_id(code, k, r, z, b);
        if (std::find(all_lg.begin(), all_lg.end(), lg) == all_lg.end()) all_lg.push_back(lg);
    }
    for (int lg : all_lg) {
        std::vector<int> members = local_group_members(code, k, r, z, lg);
        std::vector<int> data_members;
        for (int b : members) if (b < k) data_members.push_back(b);
        if (static_cast<int>(data_members.size()) < leftover_count) continue;

        auto try_leftover = [&](const std::vector<int> &leftover) {
            ++total;
            // Sources = all OTHER members of this local group (data siblings, group-internal
            // global parities, and local parity block(s)).
            std::vector<int> sources;
            for (int b : members)
                if (std::find(leftover.begin(), leftover.end(), b) == leftover.end())
                    sources.push_back(b);
            std::vector<unsigned char> coeffs;
            if (!get_local_fill_plan(k, r, z, code, leftover, sources, coeffs)) { ++fb; return; }
            const int S = static_cast<int>(sources.size());
            bool good = true;
            for (size_t rr = 0; rr < leftover.size(); ++rr) {
                unsigned char acc = 0;
                for (int i = 0; i < S; ++i)
                    acc ^= gf_mul(coeffs[rr * S + i], val[sources[i]]);
                if (acc != val[leftover[rr]]) {
                    good = false;
                    std::cout << "  MISMATCH code=" << code << " lg=" << lg
                              << " leftover=" << leftover[rr]
                              << " got=" << (int)acc << " want=" << (int)val[leftover[rr]] << "\n";
                }
            }
            if (good) ++ok; else ++mism;
        };

        if (leftover_count == 1) {
            for (int f : data_members) try_leftover({f});
        } else {
            for (size_t a = 0; a < data_members.size(); ++a)
                for (size_t b = a + 1; b < data_members.size(); ++b)
                    try_leftover({data_members[a], data_members[b]});
        }
    }
    std::cout << "  " << code << " k=" << k << " r=" << r << " z=" << z
              << " leftover_count=" << leftover_count
              << " total=" << total << " ok=" << ok << " need_global=" << fb
              << " mismatch=" << mism << "\n";
    return mism;
}

} // namespace

int main()
{
    int bad = 0;
    std::cout << "[maintenance local fill GF verify]\n";
    bad += run_case("AzureLRC", 30, 6, 6, 1);
    bad += run_case("OptimalLRC", 30, 10, 2, 1);
    bad += run_case("UniformLRC", 30, 6, 6, 1);
    bad += run_case("UniLRC", 30, 6, 6, 1);
    bad += run_case("LotusLRC", 48, 2, 8, 1);
    bad += run_case("LotusLRC", 48, 2, 8, 2); // same-local-group 2-block leftover
    std::cout << (bad == 0 ? "ALL PASS" : "FAILED") << " (mismatches=" << bad << ")\n";
    return bad == 0 ? 0 : 1;
}
