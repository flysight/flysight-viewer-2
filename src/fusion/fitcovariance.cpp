#include "fusion/fitcovariance.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Eigen/QR>

#include <gtsam/geometry/Pose3.h>
#include <gtsam/inference/Ordering.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/linear/GaussianBayesTree.h>
#include <gtsam/linear/GaussianFactorGraph.h>
#include <gtsam/linear/JacobianFactor.h>

namespace FlySight::Fusion::Detail {

using gtsam::symbol_shorthand::B;
using gtsam::symbol_shorthand::S;
using gtsam::symbol_shorthand::T;
using gtsam::symbol_shorthand::V;
using gtsam::symbol_shorthand::X;

const char kCovarianceFailure[] = "covariance unavailable: the factorization of the converged graph failed";

namespace {

// The globals g = (B(0), T(0), S(0)): each key with its offset in g and its
// dimension.
struct Global { gtsam::Key key; int offset, dimension; };
const Global kGlobals[] = {{B(0), 0, 6}, {T(0), 6, 3}, {S(0), 9, 6}};

/// The failed result: no block, the fixed text.
FitCovariance failed()
{
    FitCovariance c;
    c.failure = kCovarianceFailure;
    return c;
}

/// The linearized converged graph with the heading prior on X(0): a 1x6 row on
/// its tangent, the navigation-frame vertical component of the body-frame
/// rotation (e_D^T R_0 phi), over kHeadingPriorSigmaRad. In this graph only:
/// the fit's own graph never sees it.
gtsam::GaussianFactorGraph linearizedWithHeadingPrior(const FitResult &fit)
{
    gtsam::GaussianFactorGraph linear = *fit.graph.linearize(fit.values);
    const gtsam::Matrix3 R0 = fit.values.at<gtsam::Pose3>(X(0)).rotation().matrix();
    gtsam::Matrix A = gtsam::Matrix::Zero(1, 6);
    A.leftCols<3>() = (R0.transpose()*gtsam::Vector3::UnitZ()).transpose()/kHeadingPriorSigmaRad;
    linear.push_back(std::make_shared<gtsam::JacobianFactor>(X(0), A, gtsam::Vector1::Zero()));
    return linear;
}

/// The joint covariance of the keys of `marginal` in the order `keys`:
/// R^-1 R^-T of the QR of its whitened Jacobian. A rank-deficient marginal
/// gives a non-finite result, which the caller refuses.
gtsam::Matrix jointCovariance(const gtsam::GaussianFactorGraph &marginal, const gtsam::KeyVector &keys)
{
    const gtsam::Matrix A = marginal.jacobian(gtsam::Ordering(keys)).first;
    const Eigen::Index d = A.cols();
    const Eigen::HouseholderQR<gtsam::Matrix> qr(A);
    const gtsam::Matrix R = qr.matrixQR().topRows(d).triangularView<Eigen::Upper>();
    const gtsam::Matrix Rinv = R.triangularView<Eigen::Upper>().solve(gtsam::Matrix::Identity(d, d));
    return Rinv*Rinv.transpose();
}

/// A clique's joint covariance, with each key's offset and dimension in it.
struct CliqueCovariance {
    gtsam::Matrix covariance;
    std::unordered_map<gtsam::Key, std::pair<Eigen::Index, Eigen::Index>> block;   ///< offset, dimension

    gtsam::Matrix get(gtsam::Key a, gtsam::Key b) const
    {
        const auto &[ra, da] = block.at(a);
        const auto &[rb, db] = block.at(b);
        return covariance.block(ra, rb, da, db);
    }
    bool has(gtsam::Key key) const { return block.count(key) != 0; }
};

/// The 9x9 block of two fix states (x = pose then velocity) from a clique.
gtsam::Matrix9 stateBlock(const CliqueCovariance &c, size_t a, size_t b)
{
    gtsam::Matrix9 m;
    m.block<6, 6>(0, 0) = c.get(X(a), X(b));
    m.block<6, 3>(0, 6) = c.get(X(a), V(b));
    m.block<3, 6>(6, 0) = c.get(V(a), X(b));
    m.block<3, 3>(6, 6) = c.get(V(a), V(b));
    return m;
}

/// The 9x15 block of a fix state and the globals from a clique.
Matrix9x15 globalBlock(const CliqueCovariance &c, size_t k)
{
    Matrix9x15 m;
    for (const Global &g : kGlobals) {
        m.block(0, g.offset, 6, g.dimension) = c.get(X(k), g.key);
        m.block(6, g.offset, 3, g.dimension) = c.get(V(k), g.key);
    }
    return m;
}

bool allFinite(const FitCovariance &c)
{
    const auto finite = [](const auto &matrices) {
        return std::all_of(matrices.begin(), matrices.end(), [](const auto &m) { return m.allFinite(); });
    };
    return finite(c.node) && finite(c.next) && finite(c.global) && c.globals.allFinite();
}

bool positiveDiagonals(const FitCovariance &c)
{
    for (const gtsam::Matrix9 &m : c.node) {
        if (!(m.diagonal().array() > 0).all())
            return false;
    }
    return (c.globals.diagonal().array() > 0).all();
}

/// The step itself; throws what linearization or elimination throws.
FitCovariance compute(const FitResult &fit, size_t states)
{
    const gtsam::GaussianFactorGraph linear = linearizedWithHeadingPrior(fit);

    // The chain ordering, the globals last: the Bayes tree is a path.
    gtsam::KeyVector keys;
    for (size_t k = 0; k < states; ++k) {
        keys.push_back(X(k));
        keys.push_back(V(k));
    }
    keys.push_back(B(0));
    keys.push_back(T(0));
    keys.push_back(S(0));
    std::unordered_map<gtsam::Key, size_t> position;
    for (size_t i = 0; i < keys.size(); ++i)
        position[keys[i]] = i;

    const std::shared_ptr<gtsam::GaussianBayesTree> tree =
        linear.eliminateMultifrontal(gtsam::Ordering(keys), gtsam::EliminateQR);

    FitCovariance c;
    c.node.resize(states);
    c.next.resize(states-1);
    c.global.resize(states);
    std::vector<bool> filled(states, false);
    bool globalsFilled = false;

    // Root first, breadth first: every clique's parent has cached its
    // separator marginal before the clique asks for it, so each marginal is
    // one level of work and the recursion never deepens.
    std::vector<gtsam::GaussianBayesTree::sharedClique> order(tree->roots().begin(), tree->roots().end());
    for (size_t i = 0; i < order.size(); ++i)
        order.insert(order.end(), order[i]->children.begin(), order[i]->children.end());
    for (const gtsam::GaussianBayesTree::sharedClique &clique : order) {
        const gtsam::GaussianConditional &conditional = *clique->conditional();
        gtsam::KeyVector cliqueKeys(conditional.begin(), conditional.end());
        std::sort(cliqueKeys.begin(), cliqueKeys.end(),
                  [&](gtsam::Key a, gtsam::Key b) { return position.at(a) < position.at(b); });
        CliqueCovariance cc;
        Eigen::Index offset = 0;
        for (gtsam::Key key : cliqueKeys) {
            const Eigen::Index dimension = Eigen::Index(fit.values.at(key).dim());
            cc.block[key] = {offset, dimension};
            offset += dimension;
        }
        cc.covariance = jointCovariance(clique->marginal2(gtsam::EliminateQR), cliqueKeys);

        // Each clique gives the blocks of its frontal fix states, and the
        // root the globals'.
        for (auto frontal = conditional.beginFrontals(); frontal != conditional.endFrontals(); ++frontal) {
            const gtsam::Symbol symbol(*frontal);
            if (*frontal == B(0)) {
                if (!cc.has(T(0)) || !cc.has(S(0)))
                    return failed();
                for (const Global &a : kGlobals) {
                    for (const Global &b : kGlobals)
                        c.globals.block(a.offset, b.offset, a.dimension, b.dimension) = cc.get(a.key, b.key);
                }
                globalsFilled = true;
            }
            if (symbol.chr() != 'x')
                continue;
            const size_t k = size_t(symbol.index());
            const bool last = k+1 == states;
            if (k >= states || !cc.has(V(k)) || !cc.has(B(0)) || !cc.has(T(0)) || !cc.has(S(0))
                || (!last && (!cc.has(X(k+1)) || !cc.has(V(k+1)))))
                return failed();
            c.node[k] = stateBlock(cc, k, k);
            if (!last)
                c.next[k] = stateBlock(cc, k, k+1);
            c.global[k] = globalBlock(cc, k);
            filled[k] = true;
        }
    }
    tree->deleteCachedShortcuts();

    if (!globalsFilled || std::find(filled.begin(), filled.end(), false) != filled.end())
        return failed();
    c.computed = true;
    return c;
}

} // namespace

Matrix33 FitCovariance::pair(size_t k) const
{
    Matrix33 z;
    z.block<9, 9>(0, 0) = node[k];
    z.block<9, 9>(0, 9) = next[k];
    z.block<9, 9>(9, 0) = next[k].transpose();
    z.block<9, 9>(9, 9) = node[k+1];
    z.block<9, 15>(0, 18) = global[k];
    z.block<15, 9>(18, 0) = global[k].transpose();
    z.block<9, 15>(9, 18) = global[k+1];
    z.block<15, 9>(18, 9) = global[k+1].transpose();
    z.block<15, 15>(18, 18) = globals;
    return z;
}

FitCovariance fitCovariance(const FitResult &fit, size_t states)
{
    // A failure is a result: the fit converged, and only its accuracy is
    // lost. Memory exhaustion is not a function of the inputs and is never
    // turned into one.
    try {
        FitCovariance c = compute(fit, states);
        if (c.computed && (!allFinite(c) || !positiveDiagonals(c)))
            return failed();
        return c;
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::exception &) {
        return failed();
    }
}

AttitudeAccuracy attitudeAccuracy(const gtsam::Matrix3 &navigation)
{
    const auto degrees = [](double variance) {
        if (!std::isfinite(variance) || variance < 0)
            return kYawSigmaCapDeg;
        return std::min(std::sqrt(variance)*180/kPi, kYawSigmaCapDeg);
    };
    return {degrees(navigation(2, 2)), degrees(navigation(0, 0)+navigation(1, 1))};
}

AccelerationAccuracy accelerationAccuracy(const gtsam::Matrix9 &joint, const gtsam::Rot3 &attitude,
                                          const gtsam::Vector3 &reading, const gtsam::Vector3 &accScale,
                                          const gtsam::Vector3 &accBias, const gtsam::Vector3 &published,
                                          double sampleSigma)
{
    const gtsam::Matrix3 R = attitude.matrix();
    // The attitude error into the navigation frame: phi^n = R phi.
    gtsam::Matrix9 toNavigation = gtsam::Matrix9::Identity();
    toNavigation.block<3, 3>(0, 0) = R;
    gtsam::Matrix9 q = toNavigation*joint*toNavigation.transpose();
    // An undetermined heading enters at most at the cap's variance, pi^2.
    if (q(2, 2) > kPi*kPi) {
        const double shrink = kPi/std::sqrt(q(2, 2));
        q.row(2) *= shrink;
        q.col(2) *= shrink;
    }

    const gtsam::Vector3 u = reading.cwiseQuotient(accScale)-accBias;
    Eigen::Matrix<double, 3, 9> L;
    L.block<3, 3>(0, 0) = -gtsam::skewSymmetric(R*u);
    L.block<3, 3>(0, 3) = -R;
    L.block<3, 3>(0, 6) = -R*reading.cwiseQuotient(accScale.cwiseProduct(accScale)).asDiagonal();
    const gtsam::Matrix3 a = L*q*L.transpose()+sampleSigma*sampleSigma*gtsam::Matrix3::Identity();

    // Along the published horizontal acceleration where it is at least that
    // accuracy; otherwise the larger horizontal principal value, which an
    // undetermined heading inflates only across the horizontal force.
    const double hn = a(0, 0), he = a(1, 1), hne = a(0, 1);
    const double largest = (hn+he)/2+std::sqrt((hn-he)*(hn-he)/4+hne*hne);
    double horizontal = std::sqrt(largest);
    const double magnitude = std::hypot(published.x(), published.y());
    if (magnitude > 0) {
        const double n = published.x()/magnitude, e = published.y()/magnitude;
        const double along = std::sqrt(n*n*hn+2*n*e*hne+e*e*he);
        if (magnitude >= along)
            horizontal = along;
    }
    return {horizontal, std::sqrt(a(2, 2))};
}

std::vector<double> wideningFactors(const std::vector<double> &gnssTime,
                                    const std::vector<FactorResidual> &residuals,
                                    const std::vector<double> &times)
{
    const size_t n = gnssTime.size();
    // Prefix sums over the fixes of the GNSS residuals (position and
    // velocity at fix k) and of the IMU residuals (the interval that ends at
    // fix k); the priors are not the window's.
    std::vector<double> gnss(n+1, 0), imu(n+1, 0);
    for (const FactorResidual &r : residuals) {
        if (r.kind == "position" || r.kind == "velocity")
            gnss[r.node+1] += r.squaredWhitenedError;
        else if (r.kind == "imu")
            imu[r.node+1] += r.squaredWhitenedError;
    }
    for (size_t k = 0; k < n; ++k) {
        gnss[k+1] += gnss[k];
        imu[k+1] += imu[k];
    }

    std::vector<double> factors;
    factors.reserve(times.size());
    for (const double t : times) {
        // The fix at or before the sample, and the one after it.
        const size_t around = std::min(
            size_t(std::max<std::ptrdiff_t>(std::upper_bound(gnssTime.begin(), gnssTime.end(), t)-gnssTime.begin()-1, 0)),
            n-2);
        size_t lo = size_t(std::lower_bound(gnssTime.begin(), gnssTime.end(), t-kWideningHalfWidthS)-gnssTime.begin());
        size_t hi = size_t(std::upper_bound(gnssTime.begin(), gnssTime.end(), t+kWideningHalfWidthS)-gnssTime.begin());
        hi = hi ? hi-1 : 0;
        lo = std::min(lo, around);
        hi = std::max(hi, around+1);
        const double fixes = double(hi-lo+1);
        const double sum = (gnss[hi+1]-gnss[lo])+(imu[hi+1]-imu[lo+1]);
        factors.push_back(sum/(6*fixes-9));
    }
    return factors;
}

double widening(double factor)
{
    return factor > 1 ? std::sqrt(factor) : 1.;
}

} // namespace FlySight::Fusion::Detail
