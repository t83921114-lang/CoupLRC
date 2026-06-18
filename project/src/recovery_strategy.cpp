/**
 * Recovery strategy: multi-block decode plan and single-block recovery group/block ids.
 * Moved from encoder.cpp (get_multi_decode_plan) and encoder_layout.cpp (get_recovery_group_and_block_ids*).
 */
#include "encoder.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ECProject {

int get_block_id_to_local_group_id(const std::string &code_type, int k, int r, int z, int block_id)
{
    if (code_type == "AzureLRC")
        return get_azurelrc_block_id_to_local_group_id(k, r, z, block_id);
    if (code_type == "OptimalLRC")
        return get_optimal_lrc_block_id_to_local_group_id(k, r, z, block_id);
    if (code_type == "UniformLRC")
        return get_uniform_lrc_block_id_to_local_group_id(k, r, z, block_id);
    if (code_type == "UniLRC") {
        if (block_id < k)
            return block_id / (k / z);
        if (block_id < k + r)
            return (block_id - k) / (r / z);
        return block_id - k - r;
    }
    if (code_type == "LotusLRC")
        return get_lotuslrc_block_id_to_local_group_id(k, r, z, block_id);
    throw std::runtime_error("unknown code type for local group id: " + code_type);
}

bool blocks_same_local_group(const std::string &code_type, int k, int r, int z, int block_id0, int block_id1)
{
    return get_block_id_to_local_group_id(code_type, k, r, z, block_id0) ==
           get_block_id_to_local_group_id(code_type, k, r, z, block_id1);
}

TwoBlockRecoveryMode select_two_block_recovery_mode(const std::string &code_type, int k, int r, int z,
                                                    int block_id0, int block_id1)
{
    if (!blocks_same_local_group(code_type, k, r, z, block_id0, block_id1))
        return TwoBlockRecoveryMode::TwoSingleBlock;
    if (code_type == "LotusLRC")
        return TwoBlockRecoveryMode::LotusSameGroupPlanBased;
    return TwoBlockRecoveryMode::GlobalThenSingle;
}

namespace {

// Solve A x = y over GF(2^8). A is (numEq x numVar) row-major, y is length numEq.
// Returns true and fills x (length numVar) iff a consistent solution exists.
bool gf_solve_rect(std::vector<unsigned char> A, std::vector<unsigned char> y,
                   int numEq, int numVar, std::vector<unsigned char> &x)
{
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
    for (int rr = 0; rr < numEq; ++rr) {
        bool all_zero = true;
        for (int c = 0; c < numVar; ++c)
            if (A[rr * numVar + c] != 0) { all_zero = false; break; }
        if (all_zero && y[rr] != 0) return false; // inconsistent
    }
    x.assign(numVar, 0);
    for (int rr = 0; rr < numEq; ++rr)
        if (pivot_col[rr] >= 0) x[pivot_col[rr]] = y[rr];
    return true;
}

// Pick an independent subset (a basis) of the given candidate generator rows.
// gen is the (k+r+z) x k generator matrix; each candidate is a block id (row index).
std::vector<int> gf_pick_basis(const unsigned char *gen, int k, const std::vector<int> &candidates)
{
    std::vector<int> chosen;
    std::vector<unsigned char> ech; // echelon rows, length k each
    std::vector<int> pcol;
    for (int cand : candidates) {
        std::vector<unsigned char> row(gen + (size_t)cand * k, gen + (size_t)cand * k + k);
        for (size_t i = 0; i < pcol.size(); ++i) {
            unsigned char f = row[pcol[i]];
            if (f == 0) continue;
            for (int c = 0; c < k; ++c)
                row[c] ^= gf_mul(f, ech[i * k + c]);
        }
        int piv = -1;
        for (int c = 0; c < k; ++c) if (row[c] != 0) { piv = c; break; }
        if (piv == -1) continue; // dependent on already-chosen rows
        unsigned char inv = gf_inv(row[piv]);
        for (int c = 0; c < k; ++c) row[c] = gf_mul(row[c], inv);
        ech.insert(ech.end(), row.begin(), row.end());
        pcol.push_back(piv);
        chosen.push_back(cand);
    }
    return chosen;
}

} // namespace

bool get_lotus_two_block_local_plan(int k, int r, int z,
                                    const std::vector<int> &failed_block_indexes,
                                    const std::vector<int> &recovery_order,
                                    std::vector<int> &chosen_sources,
                                    std::vector<unsigned char> &full_coeffs)
{
    chosen_sources.clear();
    full_coeffs.clear();
    if (failed_block_indexes.size() != 2) return false;
    int f0 = failed_block_indexes[0];
    int f1 = failed_block_indexes[1];
    int lg0 = get_lotuslrc_block_id_to_local_group_id(k, r, z, f0);
    int lg1 = get_lotuslrc_block_id_to_local_group_id(k, r, z, f1);
    if (lg0 != lg1) return false;

    const int nrows = k + r + z;
    std::vector<unsigned char> gen((size_t)nrows * k, 0);
    gen_lotuslrc_matrix(gen.data(), k, r, z);

    // Surviving blocks of the failed blocks' local group.
    std::vector<int> survivors;
    for (int b = 0; b < nrows; ++b) {
        if (b == f0 || b == f1) continue;
        if (get_lotuslrc_block_id_to_local_group_id(k, r, z, b) == lg0)
            survivors.push_back(b);
    }

    std::vector<int> chosen = gf_pick_basis(gen.data(), k, survivors);
    const int D = static_cast<int>(chosen.size());
    if (D == 0) return false;

    const int R = static_cast<int>(recovery_order.size());
    full_coeffs.assign((size_t)R * D, 0);
    for (int rr = 0; rr < R; ++rr) {
        int f = recovery_order[rr];
        // Solve sum_i coeff_i * gen[chosen_i] = gen[f] (k equations, D unknowns).
        std::vector<unsigned char> A((size_t)k * D, 0), y(k, 0);
        for (int eq = 0; eq < k; ++eq) {
            for (int i = 0; i < D; ++i)
                A[(size_t)eq * D + i] = gen[(size_t)chosen[i] * k + eq];
            y[eq] = gen[(size_t)f * k + eq];
        }
        std::vector<unsigned char> coeff;
        if (!gf_solve_rect(std::move(A), std::move(y), k, D, coeff))
            return false; // failed block not in local span -> caller falls back to global
        for (int i = 0; i < D; ++i)
            full_coeffs[(size_t)rr * D + i] = coeff[i];
    }
    chosen_sources = std::move(chosen);
    return true;
}

namespace {

// Build the full (k+r+z) x k generator matrix for the given code type.
bool build_generator_matrix(const std::string &code_type, int k, int r, int z,
                            std::vector<unsigned char> &gen)
{
    gen.assign(static_cast<size_t>(k + r + z) * k, 0);
    if (code_type == "UniLRC")
        gen_unilrc_matrix(gen.data(), k, r, z);
    else if (code_type == "AzureLRC")
        gen_azure_lrc_matrix(gen.data(), k, r, z);
    else if (code_type == "OptimalLRC")
        gen_optimal_lrc_matrix(gen.data(), k, r, z);
    else if (code_type == "UniformLRC")
        gen_uniform_lrc_matrix(gen.data(), k, r, z);
    else if (code_type == "LotusLRC")
        gen_lotuslrc_matrix(gen.data(), k, r, z);
    else
        return false;
    return true;
}

} // namespace

bool get_local_fill_plan(int k, int r, int z, const std::string &code_type,
                         const std::vector<int> &leftover_block_ids,
                         const std::vector<int> &source_block_ids,
                         std::vector<unsigned char> &coeffs)
{
    coeffs.clear();
    const int R = static_cast<int>(leftover_block_ids.size());
    const int S = static_cast<int>(source_block_ids.size());
    if (R == 0 || S == 0)
        return false;

    const int nrows = k + r + z;
    std::vector<unsigned char> gen;
    if (!build_generator_matrix(code_type, k, r, z, gen))
    {
        std::cerr << "Error: get_local_fill_plan unsupported code type " << code_type << std::endl;
        return false;
    }
    for (int bid : source_block_ids)
        if (bid < 0 || bid >= nrows)
            return false;
    for (int bid : leftover_block_ids)
        if (bid < 0 || bid >= nrows)
            return false;

    coeffs.assign(static_cast<size_t>(R) * S, 0);
    for (int rr = 0; rr < R; ++rr)
    {
        const int f = leftover_block_ids[rr];
        // Solve sum_i coeff_i * gen[source_i] = gen[f]  (k equations, S unknowns) over GF(2^8).
        std::vector<unsigned char> A(static_cast<size_t>(k) * S, 0), y(k, 0);
        for (int eq = 0; eq < k; ++eq)
        {
            for (int i = 0; i < S; ++i)
                A[static_cast<size_t>(eq) * S + i] = gen[static_cast<size_t>(source_block_ids[i]) * k + eq];
            y[eq] = gen[static_cast<size_t>(f) * k + eq];
        }
        std::vector<unsigned char> coeff;
        if (!gf_solve_rect(std::move(A), std::move(y), k, S, coeff))
            return false; // leftover not in the span of the provided sources
        for (int i = 0; i < S; ++i)
            coeffs[static_cast<size_t>(rr) * S + i] = coeff[i];
    }
    return true;
}

bool get_global_decode_plan(int k, int r, int z, const std::string &code_type,
                            const std::vector<int> &failed_block_indexes,
                            std::vector<int> &global_decode_block_indexes,
                            const std::vector<int> *local_source_block_ids,
                            unsigned char *local_matrix,
                            int &rows, int &cols,
                            const std::vector<int> *recovery_block_indexes)
{
    std::vector<int> recovery_order;
    if (recovery_block_indexes != nullptr && !recovery_block_indexes->empty()) {
        std::unordered_set<int> failed_set_early(failed_block_indexes.begin(), failed_block_indexes.end());
        for (int bid : *recovery_block_indexes) {
            if (!failed_set_early.count(bid)) {
                std::cerr << "Error: get_global_decode_plan recovery block id " << bid
                          << " is not in failed_block_indexes" << std::endl;
                global_decode_block_indexes.clear();
                rows = cols = 0;
                return false;
            }
        }
        recovery_order = *recovery_block_indexes;
    } else {
        recovery_order = failed_block_indexes;
    }
    const int R = static_cast<int>(recovery_order.size());
    if (R == 0) {
        global_decode_block_indexes.clear();
        rows = cols = 0;
        return false;
    }

    // LotusLRC: true two-block same-local-group local recovery (one round).
    // Reads only this local group's surviving blocks and produces a 2-row coefficient
    // matrix, so the existing multi-block transport returns 2 x BlockSize per helper.
    // Falls through to the generic global k x k plan if the local span is insufficient.
    if (code_type == "LotusLRC" && failed_block_indexes.size() == 2) {
        std::vector<int> chosen;
        std::vector<unsigned char> full_coeffs;
        if (get_lotus_two_block_local_plan(k, r, z, failed_block_indexes, recovery_order,
                                           chosen, full_coeffs)) {
            global_decode_block_indexes = chosen;
            rows = R;
            cols = 0;
            const int D = static_cast<int>(chosen.size());
            if (local_source_block_ids != nullptr && local_matrix != nullptr) {
                cols = static_cast<int>(local_source_block_ids->size());
                for (int rr = 0; rr < R; ++rr) {
                    for (int i = 0; i < cols; ++i) {
                        int local_bid = (*local_source_block_ids)[i];
                        int j = -1;
                        for (int jj = 0; jj < D; ++jj) {
                            if (chosen[jj] == local_bid) { j = jj; break; }
                        }
                        local_matrix[rr * cols + i] =
                            (j >= 0) ? full_coeffs[(size_t)rr * D + j] : 0;
                    }
                }
            }
            return true;
        }
        // else: not locally recoverable -> fall through to generic global plan
    }

    int m = k + r;
    int nrows = k + r + z;
    unsigned char gen_matrix[k * (k + r + z)];
    memset(gen_matrix, 0, k * (k + r + z));
    if(code_type == "UniLRC"){
        gen_unilrc_matrix(gen_matrix, k, r, z);
    }
    else if(code_type == "AzureLRC"){
        gen_azure_lrc_matrix(gen_matrix, k, r, z);
    }
    else if(code_type == "OptimalLRC"){
        gen_optimal_lrc_matrix(gen_matrix, k, r, z);
    }
    else if(code_type == "UniformLRC"){
        gen_uniform_lrc_matrix(gen_matrix, k, r, z);
    }
    else if(code_type == "LotusLRC"){
        gen_lotuslrc_matrix(gen_matrix, k, r, z);
    }
    else{
        std::cerr << "Error: Unsupported code type " << code_type << std::endl;
        return false;
    }

    // build failed set
    std::unordered_map<int, bool> failed_map;
    for (int idx : failed_block_indexes) failed_map[idx] = true;

    std::unordered_set<int> failed_local_groups;
    for (int idx : failed_block_indexes) {
        try {
            failed_local_groups.insert(
                get_block_id_to_local_group_id(code_type, k, r, z, idx));
        } catch (const std::exception &) {
        }
    }
    bool single_local_group_failure = !failed_local_groups.empty();
    if (single_local_group_failure) {
        for (int idx : failed_block_indexes) {
            try {
                if (!failed_local_groups.count(
                        get_block_id_to_local_group_id(code_type, k, r, z, idx))) {
                    single_local_group_failure = false;
                    break;
                }
            } catch (const std::exception &) {
                single_local_group_failure = false;
                break;
            }
        }
    }
    // Single-rack same-local-group loss: off-rack helpers are the failed group's global
    // parity plus its local parity on other racks; other groups' local parities mislead GE.
    auto is_decode_candidate = [&](int row_id) {
        if (failed_map.count(row_id))
            return false;
        if (single_local_group_failure && row_id >= k + r) {
            try {
                const int lg = get_block_id_to_local_group_id(code_type, k, r, z, row_id);
                if (!failed_local_groups.count(lg))
                    return false;
            } catch (const std::exception &) {
            }
        }
        return true;
    };

    // Prefer first m rows (global rows). Collect candidate rows (non-failed).
    std::vector<int> candidates;
    for (int i = 0; i < m; i++) {
        if (is_decode_candidate(i))
            candidates.push_back(i);
    }
    // If not enough, append remaining non-failed rows (local parity rows)
    if ((int)candidates.size() < k) {
        for (int i = m; i < nrows; i++) {
            if (is_decode_candidate(i))
                candidates.push_back(i);
            if ((int)candidates.size() >= k)
                break;
        }
    }

    // If still fewer than k rows, cannot form left-inverse
    if ((int)candidates.size() < k) {
        global_decode_block_indexes.clear();
        rows = cols = 0;
        return false;
    }

    // Find k linearly independent rows among candidates using Gaussian elimination (GF)
    int M = (int)candidates.size();
    unsigned char *mat = new unsigned char[M * k];
    // copy candidate rows into mat (row-major: M x k)
    for (int i = 0; i < M; i++) {
        int gro = candidates[i];
        for (int c = 0; c < k; c++) mat[i * k + c] = gen_matrix[gro * k + c];
    }

    int cur = 0; // current pivot row index in mat
    for (int col = 0; col < k && cur < M; col++) {
        // find row with non-zero in this column
        int sel = -1;
        for (int row = cur; row < M; row++) {
            if (mat[row * k + col] != 0) { sel = row; break; }
        }
        if (sel == -1) continue; // no pivot in this column

        // swap sel and cur (both in mat and candidates)
        if (sel != cur) {
            for (int c = 0; c < k; c++) {
                unsigned char tmp = mat[cur * k + c];
                mat[cur * k + c] = mat[sel * k + c];
                mat[sel * k + c] = tmp;
            }
            int tmpidx = candidates[cur];
            candidates[cur] = candidates[sel];
            candidates[sel] = tmpidx;
        }

        // normalize pivot row: make pivot == 1
        unsigned char pivot = mat[cur * k + col];
        unsigned char inv_pivot = gf_inv(pivot);
        for (int c = col; c < k; c++) mat[cur * k + c] = gf_mul(mat[cur * k + c], inv_pivot);

        // eliminate this column in all other rows
        for (int row = 0; row < M; row++) {
            if (row == cur) continue;
            unsigned char factor = mat[row * k + col];
            if (factor == 0) continue;
            for (int c = col; c < k; c++) {
                mat[row * k + c] ^= gf_mul(factor, mat[cur * k + c]);
            }
        }

        cur++;
    }

    // check if we found k independent rows (need cur >= k)
    if (cur < k) {
        delete[] mat;
        global_decode_block_indexes.clear();
        rows = cols = 0;
        return false;
    }

    // selected rows are candidates[0..k-1]
    std::vector<int> chosen(k);
    for (int i = 0; i < k; i++) chosen[i] = candidates[i];

    // build tempM from chosen rows (k x k) and compute inverse
    unsigned char *tempM = new unsigned char[k * k];
    for (int row = 0; row < k; row++) {
        int global_row = chosen[row];
        for (int col = 0; col < k; col++) {
            tempM[row * k + col] = gen_matrix[global_row * k + col];
        }
    }
    unsigned char *invM = new unsigned char[k * k];
    if (gf_invert_matrix(tempM, invM, k) != 0) {
        // unexpected: invert failed though rows are independent; return empty
        delete[] mat;
        delete[] tempM;
        delete[] invM;
        global_decode_block_indexes.clear();
        rows = cols = 0;
        return false;
    }

    // global_decode_block_indexes = chosen (sources)
    global_decode_block_indexes = chosen;

    // For each failed block, compute global coefficients c = G_row_failed * invM
    int F = static_cast<int>(failed_block_indexes.size());
    int K = static_cast<int>(global_decode_block_indexes.size());
    cols = 0;
    std::vector<unsigned char> full_coeffs(F * K);
    for (int f = 0; f < F; ++f) {
        int fidx = failed_block_indexes[f];
        unsigned char *coeff = new unsigned char[K];
        gf_mul_vect_matrix(gen_matrix + fidx * k, invM, coeff, K);
        for (int j = 0; j < K; ++j) {
            full_coeffs[f * K + j] = coeff[j];
        }
        delete[] coeff;
    }

    rows = R;

    // If caller requested a local matrix, project global coefficients onto its local sources
    if (local_source_block_ids != nullptr && local_matrix != nullptr) {
        cols = static_cast<int>(local_source_block_ids->size());
        for (int rr = 0; rr < R; ++rr) {
            const int want_bid = recovery_order[rr];
            int f_row = -1;
            for (int f = 0; f < F; ++f) {
                if (failed_block_indexes[f] == want_bid) {
                    f_row = f;
                    break;
                }
            }
            if (f_row < 0) {
                delete[] mat;
                delete[] tempM;
                delete[] invM;
                global_decode_block_indexes.clear();
                rows = cols = 0;
                return false;
            }
            for (int i = 0; i < cols; ++i) {
                int local_bid = (*local_source_block_ids)[i];
                int j_global = -1;
                for (int j = 0; j < K; ++j) {
                    if (global_decode_block_indexes[j] == local_bid) {
                        j_global = j;
                        break;
                    }
                }
                unsigned char val = 0;
                if (j_global != -1) {
                    val = full_coeffs[f_row * K + j_global];
                }
                local_matrix[rr * cols + i] = val;
            }
        }
    }

    delete[] mat;
    delete[] tempM;
    delete[] invM;
    return true;
}

namespace {

int local_recovery_block_num(const std::string &code_type)
{
    return (code_type == "LotusLRC") ? 2 : 1;
}

bool global_decode_plan_exists(int k, int r, int z, const std::string &code_type,
                               const std::vector<int> &all_failed,
                               const std::vector<int> &recover_ids)
{
    if (recover_ids.empty())
        return false;
    std::vector<int> decode_ids;
    int rows = 0, cols = 0;
    return get_global_decode_plan(k, r, z, code_type, all_failed, decode_ids, nullptr, nullptr,
                                  rows, cols, &recover_ids);
}

bool all_same_local_group(const std::string &code_type, int k, int r, int z,
                          const std::vector<int> &failed)
{
    if (failed.empty())
        return false;
    const int lg0 = get_block_id_to_local_group_id(code_type, k, r, z, failed[0]);
    for (size_t i = 1; i < failed.size(); ++i) {
        if (get_block_id_to_local_group_id(code_type, k, r, z, failed[i]) != lg0)
            return false;
    }
    return true;
}

std::vector<int> multi_recovery_batch(const std::string &code_type, int r,
                                      const std::vector<int> &failed_on_rack)
{
    (void)r;
    const size_t local_num = static_cast<size_t>(local_recovery_block_num(code_type));
    if (failed_on_rack.size() <= local_num)
        return {};
    return std::vector<int>(failed_on_rack.begin(),
                            failed_on_rack.end() - static_cast<std::ptrdiff_t>(local_num));
}

} // namespace

std::vector<RecoveryPhase> plan_multi_block_recovery(const std::string &code_type, int k, int r,
                                                       int z, const std::vector<int> &failed)
{
    std::vector<RecoveryPhase> phases;
    if (failed.empty())
        return phases;

    const size_t local_num = static_cast<size_t>(local_recovery_block_num(code_type));

    if (failed.size() == 1) {
        phases.push_back({RecoveryPhaseKind::SingleBlock, failed, failed});
        return phases;
    }

    if (failed.size() == 2) {
        if (!blocks_same_local_group(code_type, k, r, z, failed[0], failed[1])) {
            phases.push_back({RecoveryPhaseKind::SingleBlock, {failed[0]}, {failed[0]}});
            phases.push_back({RecoveryPhaseKind::SingleBlock, {failed[1]}, {failed[1]}});
            return phases;
        }
        if (code_type == "LotusLRC") {
            phases.push_back({RecoveryPhaseKind::GlobalMulti, failed, {}});
            return phases;
        }
        const int first = std::min(failed[0], failed[1]);
        const int second = std::max(failed[0], failed[1]);
        phases.push_back({RecoveryPhaseKind::GlobalMulti, failed, {first}});
        phases.push_back({RecoveryPhaseKind::SingleBlock, {second}, {second}});
        return phases;
    }

    if (!all_same_local_group(code_type, k, r, z, failed)) {
        phases.push_back({RecoveryPhaseKind::GlobalMulti, failed, failed});
        return phases;
    }

    const std::vector<int> std_batch = multi_recovery_batch(code_type, r, failed);
    if (!std_batch.empty() &&
        global_decode_plan_exists(k, r, z, code_type, failed, std_batch)) {
        phases.push_back({RecoveryPhaseKind::GlobalMulti, failed, std_batch});
        const std::vector<int> leftover(failed.begin() + static_cast<std::ptrdiff_t>(std_batch.size()),
                                        failed.end());
        if (code_type == "LotusLRC" && leftover.size() == 2) {
            phases.push_back({RecoveryPhaseKind::GlobalMulti, leftover, {}});
        } else {
            for (int bid : leftover)
                phases.push_back({RecoveryPhaseKind::SingleBlock, {bid}, {bid}});
        }
        return phases;
    }

    std::vector<int> data_failed;
    std::vector<int> parity_failed;
    data_failed.reserve(failed.size());
    parity_failed.reserve(failed.size());
    for (int bid : failed) {
        if (bid < k)
            data_failed.push_back(bid);
        else
            parity_failed.push_back(bid);
    }

    if (data_failed.size() >= 2) {
        if (global_decode_plan_exists(k, r, z, code_type, failed, data_failed)) {
            phases.push_back({RecoveryPhaseKind::GlobalMulti, failed, data_failed});
            for (int bid : parity_failed)
                phases.push_back({RecoveryPhaseKind::SingleBlock, {bid}, {bid}});
            return phases;
        }
        if (data_failed.size() > local_num) {
            const std::vector<int> data_batch(
                data_failed.begin(),
                data_failed.end() - static_cast<std::ptrdiff_t>(local_num));
            if (global_decode_plan_exists(k, r, z, code_type, failed, data_batch)) {
                phases.push_back({RecoveryPhaseKind::GlobalMulti, failed, data_batch});
                for (size_t i = data_failed.size() - local_num; i < data_failed.size(); ++i)
                    phases.push_back(
                        {RecoveryPhaseKind::SingleBlock, {data_failed[i]}, {data_failed[i]}});
                for (int bid : parity_failed)
                    phases.push_back({RecoveryPhaseKind::SingleBlock, {bid}, {bid}});
                return phases;
            }
        }
    }

    for (int bid : failed)
        phases.push_back({RecoveryPhaseKind::SingleBlock, {bid}, {bid}});
    return phases;
}

/* ----- Recovery group and block ids ----- */

std::vector<std::pair<int, std::vector<int>>> get_recovery_group_and_block_ids(const std::string &code_type, int k, int r, int z, int failed_block_id)
{
  if (code_type == "AzureLRC")
    return get_recovery_group_and_block_ids_azurelrc(k, r, z, failed_block_id);
  else if (code_type == "OptimalLRC")
    return get_recovery_group_and_block_ids_optimal_lrc(k, r, z, failed_block_id);
  else if (code_type == "UniformLRC")
    return get_recovery_group_and_block_ids_uniform_lrc(k, r, z, failed_block_id);
  else if (code_type == "UniLRC")
    return get_recovery_group_and_block_ids_unilrc(k, r, z, failed_block_id);
  else if (code_type == "LotusLRC")
    return get_recovery_group_and_block_ids_lotuslrc(k, r, z, failed_block_id);
  else
    throw std::runtime_error("unknown code type");
}

std::vector<std::pair<int, std::vector<int>>> get_recovery_group_and_block_ids_azurelrc(int k, int r, int z, int failed_block_id)
{
  std::vector<std::pair<int, std::vector<int>>> recovery_group_and_block_ids;
  std::vector<int> recovery_block_ids;
  if(failed_block_id < k)
  {
    int local_group_size = k / z;
    int local_group_id = failed_block_id / local_group_size;
    for (int i = local_group_id * local_group_size; i < (local_group_id + 1) * local_group_size; i++)
    {
      if (i != failed_block_id)
        recovery_block_ids.push_back(i);
    }
    recovery_block_ids.push_back(k + r + local_group_id); // plus one local parity block
  }
  else if(failed_block_id < k + r)
  {
    for(int i = r - 1; i < k + r; i++) // global parity blocks need global recovery;
    {
      if (i != failed_block_id)
        recovery_block_ids.push_back(i);
    }
  }
  else
  {
    int local_group_size = k / z;
    int local_group_id = failed_block_id - k - r;
    for (int i = local_group_id * local_group_size; i < (local_group_id + 1) * local_group_size; i++)
    {
      if (i != failed_block_id)
        recovery_block_ids.push_back(i);
    }
  }
  std::unordered_map<int, int> block_id_to_group_id = get_azurelrc_block_id_to_group_id(k, r, z);
  for (size_t i = 0; i < recovery_block_ids.size(); i++)
  {
    int gid = block_id_to_group_id[recovery_block_ids[i]];
    auto it = std::find_if(recovery_group_and_block_ids.begin(), recovery_group_and_block_ids.end(),
        [gid](const std::pair<int, std::vector<int>> &p) { return p.first == gid; });
    if (it == recovery_group_and_block_ids.end())
      recovery_group_and_block_ids.push_back({gid, {recovery_block_ids[i]}});
    else
      it->second.push_back(recovery_block_ids[i]);
  }
  return recovery_group_and_block_ids;
}

std::vector<std::pair<int, std::vector<int>>> get_recovery_group_and_block_ids_lotuslrc(int k, int r, int z, int failed_block_id)
{
  std::vector<std::pair<int, std::vector<int>>> recovery_group_and_block_ids;
  int local_group_id = get_lotuslrc_block_id_to_local_group_id(k, r, z, failed_block_id);
  std::vector<int> group_num_per_local_group = get_lotuslrc_group_num_per_local_group(k, r, z);
  std::vector<int> group_ids;
  int start_group_id = std::accumulate(group_num_per_local_group.begin(), group_num_per_local_group.begin() + local_group_id, 0);
  for (size_t i = 0; i < (size_t)group_num_per_local_group[local_group_id]; i++)
  {
    group_ids.push_back((int)(start_group_id + i));
  }
  std::unordered_map<int, std::vector<int>> group_id_to_block_ids = get_lotuslrc_group_id_to_block_ids(k, r, z);
  for (size_t i = 0; i < group_ids.size(); i++)
  {
    recovery_group_and_block_ids.push_back({group_ids[i], group_id_to_block_ids[group_ids[i]]});
  }
  // remove the failed block
  for (size_t i = 0; i < recovery_group_and_block_ids.size(); i++)
  {
    for (size_t j = 0; j < recovery_group_and_block_ids[i].second.size(); j++)
    {
      if (recovery_group_and_block_ids[i].second[j] == failed_block_id)
        recovery_group_and_block_ids[i].second.erase(recovery_group_and_block_ids[i].second.begin() + (std::ptrdiff_t)j);
    }
  }
  // remove one the last block of the last group because lotuslrc has two local parity blocks, if the last group is empty, remove the group
  if (recovery_group_and_block_ids.size() > 0)
  {
    if (recovery_group_and_block_ids[recovery_group_and_block_ids.size() - 1].second.size() > 0)
      recovery_group_and_block_ids[recovery_group_and_block_ids.size() - 1].second.pop_back();
    if (recovery_group_and_block_ids[recovery_group_and_block_ids.size() - 1].second.size() == 0)
      recovery_group_and_block_ids.erase(recovery_group_and_block_ids.begin() + (std::ptrdiff_t)recovery_group_and_block_ids.size() - 1);
  }
  return recovery_group_and_block_ids;
}

std::vector<std::pair<int, std::vector<int>>> get_recovery_group_and_block_ids_lotuslrc_2block_recovery(int k, int r, int z, int failed_block_id0, int failed_block_id1)
{
    std::vector<std::pair<int, std::vector<int>>> recovery_group_and_block_ids;
    int local_group_id = get_lotuslrc_block_id_to_local_group_id(k, r, z, failed_block_id0);
    int local_group_id1 = get_lotuslrc_block_id_to_local_group_id(k, r, z, failed_block_id1);
    if (local_group_id != local_group_id1)
    {
      throw std::runtime_error("failed blocks are not in the same local group");
    }
    std::vector<int> group_num_per_local_group = get_lotuslrc_group_num_per_local_group(k, r, z);
    std::vector<int> group_ids;
    int start_group_id = std::accumulate(group_num_per_local_group.begin(), group_num_per_local_group.begin() + local_group_id, 0);
    for (size_t i = 0; i < (size_t)group_num_per_local_group[local_group_id]; i++)
    {
      group_ids.push_back((int)(start_group_id + i));
    }
    std::unordered_map<int, std::vector<int>> group_id_to_block_ids = get_lotuslrc_group_id_to_block_ids(k, r, z);
    for (size_t i = 0; i < group_ids.size(); i++)
    {
      recovery_group_and_block_ids.push_back({group_ids[i], group_id_to_block_ids[group_ids[i]]});
    }
    // remove the failed block, if the failed block is the last block of the group, remove the group
    for (size_t i = 0; i < recovery_group_and_block_ids.size(); i++)
    {
      for (size_t j = 0; j < recovery_group_and_block_ids[i].second.size(); j++)
      {
        if (recovery_group_and_block_ids[i].second[j] == failed_block_id0 || recovery_group_and_block_ids[i].second[j] == failed_block_id1)
          recovery_group_and_block_ids[i].second.erase(recovery_group_and_block_ids[i].second.begin() + (std::ptrdiff_t)j);
      }
      if (recovery_group_and_block_ids[i].second.size() == 0)
        recovery_group_and_block_ids.erase(recovery_group_and_block_ids.begin() + (std::ptrdiff_t)i);
    }
    return recovery_group_and_block_ids;  
}

std::vector<std::pair<int, std::vector<int>>> get_recovery_group_and_block_ids_optimal_lrc(int k, int r, int z, int failed_block_id)
{
  std::vector<std::pair<int, std::vector<int>>> recovery_group_and_block_ids;
  std::vector<int> recovery_block_ids;
  if(failed_block_id < k)
  {
    int local_group_size = k / z;
    int local_group_id = failed_block_id / local_group_size;
    for (int i = local_group_id * local_group_size; i < (local_group_id + 1) * local_group_size; i++)
    {
      if (i != failed_block_id)
        recovery_block_ids.push_back(i);
    }
    recovery_block_ids.push_back(k + r + local_group_id); // plus one local parity block
    for(int i = k; i < k + r; i++)
    {
      recovery_block_ids.push_back(i); // optimal lrc need global parity blocks for local group recovery
    }
  }
  else if(failed_block_id < k + r)
  {
    // use the first local group for recovery
    for(int i = 0; i < k / z; i++)
    {
      recovery_block_ids.push_back(i);
    }
    recovery_block_ids.push_back(k + r); // plus one local parity block
    for(int i = k; i < k + r; i++)
    {
      if(i != failed_block_id)
      recovery_block_ids.push_back(i); // optimal lrc need global parity blocks for local group recovery
    }
  }
  else
  {
    int local_group_size = k / z;
    int local_group_id = failed_block_id - k - r;
    for (int i = local_group_id * local_group_size; i < (local_group_id + 1) * local_group_size; i++)
    {
      if (i != failed_block_id)
        recovery_block_ids.push_back(i);
    }
    for(int i = k; i < k + r; i++)
    {
      recovery_block_ids.push_back(i); // optimal lrc need global parity blocks for local group recovery
    }
  }
  std::unordered_map<int, int> block_id_to_group_id = get_optimal_lrc_block_id_to_group_id(k, r, z);
  for (size_t i = 0; i < recovery_block_ids.size(); i++)
  {
    int gid = block_id_to_group_id[recovery_block_ids[i]];
    auto it = std::find_if(recovery_group_and_block_ids.begin(), recovery_group_and_block_ids.end(),
        [gid](const std::pair<int, std::vector<int>> &p) { return p.first == gid; });
    if (it == recovery_group_and_block_ids.end())
      recovery_group_and_block_ids.push_back({gid, {recovery_block_ids[i]}});
    else
      it->second.push_back(recovery_block_ids[i]);
  }
  return recovery_group_and_block_ids;
}

std::vector<std::pair<int, std::vector<int>>> get_recovery_group_and_block_ids_uniform_lrc(int k, int r, int z, int failed_block_id)
{
  std::vector<std::pair<int, std::vector<int>>> recovery_group_and_block_ids;
  int local_group_id = get_uniform_lrc_block_id_to_local_group_id(k, r, z, failed_block_id);
  std::vector<int> group_num_per_local_group = get_uniform_lrc_group_num_per_local_group(k, r, z);
  std::vector<int> group_ids;
  int start_group_id = std::accumulate(group_num_per_local_group.begin(), group_num_per_local_group.begin() + local_group_id, 0);
  for (size_t i = 0; i < (size_t)group_num_per_local_group[local_group_id]; i++)
  {
    group_ids.push_back((int)(start_group_id + i));
  }
  std::unordered_map<int, std::vector<int>> group_id_to_block_ids = get_uniform_lrc_group_id_to_block_ids(k, r, z);
  for (size_t i = 0; i < group_ids.size(); i++)
  {
    recovery_group_and_block_ids.push_back({group_ids[i], group_id_to_block_ids[group_ids[i]]});
  }
  // remove the failed block, if the failed block is the last block of the group, remove the group
  for (size_t i = 0; i < recovery_group_and_block_ids.size(); i++)
  {
    for (size_t j = 0; j < recovery_group_and_block_ids[i].second.size(); j++)
    {
      if (recovery_group_and_block_ids[i].second[j] == failed_block_id)
        recovery_group_and_block_ids[i].second.erase(recovery_group_and_block_ids[i].second.begin() + (std::ptrdiff_t)j);
      if (recovery_group_and_block_ids[i].second.size() == 0)
        recovery_group_and_block_ids.erase(recovery_group_and_block_ids.begin() + (std::ptrdiff_t)i);
    }
  }
  return recovery_group_and_block_ids;
}

std::vector<std::pair<int, std::vector<int>>> get_recovery_group_and_block_ids_unilrc(int k, int r, int z, int failed_block_id)
{
  return {{0, {failed_block_id}}}; // TODO
}

} // namespace ECProject
