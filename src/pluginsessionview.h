#ifndef PLUGINSESSIONVIEW_H
#define PLUGINSESSIONVIEW_H

#include <stdexcept>
#include <string>

#include <QString>
#include <QVariant>
#include <QVector>

#include "engine/evaluationcontext.h"

namespace FlySight {

/// Raised (as flysight_cpp_bridge.UndeclaredInputError in Python) when a plugin
/// reads a key it did not return from inputs().
struct UndeclaredInputError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Raised (as RuntimeError in Python) when the view is used after the
/// compute() call it was created for has ended.
struct StaleSessionViewError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// What a Python plugin's compute() receives as its "session": a read-only
/// view over the inputs the engine resolved for this one evaluation.
///
/// A plugin runs inside an evaluation, where inputs are already resolved in the
/// EvaluationContext and ordinary SessionData reads are forbidden, so the view
/// wraps the context rather than the session.
///
/// Reads are effective values only (getMeasurement / getAttribute /
/// effectiveUnit / has*): what every other consumer sees. The key must be a
/// declared attr() / meas() input. The source layer is not reachable from a
/// plugin; only the conversion layer reads it.
///
/// Reading anything the plugin did not declare is an error that is visible in
/// Python: the evaluation is flagged first (so the engine discards whatever
/// compute() returns, even if the plugin swallows the exception) and then
/// UndeclaredInputError is thrown, naming the plugin, the key, and the fix.
///
/// The host calls invalidate() as soon as compute() returns. A plugin that
/// keeps the object gets StaleSessionViewError, never a dangling pointer.
///
/// Header-only, Qt Core + the evaluation context only: it is compiled into both
/// the application and the bridge module, and must never reach the calculation
/// registry or create a session (each binary has its own copy of those statics).
class PluginSessionView {
public:
    /// `pluginLabel` is "<calculation id> (<module>.<qualname>)".
    PluginSessionView(const EvaluationContext *ctx, const QString &pluginLabel)
        : m_ctx(ctx), m_label(pluginLabel) {}

    void invalidate() { m_ctx = nullptr; }

    QVector<double> getMeasurement(const QString &sensor, const QString &name) const
    {
        requireMeasurement(sensor, name);
        return m_ctx->measurement(sensor, name);
    }

    QVariant getAttribute(const QString &key) const
    {
        requireAttribute(key);
        return m_ctx->attribute(key);
    }

    QString effectiveUnit(const QString &sensor, const QString &name) const
    {
        requireMeasurement(sensor, name);
        return m_ctx->measurementUnit(sensor, name);
    }

    /// True for a declared input: compute() runs only when every declared
    /// input is available.
    bool hasMeasurement(const QString &sensor, const QString &name) const
    {
        requireMeasurement(sensor, name);
        return true;
    }

    bool hasAttribute(const QString &key) const
    {
        requireAttribute(key);
        return true;
    }

private:
    void requireLive() const
    {
        if (!m_ctx)
            throw StaleSessionViewError("SessionData is only valid inside compute()");
    }

    void requireAttribute(const QString &key) const
    {
        requireLive();
        if (m_ctx->isDeclared(CalcInput::attribute(key)))
            return;
        (void)m_ctx->attribute(key);        // flags the evaluation as an undeclared read
        throw UndeclaredInputError(message(QStringLiteral("attribute"), key,
                                           QStringLiteral("attr('%1')").arg(key)));
    }

    void requireMeasurement(const QString &sensor, const QString &name) const
    {
        requireLive();
        if (m_ctx->isDeclared(CalcInput::measurement(sensor, name)))
            return;
        (void)m_ctx->measurement(sensor, name);
        throw UndeclaredInputError(message(QStringLiteral("measurement"), sensor + QLatin1Char('/') + name,
                                           QStringLiteral("meas('%1', '%2')").arg(sensor, name)));
    }

    std::string message(const QString &kind, const QString &key, const QString &fix) const
    {
        return QStringLiteral("%1 read undeclared %2 %3; add %4 to inputs()")
            .arg(m_label, kind, key, fix).toStdString();
    }

    const EvaluationContext *m_ctx;
    QString m_label;
};

} // namespace FlySight

#endif // PLUGINSESSIONVIEW_H
