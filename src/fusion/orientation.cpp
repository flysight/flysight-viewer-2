#include "fusion/orientation.h"

using namespace FlySight::Fusion;

Orientation Orientation::defaultOrientation()
{
    return Orientation(Axis::PlusY, Axis::PlusZ);
}

// Built once, on first use (a function-local static is initialized once even
// when two threads get here together), and never changed.
const std::vector<Orientation> &Orientation::all()
{
    static const std::vector<Orientation> orientations = [] {
        constexpr Axis axes[] = { Axis::PlusX, Axis::MinusX, Axis::PlusY,
                                  Axis::MinusY, Axis::PlusZ, Axis::MinusZ };
        const Orientation first = defaultOrientation();
        std::vector<Orientation> list{ first };
        for (const Axis forward : axes) {
            for (const Axis up : axes) {
                const Orientation candidate(forward, up);
                if (int(forward) / 2 != int(up) / 2 && candidate != first)
                    list.push_back(candidate);
            }
        }
        return list;
    }();
    return orientations;
}

// By lookup in the enumeration, so that exactly its 24 tokens parse
std::optional<Orientation> Orientation::fromToken(const QString &token)
{
    for (const Orientation &orientation : all()) {
        if (orientation.token() == token)
            return orientation;
    }
    return std::nullopt;
}

QString Orientation::token() const
{
    return axisText(m_forward) + QLatin1Char(',') + axisText(m_up);
}

QString Orientation::label() const
{
    return QStringLiteral("forward ") + axisText(m_forward) + QStringLiteral(", up ") + axisText(m_up);
}

// Integer arithmetic, so every entry is exactly -1, 0 or 1 (and no zero is
// negative)
Orientation::Matrix Orientation::bodyToDevice() const
{
    const std::array<int, 3> f = axisVector(m_forward);
    const std::array<int, 3> u = axisVector(m_up);
    const std::array<int, 3> right = { f[1]*u[2] - f[2]*u[1],
                                       f[2]*u[0] - f[0]*u[2],
                                       f[0]*u[1] - f[1]*u[0] };
    const std::array<int, 3> down = { -u[0], -u[1], -u[2] };

    Matrix m{};
    for (int row = 0; row < 3; ++row) {
        m[row][0] = f[row];
        m[row][1] = right[row];
        m[row][2] = down[row];
    }
    return m;
}

QString Orientation::axisText(Axis axis)
{
    const int index = int(axis);
    return QString(QLatin1Char(index % 2 == 0 ? '+' : '-')) + QLatin1Char("xyz"[index / 2]);
}

std::array<int, 3> Orientation::axisVector(Axis axis)
{
    const int index = int(axis);
    std::array<int, 3> v = { 0, 0, 0 };
    v[index / 2] = index % 2 == 0 ? 1 : -1;
    return v;
}
