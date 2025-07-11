// Copyright (C) 2018-2025 Yixuan Qiu <yixuan.qiu@cos.name>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef SPECTRA_GEN_EIGS_BASE_H
#define SPECTRA_GEN_EIGS_BASE_H

#include <Eigen/Core>
#include <vector>     // std::vector
#include <cmath>      // std::abs, std::pow, std::sqrt
#include <algorithm>  // std::min, std::copy
#include <complex>    // std::complex, std::norm, std::abs
#include <stdexcept>  // std::invalid_argument

#include "Util/Version.h"
#include "Util/TypeTraits.h"
#include "Util/SelectionRule.h"
#include "Util/CompInfo.h"
#include "Util/SimpleRandom.h"
#include "MatOp/internal/ArnoldiOp.h"
#include "LinAlg/UpperHessenbergQR.h"
#include "LinAlg/DoubleShiftQR.h"
#include "LinAlg/UpperHessenbergEigen.h"
#include "LinAlg/Arnoldi.h"

namespace Spectra {

// Helper class to restart Arnoldi factorization
//
// Given the current upper Hessenberg matrix H and a number of
// shifts mu[1], ..., mu[n]:
//
// 1. Compute QR decomposition H - mu[i] * I = Qi * Ri
// 2. Update Q <- Q * Qi
// 3. Update H <- Qi^H * H * Qi
//
// The updated H has the same eigenvalues as the original one,
// but typically the new H is closer to a diagonal matrix
//
// Default implementation for real type
template <typename Scalar, typename ArnoldiFac>
class RestartArnoldi
{
private:
    using Index = Eigen::Index;
    using Complex = std::complex<Scalar>;
    using Matrix = Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic>;
    using ComplexVector = Eigen::Matrix<Complex, Eigen::Dynamic, 1>;

    // Real Ritz values calculated from UpperHessenbergEigen have exact zero imaginary part
    // Complex Ritz values have exact conjugate pairs
    // So we use exact tests here
    static bool is_complex(const Complex& v) { return v.imag() != Scalar(0); }
    static bool is_conj(const Complex& v1, const Complex& v2) { return v1 == Eigen::numext::conj(v2); }

public:
    // Shifts are ritz_val[k], ritz_val[k+1], ...
    static void run(const ComplexVector& ritz_val, Index k, ArnoldiFac& fac, Matrix& Q)
    {
        using std::norm;

        const Index ncv = ritz_val.size();
        DoubleShiftQR<Scalar> decomp_ds(ncv);
        UpperHessenbergQR<Scalar> decomp_hb(ncv);

        for (Index i = k; i < ncv; i++)
        {
            if (is_complex(ritz_val[i]) && is_conj(ritz_val[i], ritz_val[i + 1]))
            {
                // For real-valued H and two conjugate shifts mu, conj(mu),
                // H - mu * I = Q * R may be a complex-valued QR decomposition,
                // which is costly in computation
                //
                // Instead, we can apply two conjugate shifts simultaneously, i.e.,
                // (H - mu * I) * (H - conj(mu) * I) = Q * R, and then update
                // H <- Q'HQ
                //
                // This is called a double shift QR decomposition
                // (H - mu * I) * (H - conj(mu) * I) = H^2 - 2 * Re(mu) * H + |mu|^2 * I
                const Scalar s = Scalar(2) * ritz_val[i].real();
                const Scalar t = norm(ritz_val[i]);
                decomp_ds.compute(fac.matrix_H(), s, t);

                // Q <- Q * Qi
                decomp_ds.apply_YQ(Q);
                // H <- Qi' * H * Qi
                // Matrix Q = Matrix::Identity(ncv, ncv);
                // decomp_ds.apply_YQ(Q);
                // fac_H = Q.transpose() * fac_H * Q;
                fac.compress_H(decomp_ds);

                i++;
            }
            else
            {
                // QR decomposition of H - mu[i] * I, mu[i] is real
                decomp_hb.compute(fac.matrix_H(), ritz_val[i].real());

                // Q <- Q * Qi
                decomp_hb.apply_YQ(Q);
                // H <- Qi' * H * Qi = Ri * Qi + mu[i] * I
                fac.compress_H(decomp_hb);
            }
        }
    }
};

// Partial specialization for complex-valued matrices
template <typename RealScalar, typename ArnoldiFac>
class RestartArnoldi<std::complex<RealScalar>, ArnoldiFac>
{
private:
    using Index = Eigen::Index;
    using Scalar = std::complex<RealScalar>;
    using Complex = Scalar;
    using Matrix = Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic>;
    using ComplexVector = Eigen::Matrix<Complex, Eigen::Dynamic, 1>;

public:
    static void run(const ComplexVector& ritz_val, Index k, ArnoldiFac& fac, Matrix& Q)
    {
        const Index ncv = ritz_val.size();
        // This is the complex-valued QR decomposition
        UpperHessenbergQR<Scalar> decomp_hb(ncv);

        // For complex-valued H, simply apply complex shifts mu[i] one by one
        for (Index i = k; i < ncv; i++)
        {
            // QR decomposition of H - mu[i] * I
            decomp_hb.compute(fac.matrix_H(), ritz_val[i]);

            // Q <- Q * Qi
            decomp_hb.apply_YQ(Q);
            // H <- Qi^H * H * Qi = Ri * Qi + mu[i] * I
            fac.compress_H(decomp_hb);
        }
    }
};

///
/// \ingroup EigenSolver
///
/// This is the base class for general eigen solvers, mainly for internal use.
/// It is kept here to provide the documentation for member functions of concrete eigen solvers
/// such as GenEigsSolver and GenEigsRealShiftSolver.
///
template <typename OpType, typename BOpType>
class GenEigsBase
{
private:
    // Scalar is the type of the matrix element
    // Can be real or complex
    using Scalar = typename OpType::Scalar;
    // The real part type of the matrix element, e.g.,
    //     Scalar = double               => RealScalar = double
    //     Scalar = std::complex<double> => RealScalar = double
    using RealScalar = typename Eigen::NumTraits<Scalar>::Real;
    // The eigenvalues are known to be complex numbers
    using Complex = std::complex<RealScalar>;
    using Index = Eigen::Index;
    using Matrix = Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic>;
    using Vector = Eigen::Matrix<Scalar, Eigen::Dynamic, 1>;
    using ComplexMatrix = Eigen::Matrix<Complex, Eigen::Dynamic, Eigen::Dynamic>;
    using ComplexVector = Eigen::Matrix<Complex, Eigen::Dynamic, 1>;
    using RealArray = Eigen::Array<RealScalar, Eigen::Dynamic, 1>;
    using BoolArray = Eigen::Array<bool, Eigen::Dynamic, 1>;
    using MapMat = Eigen::Map<Matrix>;
    using MapVec = Eigen::Map<Vector>;
    using MapConstVec = Eigen::Map<const Vector>;

    using ArnoldiOpType = ArnoldiOp<OpType, BOpType>;
    using ArnoldiFac = Arnoldi<ArnoldiOpType>;

protected:
    // clang-format off
    OpType&       m_op;        // object to conduct matrix operation,
                               // e.g. matrix-vector product
    const Index   m_n;         // dimension of matrix A
    const Index   m_nev;       // number of eigenvalues requested
    const Index   m_ncv;       // dimension of Krylov subspace in the Arnoldi method
    Index         m_nmatop;    // number of matrix operations called
    Index         m_niter;     // number of restarting iterations

    ArnoldiFac    m_fac;       // Arnoldi factorization

    ComplexVector m_ritz_val;  // Ritz values
    ComplexMatrix m_ritz_vec;  // Ritz vectors
    ComplexVector m_ritz_est;  // last row of m_ritz_vec, also called the Ritz estimates

private:
    BoolArray     m_ritz_conv; // indicator of the convergence of Ritz values
    CompInfo      m_info;      // status of the computation
    // clang-format on

    // Real Ritz values calculated from UpperHessenbergEigen have exact zero imaginary part
    // Complex Ritz values have exact conjugate pairs
    // So we use exact tests here
    static bool is_complex(const Complex& v) { return v.imag() != Scalar(0); }
    static bool is_conj(const Complex& v1, const Complex& v2) { return v1 == Eigen::numext::conj(v2); }

    // Implicitly restarted Arnoldi factorization
    void restart(Index k, SortRule selection)
    {
        using std::norm;

        if (k >= m_ncv)
            return;

        // Use Q to collect orthogonal transformations
        Matrix Q = Matrix::Identity(m_ncv, m_ncv);
        // Apply shifts and update H and Q
        RestartArnoldi<Scalar, ArnoldiFac>::run(m_ritz_val, k, m_fac, Q);
        // Apply orthogonal transformation to V, V <- V * Q
        m_fac.compress_V(Q);
        // It can be verified that the updated V and H admit a k-step
        // Arnoldi factorization, and we expand it to m-step
        m_fac.factorize_from(k, m_ncv, m_nmatop);
        // Retrieve the new Ritz pairs
        retrieve_ritzpair(selection);
    }

    // Calculates the number of converged Ritz values
    Index num_converged(const RealScalar& tol)
    {
        using std::pow;

        // The machine precision, ~= 1e-16 for the "double" type
        const RealScalar eps = TypeTraits<RealScalar>::epsilon();
        // std::pow() is not constexpr, so we do not declare eps23 to be constexpr
        // But most compilers should be able to compute eps23 at compile time
        const RealScalar eps23 = pow(eps, RealScalar(2) / 3);

        // thresh = tol * max(eps23, abs(theta)), theta for Ritz value
        RealArray thresh = tol * m_ritz_val.head(m_nev).array().abs().max(eps23);
        RealArray resid = m_ritz_est.head(m_nev).array().abs() * m_fac.f_norm();
        // Converged "wanted" Ritz values
        m_ritz_conv = (resid < thresh);

        return m_ritz_conv.count();
    }

    // Returns the adjusted nev for restarting
    Index nev_adjusted(Index nconv)
    {
        using std::abs;

        // A very small value, but 1.0 / near_0 does not overflow
        // ~= 1e-307 for the "double" type
        const RealScalar near_0 = TypeTraits<RealScalar>::min() * RealScalar(10);

        Index nev_new = m_nev;
        for (Index i = m_nev; i < m_ncv; i++)
            if (abs(m_ritz_est[i]) < near_0)
                nev_new++;

        // Adjust nev_new, according to dnaup2.f line 660~674 in ARPACK
        nev_new += (std::min)(nconv, (m_ncv - nev_new) / 2);
        if (nev_new == 1 && m_ncv >= 6)
            nev_new = m_ncv / 2;
        else if (nev_new == 1 && m_ncv > 3)
            nev_new = 2;

        if (nev_new > m_ncv - 2)
            nev_new = m_ncv - 2;

        // Increase nev by one if ritz_val[nev - 1] and
        // ritz_val[nev] are conjugate pairs
        if (is_complex(m_ritz_val[nev_new - 1]) &&
            is_conj(m_ritz_val[nev_new - 1], m_ritz_val[nev_new]))
        {
            nev_new++;
        }

        return nev_new;
    }

    // Retrieves and sorts Ritz values and Ritz vectors
    void retrieve_ritzpair(SortRule selection)
    {
        UpperHessenbergEigen<Scalar> decomp(m_fac.matrix_H());
        const ComplexVector& evals = decomp.eigenvalues();
        ComplexMatrix evecs = decomp.eigenvectors();

        // Sort Ritz values and put the wanted ones at the beginning
        std::vector<Index> ind;
        switch (selection)
        {
            case SortRule::LargestMagn:
            {
                SortEigenvalue<Complex, SortRule::LargestMagn> sorting(evals.data(), m_ncv);
                sorting.swap(ind);
                break;
            }
            case SortRule::LargestReal:
            {
                SortEigenvalue<Complex, SortRule::LargestReal> sorting(evals.data(), m_ncv);
                sorting.swap(ind);
                break;
            }
            case SortRule::LargestImag:
            {
                SortEigenvalue<Complex, SortRule::LargestImag> sorting(evals.data(), m_ncv);
                sorting.swap(ind);
                break;
            }
            case SortRule::SmallestMagn:
            {
                SortEigenvalue<Complex, SortRule::SmallestMagn> sorting(evals.data(), m_ncv);
                sorting.swap(ind);
                break;
            }
            case SortRule::SmallestReal:
            {
                SortEigenvalue<Complex, SortRule::SmallestReal> sorting(evals.data(), m_ncv);
                sorting.swap(ind);
                break;
            }
            case SortRule::SmallestImag:
            {
                SortEigenvalue<Complex, SortRule::SmallestImag> sorting(evals.data(), m_ncv);
                sorting.swap(ind);
                break;
            }
            default:
                throw std::invalid_argument("unsupported selection rule");
        }

        // Copy the Ritz values and vectors to m_ritz_val and m_ritz_vec, respectively
        for (Index i = 0; i < m_ncv; i++)
        {
            m_ritz_val[i] = evals[ind[i]];
            m_ritz_est[i] = evecs(m_ncv - 1, ind[i]);
        }
        for (Index i = 0; i < m_nev; i++)
        {
            m_ritz_vec.col(i).noalias() = evecs.col(ind[i]);
        }
    }

protected:
    // Sorts the first nev Ritz pairs in the specified order
    // This is used to return the final results
    virtual void sort_ritzpair(SortRule sort_rule)
    {
        std::vector<Index> ind;
        switch (sort_rule)
        {
            case SortRule::LargestMagn:
            {
                SortEigenvalue<Complex, SortRule::LargestMagn> sorting(m_ritz_val.data(), m_nev);
                sorting.swap(ind);
                break;
            }
            case SortRule::LargestReal:
            {
                SortEigenvalue<Complex, SortRule::LargestReal> sorting(m_ritz_val.data(), m_nev);
                sorting.swap(ind);
                break;
            }
            case SortRule::LargestImag:
            {
                SortEigenvalue<Complex, SortRule::LargestImag> sorting(m_ritz_val.data(), m_nev);
                sorting.swap(ind);
                break;
            }
            case SortRule::SmallestMagn:
            {
                SortEigenvalue<Complex, SortRule::SmallestMagn> sorting(m_ritz_val.data(), m_nev);
                sorting.swap(ind);
                break;
            }
            case SortRule::SmallestReal:
            {
                SortEigenvalue<Complex, SortRule::SmallestReal> sorting(m_ritz_val.data(), m_nev);
                sorting.swap(ind);
                break;
            }
            case SortRule::SmallestImag:
            {
                SortEigenvalue<Complex, SortRule::SmallestImag> sorting(m_ritz_val.data(), m_nev);
                sorting.swap(ind);
                break;
            }
            default:
                throw std::invalid_argument("unsupported sorting rule");
        }

        ComplexVector new_ritz_val(m_ncv);
        ComplexMatrix new_ritz_vec(m_ncv, m_nev);
        BoolArray new_ritz_conv(m_nev);

        for (Index i = 0; i < m_nev; i++)
        {
            new_ritz_val[i] = m_ritz_val[ind[i]];
            new_ritz_vec.col(i).noalias() = m_ritz_vec.col(ind[i]);
            new_ritz_conv[i] = m_ritz_conv[ind[i]];
        }

        m_ritz_val.swap(new_ritz_val);
        m_ritz_vec.swap(new_ritz_vec);
        m_ritz_conv.swap(new_ritz_conv);
    }

public:
    /// \cond

    GenEigsBase(OpType& op, const BOpType& Bop, Index nev, Index ncv) :
        m_op(op),
        m_n(m_op.rows()),
        m_nev(nev),
        m_ncv(ncv > m_n ? m_n : ncv),
        m_nmatop(0),
        m_niter(0),
        m_fac(ArnoldiOpType(op, Bop), m_ncv),
        m_info(CompInfo::NotComputed)
    {
        if (nev < 1 || nev > m_n - 2)
            throw std::invalid_argument("nev must satisfy 1 <= nev <= n - 2, n is the size of matrix");

        if (ncv < nev + 2 || ncv > m_n)
            throw std::invalid_argument("ncv must satisfy nev + 2 <= ncv <= n, n is the size of matrix");
    }

    ///
    /// Virtual destructor
    ///
    virtual ~GenEigsBase() {}

    /// \endcond

    ///
    /// Initializes the solver by providing an initial residual vector.
    ///
    /// \param init_resid Pointer to the initial residual vector.
    ///
    /// **Spectra** (and also **ARPACK**) uses an iterative algorithm
    /// to find eigenvalues. This function allows the user to provide the initial
    /// residual vector.
    ///
    void init(const Scalar* init_resid)
    {
        // Reset all matrices/vectors to zero
        m_ritz_val.resize(m_ncv);
        m_ritz_vec.resize(m_ncv, m_nev);
        m_ritz_est.resize(m_ncv);
        m_ritz_conv.resize(m_nev);

        m_ritz_val.setZero();
        m_ritz_vec.setZero();
        m_ritz_est.setZero();
        m_ritz_conv.setZero();

        m_nmatop = 0;
        m_niter = 0;

        // Initialize the Arnoldi factorization
        MapConstVec v0(init_resid, m_n);
        m_fac.init(v0, m_nmatop);
    }

    ///
    /// Initializes the solver by providing a random initial residual vector.
    ///
    /// This overloaded function generates a random initial residual vector
    /// (with a fixed random seed) for the algorithm. Elements in the vector
    /// follow independent Uniform(-0.5, 0.5) distribution.
    ///
    void init()
    {
        SimpleRandom<Scalar> rng(0);
        Vector init_resid = rng.random_vec(m_n);
        init(init_resid.data());
    }

    ///
    /// Conducts the major computation procedure.
    ///
    /// \param selection  An enumeration value indicating the selection rule of
    ///                   the requested eigenvalues, for example `SortRule::LargestMagn`
    ///                   to retrieve eigenvalues with the largest magnitude.
    ///                   The full list of enumeration values can be found in
    ///                   \ref Enumerations.
    /// \param maxit      Maximum number of iterations allowed in the algorithm.
    /// \param tol        Precision parameter for the calculated eigenvalues.
    /// \param sorting    Rule to sort the eigenvalues and eigenvectors.
    ///                   Supported values are
    ///                   `SortRule::LargestMagn`, `SortRule::LargestReal`,
    ///                   `SortRule::LargestImag`, `SortRule::SmallestMagn`,
    ///                   `SortRule::SmallestReal` and `SortRule::SmallestImag`,
    ///                   for example `SortRule::LargestMagn` indicates that eigenvalues
    ///                   with largest magnitude come first.
    ///                   Note that this argument is only used to
    ///                   **sort** the final result, and the **selection** rule
    ///                   (e.g. selecting the largest or smallest eigenvalues in the
    ///                   full spectrum) is specified by the parameter `selection`.
    ///
    /// \return Number of converged eigenvalues.
    ///
    Index compute(SortRule selection = SortRule::LargestMagn, Index maxit = 1000,
                  RealScalar tol = 1e-10, SortRule sorting = SortRule::LargestMagn)
    {
        // The m-step Arnoldi factorization
        m_fac.factorize_from(1, m_ncv, m_nmatop);
        retrieve_ritzpair(selection);
        // Restarting
        Index i, nconv = 0, nev_adj;
        for (i = 0; i < maxit; i++)
        {
            nconv = num_converged(tol);
            if (nconv >= m_nev)
                break;

            nev_adj = nev_adjusted(nconv);
            restart(nev_adj, selection);
        }
        // Sorting results
        sort_ritzpair(sorting);

        m_niter += (i + 1);
        m_info = (nconv >= m_nev) ? CompInfo::Successful : CompInfo::NotConverging;

        return (std::min)(m_nev, nconv);
    }

    ///
    /// Returns the status of the computation.
    /// The full list of enumeration values can be found in \ref Enumerations.
    ///
    CompInfo info() const { return m_info; }

    ///
    /// Returns the number of iterations used in the computation.
    ///
    Index num_iterations() const { return m_niter; }

    ///
    /// Returns the number of matrix operations used in the computation.
    ///
    Index num_operations() const { return m_nmatop; }

    ///
    /// Returns the converged eigenvalues.
    ///
    /// \return A complex-valued vector containing the eigenvalues.
    /// Returned vector type will be `Eigen::Vector<std::complex<Scalar>, ...>`, depending on
    /// the template parameter `Scalar` defined.
    ///
    ComplexVector eigenvalues() const
    {
        const Index nconv = m_ritz_conv.cast<Index>().sum();
        ComplexVector res(nconv);

        if (!nconv)
            return res;

        Index j = 0;
        for (Index i = 0; i < m_nev; i++)
        {
            if (m_ritz_conv[i])
            {
                res[j] = m_ritz_val[i];
                j++;
            }
        }

        return res;
    }

    ///
    /// Returns the eigenvectors associated with the converged eigenvalues.
    ///
    /// \param nvec The number of eigenvectors to return.
    ///
    /// \return A complex-valued matrix containing the eigenvectors.
    /// Returned matrix type will be `Eigen::Matrix<std::complex<Scalar>, ...>`,
    /// depending on the template parameter `Scalar` defined.
    ///
    ComplexMatrix eigenvectors(Index nvec) const
    {
        const Index nconv = m_ritz_conv.cast<Index>().sum();
        nvec = (std::min)(nvec, nconv);
        ComplexMatrix res(m_n, nvec);

        if (!nvec)
            return res;

        ComplexMatrix ritz_vec_conv(m_ncv, nvec);
        Index j = 0;
        for (Index i = 0; i < m_nev && j < nvec; i++)
        {
            if (m_ritz_conv[i])
            {
                ritz_vec_conv.col(j).noalias() = m_ritz_vec.col(i);
                j++;
            }
        }

        res.noalias() = m_fac.matrix_V() * ritz_vec_conv;

        return res;
    }

    ///
    /// Returns all converged eigenvectors.
    ///
    ComplexMatrix eigenvectors() const
    {
        return eigenvectors(m_nev);
    }
};

}  // namespace Spectra

#endif  // SPECTRA_GEN_EIGS_BASE_H
