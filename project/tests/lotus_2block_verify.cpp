/**
 * Verification harness for LotusLRC two-block same-local-group local recovery.
 *
 * Goal: confirm that, for any two erased blocks in the same local group, the two
 * failed blocks' generator rows lie in the span of the surviving local-group
 * blocks' generator rows, and that the GF coefficients we compute reconstruct
 * the exact block values (block_b = G[b] . message).
 *
 * This validates the coefficient math BEFORE it is ported into recovery_strategy.cpp.
 */
#include "encoder.h"

#include <cstdlib>
#include <iostream>
#include <vector>

using ECProject::gf_mul;
using ECProject::gf_inv;

namespace {

// Solve A x = y over GF(2^8). A is (numEq x numVar) row-major, y is length numEq.
// Returns true and fills x (length numVar) if a consistent solution exists.
bool solve_gf(const std::vector<unsigned char> &A_in, const std::vector<unsigned char> &y_in,
              int numEq, int numVar, std::vector<unsigned char> &x)
{
    std::vector<unsigned char> A = A_in; // numEq x numVar
    std::vector<unsigned char> y = y_in; // numEq
    std::vector<int> pivot_col(numEq, -1);
    int row = 0;
    for (int col = 0; col < numVar && row < numEq; ++col) {
        int sel = -1;
        for (int rr = row; rr < numEq; ++rr) {
            if (A[rr * numVar + col] != 0) { sel = rr; break; }
        }
        if (sel == -1) continue;
        if (sel != row) {
            for (int c = 0; c < numVar; ++c)
                std::swap(A[row * numVar + c], A[sel * numVar + c]);
            std::swap(y[row], y[sel]);
        }
        unsigned char inv = gf_inv(A[row * numVar + col]);
        for (int c = 0; c < numVar; ++c)
            A[row * numVar + c] = gf_mul(A[row * numVar + c], inv);
        y[row] = gf_mul(y[row], inv);
        for (int rr = 0; rr < numEq; ++rr) {
            if (rr == row) continue;
            unsigned char f = A[rr * numVar + col];
            if (f == 0) continue;
            for (int c = 0; c < numVar; ++c)
                A[rr * numVar + c] ^= gf_mul(f, A[row * numVar + c]);
            y[rr] ^= gf_mul(f, y[row]);
        }
        pivot_col[row] = col;
        ++row;
    }
    // Consistency: any all-zero row in A must have y == 0.
    for (int rr = 0; rr < numEq; ++rr) {
        bool all_zero = true;
        for (int c = 0; c < numVar; ++c)
            if (A[rr * numVar + c] != 0) { all_zero = false; break; }
        if (all_zero && y[rr] != 0) return false;
    }
    x.assign(numVar, 0);
    for (int rr = 0; rr < numEq; ++rr) {
        if (pivot_col[rr] >= 0) x[pivot_col[rr]] = y[rr];
    }
    return true;
}

int local_group_of(int k, int r, int z, int b)
{
    return ECProject::get_lotuslrc_block_id_to_local_group_id(k, r, z, b);
}

// Collect all block ids that belong to local group lg.
std::vector<int> local_group_blocks(int k, int r, int z, int lg)
{
    std::vector<int> blocks;
    for (int b = 0; b < k + r + z; ++b)
        if (local_group_of(k, r, z, b) == lg) blocks.push_back(b);
    return blocks;
}

// Pick an independent subset (basis) of the given source rows (each length k).
std::vector<int> pick_basis(const std::vector<unsigned char> &G, int k,
                            const std::vector<int> &candidates)
{
    std::vector<int> chosen;
    std::vector<unsigned char> ech; // echelon rows, length k each
    std::vector<int> pcol;
    for (int cand : candidates) {
        std::vector<unsigned char> row(G.begin() + (size_t)cand * k, G.begin() + (size_t)cand * k + k);
        for (size_t i = 0; i < pcol.size(); ++i) {
            unsigned char f = row[pcol[i]];
            if (f == 0) continue;
            for (int c = 0; c < k; ++c)
                row[c] ^= gf_mul(f, ech[i * k + c]);
        }
        int piv = -1;
        for (int c = 0; c < k; ++c) if (row[c] != 0) { piv = c; break; }
        if (piv == -1) continue; // dependent
        unsigned char inv = gf_inv(row[piv]);
        for (int c = 0; c < k; ++c) row[c] = gf_mul(row[c], inv);
        ech.insert(ech.end(), row.begin(), row.end());
        pcol.push_back(piv);
        chosen.push_back(cand);
    }
    return chosen;
}

} // namespace

int main(int argc, char **argv)
{
    int k = 48, r = 2, z = 8;
    if (argc >= 4) { k = atoi(argv[1]); r = atoi(argv[2]); z = atoi(argv[3]); }
    const int nrows = k + r + z;

    std::vector<unsigned char> G(nrows * k, 0);
    ECProject::gen_lotuslrc_matrix(G.data(), k, r, z);

    // Random message and ground-truth block values.
    std::srand(12345);
    std::vector<unsigned char> msg(k);
    for (int i = 0; i < k; ++i) msg[i] = (unsigned char)(std::rand() & 0xff);
    std::vector<unsigned char> val(nrows, 0);
    for (int b = 0; b < nrows; ++b) {
        unsigned char acc = 0;
        for (int c = 0; c < k; ++c) acc ^= gf_mul(G[(size_t)b * k + c], msg[c]);
        val[b] = acc;
    }

    int local_group_num = z / 2;

    // --- single-block local recovery check (1 erasure from local-group survivors) ---
    {
        int s_total = 0, s_pass = 0, s_fb = 0;
        for (int lg = 0; lg < local_group_num; ++lg) {
            std::vector<int> blocks = local_group_blocks(k, r, z, lg);
            for (int f : blocks) {
                ++s_total;
                std::vector<int> survivors;
                for (int bid : blocks) if (bid != f) survivors.push_back(bid);
                std::vector<int> chosen = pick_basis(G, k, survivors);
                int D = (int)chosen.size();
                std::vector<unsigned char> Aeq((size_t)k * D, 0), yeq(k, 0);
                for (int eq = 0; eq < k; ++eq) {
                    for (int i = 0; i < D; ++i) Aeq[(size_t)eq * D + i] = G[(size_t)chosen[i] * k + eq];
                    yeq[eq] = G[(size_t)f * k + eq];
                }
                std::vector<unsigned char> coeff;
                if (!solve_gf(Aeq, yeq, k, D, coeff)) { ++s_fb; continue; }
                unsigned char acc = 0;
                for (int i = 0; i < D; ++i) acc ^= gf_mul(coeff[i], val[chosen[i]]);
                if (acc == val[f]) ++s_pass;
            }
        }
        std::cout << "[single-block] k=" << k << " r=" << r << " z=" << z
                  << " total=" << s_total << " local_ok=" << s_pass
                  << " need_global=" << s_fb << std::endl;
    }

    int total = 0, passed = 0, fallback = 0;
    for (int lg = 0; lg < local_group_num; ++lg) {
        std::vector<int> blocks = local_group_blocks(k, r, z, lg);
        for (size_t a = 0; a < blocks.size(); ++a) {
            for (size_t b = a + 1; b < blocks.size(); ++b) {
                int f0 = blocks[a], f1 = blocks[b];
                ++total;
                std::vector<int> survivors;
                for (int bid : blocks)
                    if (bid != f0 && bid != f1) survivors.push_back(bid);
                std::vector<int> chosen = pick_basis(G, k, survivors);

                // For each failed block solve coeff over chosen so that sum coeff_i G[chosen_i] = G[f].
                bool ok = true;
                std::vector<unsigned char> recon(2, 0);
                int fs[2] = {f0, f1};
                for (int t = 0; t < 2 && ok; ++t) {
                    int f = fs[t];
                    int D = (int)chosen.size();
                    std::vector<unsigned char> Aeq((size_t)k * D, 0); // k eq x D var
                    std::vector<unsigned char> yeq(k, 0);
                    for (int eq = 0; eq < k; ++eq) {
                        for (int i = 0; i < D; ++i)
                            Aeq[(size_t)eq * D + i] = G[(size_t)chosen[i] * k + eq];
                        yeq[eq] = G[(size_t)f * k + eq];
                    }
                    std::vector<unsigned char> coeff;
                    if (!solve_gf(Aeq, yeq, k, D, coeff)) { ok = false; break; }
                    unsigned char acc = 0;
                    for (int i = 0; i < D; ++i) acc ^= gf_mul(coeff[i], val[chosen[i]]);
                    recon[t] = acc;
                }
                if (!ok) {
                    ++fallback;
                    std::cout << "FALLBACK lg=" << lg << " f0=" << f0 << " f1=" << f1
                              << " (failed block not in local survivor span)" << std::endl;
                    continue;
                }
                if (recon[0] == val[f0] && recon[1] == val[f1]) {
                    ++passed;
                } else {
                    std::cout << "MISMATCH lg=" << lg << " f0=" << f0 << " f1=" << f1
                              << " got(" << (int)recon[0] << "," << (int)recon[1] << ")"
                              << " want(" << (int)val[f0] << "," << (int)val[f1] << ")" << std::endl;
                }
            }
        }
    }
    std::cout << "k=" << k << " r=" << r << " z=" << z
              << " total_pairs=" << total << " passed=" << passed
              << " fallback=" << fallback << " mismatch=" << (total - passed - fallback)
              << std::endl;
    return (total - passed - fallback) == 0 ? 0 : 1;
}
