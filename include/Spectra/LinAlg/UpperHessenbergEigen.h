// The code was adapted from Eigen/src/Eigenvaleus/EigenSolver.h
// and Eigen/src/Eigenvaleus/ComplexEigenSolver.h
//
// Copyright (C) 2008 Gael Guennebaud <gael.guennebaud@inria.fr>
// Copyright (C) 2010,2012 Jitse Niesen <jitse@maths.leeds.ac.uk>
// Copyright (C) 2016-2025 Yixuan Qiu <yixuan.qiu@cos.name>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef SPECTRA_UPPER_HESSENBERG_EIGEN_H
#define SPECTRA_UPPER_HESSENBERG_EIGEN_H

#include <Eigen/Core>
#include <Eigen/Eigenvalues>
#include <cmath>      // std::sqrt, std::abs
#include <complex>    // std::complex
#include <algorithm>  // std::max, std::swap
#include <stdexcept>  // std::invalid_argument, std::logic_error, std::runtime_error

#include "../Util/TypeTraits.h"
#include "UpperHessenbergSchur.h"

namespace Spectra {

// Default implementation for real-valued matrices
//
// Modified from Eigen/src/Eigenvaleus/EigenSolver.h
// The main change in this version is to assume that the input matrix
// is upper Hessenberg, and in general it is faster than Eigen::EigenSolver
template <typename Scalar = double>
class UpperHessenbergEigen
{
private:
    using Index = Eigen::Index;
    using Matrix = Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic>;
    using Vector = Eigen::Matrix<Scalar, Eigen::Dynamic, 1>;
    using GenericMatrix = Eigen::Ref<Matrix>;
    using ConstGenericMatrix = const Eigen::Ref<const Matrix>;

    using Complex = std::complex<Scalar>;
    using ComplexMatrix = Eigen::Matrix<Complex, Eigen::Dynamic, Eigen::Dynamic>;
    using ComplexVector = Eigen::Matrix<Complex, Eigen::Dynamic, 1>;

    Index m_n;                             // Size of the matrix
    UpperHessenbergSchur<Scalar> m_schur;  // Schur decomposition solver
    Matrix m_matT;                         // Schur T matrix
    Matrix m_eivec;                        // Storing eigenvectors
    ComplexVector m_eivalues;              // Eigenvalues
    bool m_computed;

    void doComputeEigenvectors()
    {
        using std::abs;

        const Index size = m_eivec.cols();
        const Scalar eps = Eigen::NumTraits<Scalar>::epsilon();

        // inefficient! this is already computed in RealSchur
        Scalar norm(0);
        for (Index j = 0; j < size; ++j)
        {
            norm += m_matT.row(j).segment((std::max)(j - 1, Index(0)), size - (std::max)(j - 1, Index(0))).cwiseAbs().sum();
        }

        // Backsubstitute to find vectors of upper triangular form
        if (norm == Scalar(0))
            return;

        for (Index n = size - 1; n >= 0; n--)
        {
            Scalar p = m_eivalues.coeff(n).real();
            Scalar q = m_eivalues.coeff(n).imag();

            // Scalar vector
            if (q == Scalar(0))
            {
                Scalar lastr(0), lastw(0);
                Index l = n;

                m_matT.coeffRef(n, n) = Scalar(1);
                for (Index i = n - 1; i >= 0; i--)
                {
                    Scalar w = m_matT.coeff(i, i) - p;
                    Scalar r = m_matT.row(i).segment(l, n - l + 1).dot(m_matT.col(n).segment(l, n - l + 1));

                    if (m_eivalues.coeff(i).imag() < Scalar(0))
                    {
                        lastw = w;
                        lastr = r;
                    }
                    else
                    {
                        l = i;
                        if (m_eivalues.coeff(i).imag() == Scalar(0))
                        {
                            if (w != Scalar(0))
                                m_matT.coeffRef(i, n) = -r / w;
                            else
                                m_matT.coeffRef(i, n) = -r / (eps * norm);
                        }
                        else  // Solve real equations
                        {
                            Scalar x = m_matT.coeff(i, i + 1);
                            Scalar y = m_matT.coeff(i + 1, i);
                            Scalar denom = (m_eivalues.coeff(i).real() - p) * (m_eivalues.coeff(i).real() - p) + m_eivalues.coeff(i).imag() * m_eivalues.coeff(i).imag();
                            Scalar t = (x * lastr - lastw * r) / denom;
                            m_matT.coeffRef(i, n) = t;
                            if (abs(x) > abs(lastw))
                                m_matT.coeffRef(i + 1, n) = (-r - w * t) / x;
                            else
                                m_matT.coeffRef(i + 1, n) = (-lastr - y * t) / lastw;
                        }

                        // Overflow control
                        Scalar t = abs(m_matT.coeff(i, n));
                        if ((eps * t) * t > Scalar(1))
                            m_matT.col(n).tail(size - i) /= t;
                    }
                }
            }
            else if (q < Scalar(0) && n > 0)
            {  // Complex vector
                Scalar lastra(0), lastsa(0), lastw(0);
                Index l = n - 1;

                // Last vector component imaginary so matrix is triangular
                if (abs(m_matT.coeff(n, n - 1)) > abs(m_matT.coeff(n - 1, n)))
                {
                    m_matT.coeffRef(n - 1, n - 1) = q / m_matT.coeff(n, n - 1);
                    m_matT.coeffRef(n - 1, n) = -(m_matT.coeff(n, n) - p) / m_matT.coeff(n, n - 1);
                }
                else
                {
                    Complex cc = Complex(Scalar(0), -m_matT.coeff(n - 1, n)) / Complex(m_matT.coeff(n - 1, n - 1) - p, q);
                    m_matT.coeffRef(n - 1, n - 1) = Eigen::numext::real(cc);
                    m_matT.coeffRef(n - 1, n) = Eigen::numext::imag(cc);
                }
                m_matT.coeffRef(n, n - 1) = Scalar(0);
                m_matT.coeffRef(n, n) = Scalar(1);
                for (Index i = n - 2; i >= 0; i--)
                {
                    Scalar ra = m_matT.row(i).segment(l, n - l + 1).dot(m_matT.col(n - 1).segment(l, n - l + 1));
                    Scalar sa = m_matT.row(i).segment(l, n - l + 1).dot(m_matT.col(n).segment(l, n - l + 1));
                    Scalar w = m_matT.coeff(i, i) - p;

                    if (m_eivalues.coeff(i).imag() < Scalar(0))
                    {
                        lastw = w;
                        lastra = ra;
                        lastsa = sa;
                    }
                    else
                    {
                        l = i;
                        if (m_eivalues.coeff(i).imag() == Scalar(0))
                        {
                            Complex cc = Complex(-ra, -sa) / Complex(w, q);
                            m_matT.coeffRef(i, n - 1) = Eigen::numext::real(cc);
                            m_matT.coeffRef(i, n) = Eigen::numext::imag(cc);
                        }
                        else
                        {
                            // Solve complex equations
                            Scalar x = m_matT.coeff(i, i + 1);
                            Scalar y = m_matT.coeff(i + 1, i);
                            Scalar vr = (m_eivalues.coeff(i).real() - p) * (m_eivalues.coeff(i).real() - p) + m_eivalues.coeff(i).imag() * m_eivalues.coeff(i).imag() - q * q;
                            Scalar vi = (m_eivalues.coeff(i).real() - p) * Scalar(2) * q;
                            if ((vr == Scalar(0)) && (vi == Scalar(0)))
                                vr = eps * norm * (abs(w) + abs(q) + abs(x) + abs(y) + abs(lastw));

                            Complex cc = Complex(x * lastra - lastw * ra + q * sa, x * lastsa - lastw * sa - q * ra) / Complex(vr, vi);
                            m_matT.coeffRef(i, n - 1) = Eigen::numext::real(cc);
                            m_matT.coeffRef(i, n) = Eigen::numext::imag(cc);
                            if (abs(x) > (abs(lastw) + abs(q)))
                            {
                                m_matT.coeffRef(i + 1, n - 1) = (-ra - w * m_matT.coeff(i, n - 1) + q * m_matT.coeff(i, n)) / x;
                                m_matT.coeffRef(i + 1, n) = (-sa - w * m_matT.coeff(i, n) - q * m_matT.coeff(i, n - 1)) / x;
                            }
                            else
                            {
                                cc = Complex(-lastra - y * m_matT.coeff(i, n - 1), -lastsa - y * m_matT.coeff(i, n)) / Complex(lastw, q);
                                m_matT.coeffRef(i + 1, n - 1) = Eigen::numext::real(cc);
                                m_matT.coeffRef(i + 1, n) = Eigen::numext::imag(cc);
                            }
                        }

                        // Overflow control
                        Scalar t = (std::max)(abs(m_matT.coeff(i, n - 1)), abs(m_matT.coeff(i, n)));
                        if ((eps * t) * t > Scalar(1))
                            m_matT.block(i, n - 1, size - i, 2) /= t;
                    }
                }

                // We handled a pair of complex conjugate eigenvalues, so need to skip them both
                n--;
            }
        }

        // Back transformation to get eigenvectors of original matrix
        Vector m_tmp(size);
        for (Index j = size - 1; j >= 0; j--)
        {
            m_tmp.noalias() = m_eivec.leftCols(j + 1) * m_matT.col(j).segment(0, j + 1);
            m_eivec.col(j) = m_tmp;
        }
    }

public:
    UpperHessenbergEigen() :
        m_n(0), m_computed(false)
    {}

    UpperHessenbergEigen(ConstGenericMatrix& mat) :
        m_n(mat.rows()), m_computed(false)
    {
        compute(mat);
    }

    void compute(ConstGenericMatrix& mat)
    {
        using std::abs;
        using std::sqrt;

        if (mat.rows() != mat.cols())
            throw std::invalid_argument("UpperHessenbergEigen: matrix must be square");

        m_n = mat.rows();
        // Scale matrix prior to the Schur decomposition
        const Scalar scale = mat.cwiseAbs().maxCoeff();

        // Reduce to real Schur form
        m_schur.compute(mat / scale);
        m_schur.swap_T(m_matT);
        m_schur.swap_U(m_eivec);

        // Compute eigenvalues from matT
        m_eivalues.resize(m_n);
        Index i = 0;
        while (i < m_n)
        {
            // Real eigenvalue
            if (i == m_n - 1 || m_matT.coeff(i + 1, i) == Scalar(0))
            {
                m_eivalues.coeffRef(i) = m_matT.coeff(i, i);
                ++i;
            }
            else  // Complex eigenvalues
            {
                Scalar p = Scalar(0.5) * (m_matT.coeff(i, i) - m_matT.coeff(i + 1, i + 1));
                Scalar z;
                // Compute z = sqrt(abs(p * p + m_matT.coeff(i+1, i) * m_matT.coeff(i, i+1)));
                // without overflow
                {
                    Scalar t0 = m_matT.coeff(i + 1, i);
                    Scalar t1 = m_matT.coeff(i, i + 1);
                    Scalar maxval = (std::max)(abs(p), (std::max)(abs(t0), abs(t1)));
                    t0 /= maxval;
                    t1 /= maxval;
                    Scalar p0 = p / maxval;
                    z = maxval * sqrt(abs(p0 * p0 + t0 * t1));
                }
                m_eivalues.coeffRef(i) = Complex(m_matT.coeff(i + 1, i + 1) + p, z);
                m_eivalues.coeffRef(i + 1) = Complex(m_matT.coeff(i + 1, i + 1) + p, -z);
                i += 2;
            }
        }

        // Compute eigenvectors
        doComputeEigenvectors();

        // Scale eigenvalues back
        m_eivalues *= scale;

        m_computed = true;
    }

    const ComplexVector& eigenvalues() const
    {
        if (!m_computed)
            throw std::logic_error("UpperHessenbergEigen: need to call compute() first");

        return m_eivalues;
    }

    ComplexMatrix eigenvectors() const
    {
        using std::abs;

        if (!m_computed)
            throw std::logic_error("UpperHessenbergEigen: need to call compute() first");

        Index n = m_eivec.cols();
        ComplexMatrix matV(n, n);
        for (Index j = 0; j < n; ++j)
        {
            // imaginary part of real eigenvalue is already set to exact zero
            if (Eigen::numext::imag(m_eivalues.coeff(j)) == Scalar(0) || j + 1 == n)
            {
                // we have a real eigen value
                matV.col(j) = m_eivec.col(j).template cast<Complex>();
                matV.col(j).normalize();
            }
            else
            {
                // we have a pair of complex eigen values
                for (Index i = 0; i < n; ++i)
                {
                    matV.coeffRef(i, j) = Complex(m_eivec.coeff(i, j), m_eivec.coeff(i, j + 1));
                    matV.coeffRef(i, j + 1) = Complex(m_eivec.coeff(i, j), -m_eivec.coeff(i, j + 1));
                }
                matV.col(j).normalize();
                matV.col(j + 1).normalize();
                ++j;
            }
        }

        return matV;
    }
};

// Partial specialization for complex-valued matrices
//
// Modified from Eigen/src/Eigenvaleus/ComplexEigenSolver.h
// The main change in this version is to assume that the input matrix
// is upper Hessenberg, and in general it is faster than Eigen::ComplexEigenSolver
template <typename RealScalar>
class UpperHessenbergEigen<std::complex<RealScalar>>
{
private:
    using Scalar = std::complex<RealScalar>;
    using Index = Eigen::Index;
    using Matrix = Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic>;
    using Vector = Eigen::Matrix<Scalar, Eigen::Dynamic, 1>;
    using ConstGenericMatrix = const Eigen::Ref<const Matrix>;
    using Complex = Scalar;
    using ComplexMatrix = Matrix;
    using ComplexVector = Vector;

    Index m_n;                            // Size of the matrix
    Eigen::ComplexSchur<Matrix> m_schur;  // Schur decomposition solver
    ComplexMatrix m_eivec;                // Eigenvectors
    ComplexVector m_eivalues;             // Eigenvalues
    bool m_computed;

    void doComputeEigenvectors(RealScalar matrixnorm)
    {
        const Index n = m_eivalues.size();

        matrixnorm = (std::max)(matrixnorm, TypeTraits<RealScalar>::min());

        // Compute X such that T = X D X^(-1), where D is the diagonal of T
        // The matrix X is unit triangular
        ComplexMatrix m_matX = ComplexMatrix::Zero(n, n);
        for (Index k = n - 1; k >= 0; k--)
        {
            m_matX.coeffRef(k, k) = Complex(1.0, 0.0);
            // Compute X(i,k) using the (i,k) entry of the equation X T = D X
            for (Index i = k - 1; i >= 0; i--)
            {
                m_matX.coeffRef(i, k) = -m_schur.matrixT().coeff(i, k);
                if (k - i - 1 > 0)
                    m_matX.coeffRef(i, k) -= (m_schur.matrixT().row(i).segment(i + 1, k - i - 1) * m_matX.col(k).segment(i + 1, k - i - 1)).value();
                Complex z = m_schur.matrixT().coeff(i, i) - m_schur.matrixT().coeff(k, k);
                if (z == Complex(0))
                {
                    // If the i-th and k-th eigenvalue are equal, then z equals 0.
                    // Use a small value instead, to prevent division by zero.
                    Eigen::numext::real_ref(z) = TypeTraits<RealScalar>::epsilon() * matrixnorm;
                }
                m_matX.coeffRef(i, k) = m_matX.coeff(i, k) / z;
            }
        }

        // Compute V as V = U X; now A = U T U^* = U X D X^(-1) U^* = V D V^(-1)
        m_eivec.noalias() = m_schur.matrixU() * m_matX;
        // .. and normalize the eigenvectors
        for (Index k = 0; k < n; k++)
        {
            m_eivec.col(k).normalize();
        }
    }

    void sortEigenvalues(bool computeEigenvectors)
    {
        const Index n = m_eivalues.size();
        for (Index i = 0; i < n; i++)
        {
            Index k;
            m_eivalues.cwiseAbs().tail(n - i).minCoeff(&k);
            if (k != 0)
            {
                k += i;
                std::swap(m_eivalues[k], m_eivalues[i]);
                if (computeEigenvectors)
                    m_eivec.col(i).swap(m_eivec.col(k));
            }
        }
    }

public:
    UpperHessenbergEigen() :
        m_n(0), m_computed(false)
    {}

    UpperHessenbergEigen(ConstGenericMatrix& mat) :
        m_n(mat.rows()), m_computed(false)
    {
        compute(mat);
    }

    void compute(ConstGenericMatrix& mat)
    {
        if (mat.rows() != mat.cols())
            throw std::invalid_argument("UpperHessenbergEigen: matrix must be square");

        // Do a complex Schur decomposition, A = U T U^*
        // The eigenvalues are on the diagonal of T
        // A is known to be upper Hessenberg
        m_eivec.resize(mat.rows(), mat.cols());
        m_eivec.setIdentity();
        m_schur.computeFromHessenberg(mat, m_eivec, true);

        if (m_schur.info() == Eigen::Success)
        {
            m_eivalues = m_schur.matrixT().diagonal();
            doComputeEigenvectors(m_schur.matrixT().norm());
            sortEigenvalues(true);
        }
        else
        {
            throw std::runtime_error("UpperHessenbergEigen: eigen decomposition failed");
        }

        m_computed = true;
    }

    const ComplexVector& eigenvalues() const
    {
        if (!m_computed)
            throw std::logic_error("UpperHessenbergEigen: need to call compute() first");

        return m_eivalues;
    }

    const ComplexMatrix& eigenvectors() const
    {
        if (!m_computed)
            throw std::logic_error("UpperHessenbergEigen: need to call compute() first");

        return m_eivec;
    }
};

}  // namespace Spectra

#endif  // SPECTRA_UPPER_HESSENBERG_EIGEN_H
