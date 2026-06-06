/**
 * Single-block recovery audit for ALL code types.
 *
 * For each (code_type, k, r, z) and each block b:
 *   1. build gen_*_matrix, encode random data -> ground-truth block values (byte-wise GF)
 *   2. plan = get_recovery_group_and_block_ids(code, ..., b)   (the real recovery plan)
 *   3. feed the planned source blocks into the matching decode_*()  (exactly what the proxy does)
 *   4. compare the decoded bytes against the true value of block b
 *
 * Reports, per config: pass / mismatch / empty-plan counts. mismatch>0 means the
 * recovery algorithm produces WRONG data for that block.
 */
#include "encoder.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using ECProject::gf_mul;

namespace {

void build_matrix(const std::string &code, int k, int r, int z, std::vector<unsigned char> &G)
{
    const int nrows = k + r + z;
    G.assign((size_t)nrows * k, 0);
    if (code == "UniLRC")        ECProject::gen_unilrc_matrix(G.data(), k, r, z);
    else if (code == "AzureLRC") ECProject::gen_azure_lrc_matrix(G.data(), k, r, z);
    else if (code == "OptimalLRC") ECProject::gen_optimal_lrc_matrix(G.data(), k, r, z);
    else if (code == "UniformLRC") ECProject::gen_uniform_lrc_matrix(G.data(), k, r, z);
    else if (code == "LotusLRC") ECProject::gen_lotuslrc_matrix(G.data(), k, r, z);
    else throw std::runtime_error("unknown code");
}

void call_decode(const std::string &code, int k, int r, int z, int block_num,
                 const std::vector<int> &idx, unsigned char **ptrs, unsigned char *res,
                 int bs, int failed)
{
    if (code == "UniLRC")
        ECProject::decode_unilrc(k, r, z, block_num, &idx, ptrs, res, bs);
    else if (code == "AzureLRC")
        ECProject::decode_azure_lrc(k, r, z, block_num, &idx, ptrs, res, bs, failed);
    else if (code == "OptimalLRC")
        ECProject::decode_optimal_lrc(k, r, z, block_num, &idx, ptrs, res, bs, failed);
    else if (code == "UniformLRC")
        ECProject::decode_uniform_lrc(k, r, z, block_num, &idx, ptrs, res, bs, failed);
    else if (code == "LotusLRC")
        ECProject::decode_lotus_lrc(k, r, z, block_num, &idx, ptrs, res, bs, failed);
}

// Generic GF(2^8) span test: is gen-row[target] in the row space of {gen-row[idx]}?
// If yes, the planned source blocks are mathematically sufficient to recover the block.
bool span_recoverable(const std::vector<unsigned char> &G, int k,
                      const std::vector<int> &idx, int target)
{
    std::vector<std::vector<unsigned char>> rows;
    for (int id : idx) {
        std::vector<unsigned char> row(G.begin() + (size_t)id * k, G.begin() + (size_t)id * k + k);
        rows.push_back(row);
    }
    std::vector<unsigned char> tgt(G.begin() + (size_t)target * k, G.begin() + (size_t)target * k + k);
    // Gaussian elimination on rows; reduce tgt alongside.
    int pivcol = 0;
    for (size_t pr = 0; pr < rows.size() && pivcol < k; ++pivcol) {
        size_t sel = pr;
        while (sel < rows.size() && rows[sel][pivcol] == 0) ++sel;
        if (sel == rows.size()) continue;
        std::swap(rows[pr], rows[sel]);
        unsigned char inv = ECProject::gf_inv(rows[pr][pivcol]);
        for (int c = 0; c < k; ++c) rows[pr][c] = gf_mul(rows[pr][c], inv);
        for (size_t rr = 0; rr < rows.size(); ++rr) {
            if (rr == pr || rows[rr][pivcol] == 0) continue;
            unsigned char f = rows[rr][pivcol];
            for (int c = 0; c < k; ++c) rows[rr][c] ^= gf_mul(f, rows[pr][c]);
        }
        // reduce target by this pivot
        if (tgt[pivcol] != 0) {
            unsigned char f = tgt[pivcol];
            for (int c = 0; c < k; ++c) tgt[c] ^= gf_mul(f, rows[pr][c]);
        }
        ++pr;
    }
    for (int c = 0; c < k; ++c) if (tgt[c]) return false;
    return true;
}

void audit(const std::string &code, int k, int r, int z)
{
    const int nrows = k + r + z;
    const int bs = 64;
    std::vector<unsigned char> G;
    build_matrix(code, k, r, z, G);

    // data: k blocks of bs bytes
    std::srand(98765);
    std::vector<std::vector<unsigned char>> data(k, std::vector<unsigned char>(bs));
    for (int j = 0; j < k; ++j)
        for (int t = 0; t < bs; ++t) data[j][t] = (unsigned char)(std::rand() & 0xff);

    // true block values
    std::vector<std::vector<unsigned char>> val(nrows, std::vector<unsigned char>(bs, 0));
    for (int b = 0; b < nrows; ++b)
        for (int j = 0; j < k; ++j) {
            unsigned char c = G[(size_t)b * k + j];
            if (!c) continue;
            for (int t = 0; t < bs; ++t) val[b][t] ^= gf_mul(c, data[j][t]);
        }

    int pass = 0, mism = 0, empty = 0, badplan = 0, insufficient = 0;
    std::vector<int> mism_blocks, bad_blocks, insuf_blocks;
    for (int b = 0; b < nrows; ++b) {
        // Faithfully reproduce the real path: per-group (per-cluster) decode_* -> XOR partials.
        auto plan = ECProject::get_recovery_group_and_block_ids(code, k, r, z, b);
        bool any = false, reads_failed = false;
        for (auto &p : plan)
            for (int bid : p.second) { any = true; if (bid == b) reads_failed = true; }
        if (!any) { ++empty; continue; }
        if (reads_failed) { ++badplan; if (bad_blocks.size() < 20) bad_blocks.push_back(b); continue; }

        // Is the plan's source set even mathematically sufficient (generic GF solve)?
        std::vector<int> flat;
        for (auto &p : plan) for (int bid : p.second) flat.push_back(bid);
        bool suff = span_recoverable(G, k, flat, b);
        if (!suff) { ++insufficient; if (insuf_blocks.size() < 20) insuf_blocks.push_back(b); }

        std::vector<unsigned char> acc(bs, 0); // XOR of all group partials
        for (auto &p : plan) {
            const std::vector<int> &idx = p.second;
            const int bn = (int)idx.size();
            if (bn == 0) continue;
            std::vector<std::vector<unsigned char>> srcbuf(bn);
            std::vector<unsigned char *> ptrs(bn);
            for (int i = 0; i < bn; ++i) { srcbuf[i] = val[idx[i]]; ptrs[i] = srcbuf[i].data(); }
            std::vector<unsigned char> partial(bs, 0);
            call_decode(code, k, r, z, bn, idx, ptrs.data(), partial.data(), bs, b);
            for (int t = 0; t < bs; ++t) acc[t] ^= partial[t];
        }

        if (std::memcmp(acc.data(), val[b].data(), bs) == 0) ++pass;
        else { ++mism; if (mism_blocks.size() < 20) mism_blocks.push_back(b); }
    }
    std::cout << code << " k=" << k << " r=" << r << " z=" << z
              << "  blocks=" << nrows << " pass=" << pass
              << " mismatch=" << mism               << " empty_plan=" << empty
              << " plan_reads_failed=" << badplan
              << " plan_insufficient=" << insufficient;
    if (!mism_blocks.empty()) {
        std::cout << "  mismatch_blocks=[";
        for (size_t i = 0; i < mism_blocks.size(); ++i)
            std::cout << (i ? "," : "") << mism_blocks[i];
        std::cout << "]";
    }
    if (!bad_blocks.empty()) {
        std::cout << "  reads_failed_blocks=[";
        for (size_t i = 0; i < bad_blocks.size(); ++i)
            std::cout << (i ? "," : "") << bad_blocks[i];
        std::cout << "]";
    }
    std::cout << std::endl;
}

} // namespace

int main()
{
    audit("LotusLRC", 48, 2, 8);
    audit("AzureLRC", 30, 6, 6);
    audit("OptimalLRC", 30, 10, 2);
    audit("UniformLRC", 30, 6, 6);
    audit("UniLRC", 12, 6, 3);
    audit("UniLRC", 24, 8, 4);
    return 0;
}
