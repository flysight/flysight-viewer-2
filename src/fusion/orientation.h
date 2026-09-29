#ifndef FLYSIGHT_FUSION_ORIENTATION_H
#define FLYSIGHT_FUSION_ORIENTATION_H

#include <array>
#include <optional>
#include <vector>

#include <QString>

namespace FlySight::Fusion {

/// How a FlySight is mounted on the body whose attitude is reported: which
/// device axis points forward and which points up. Each is one of the six
/// signed device axes, and the two are neither equal nor opposite, so there
/// are exactly 24 orientations. Forward and up fix the body frame of forward,
/// right and down: right is forward x up, down is the opposite of up. The
/// orientation describes the mount and nothing else; it says nothing about
/// the posture of whoever wears the device.
///
/// This class is the one place that spells the vocabulary. A token is
/// `<forward>,<up>`, each axis written as a sign and a lower-case letter with
/// the ASCII hyphen-minus for minus; a label is `forward <forward>, up <up>`
/// in the same axis text. Labels are not translated: they name axes. The
/// orientation attribute's choices, its constant default and the attitude
/// derivation's parser all come from here, so nothing else spells a token.
///
/// Pure: Qt Core and the standard library only, no logging, no state beyond
/// the two axes. Safe to use from any thread.
class Orientation {
public:
    /// A 3x3 matrix, row-major: m[row][column].
    using Matrix = std::array<std::array<double, 3>, 3>;

    /// Forward +y, up +z: a FlySight 2 on the back of a helmet, label up.
    static Orientation defaultOrientation();

    /// The 24 orientations: the default first, then the others in the
    /// enumeration order. Forward runs through +x, -x, +y, -y, +z, -z, and
    /// for each forward, up runs through the same order, skipping forward's
    /// axis.
    static const std::vector<Orientation> &all();

    /// The orientation whose token is exactly `token`; nothing for any other
    /// text (another spelling, spaces, upper case, a label, an empty string,
    /// forward equal or opposite to up).
    static std::optional<Orientation> fromToken(const QString &token);

    QString token() const;
    QString label() const;

    /// The rotation from the body frame to the device frame: its columns are
    /// the forward, right and down axes in device coordinates. A signed
    /// permutation, exact (every entry -1, 0 or 1) and a proper rotation.
    Matrix bodyToDevice() const;

    bool operator==(const Orientation &other) const
    {
        return m_forward == other.m_forward && m_up == other.m_up;
    }
    bool operator!=(const Orientation &other) const { return !(*this == other); }

private:
    // In the enumeration order; an axis's index / 2 is its coordinate, and an
    // even index is the positive direction.
    enum class Axis { PlusX, MinusX, PlusY, MinusY, PlusZ, MinusZ };

    Orientation(Axis forward, Axis up) : m_forward(forward), m_up(up) {}

    static QString axisText(Axis axis);
    static std::array<int, 3> axisVector(Axis axis);

    Axis m_forward;
    Axis m_up;
};

} // namespace FlySight::Fusion

#endif // FLYSIGHT_FUSION_ORIENTATION_H
