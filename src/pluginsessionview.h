#ifndef PLUGINSESSIONVIEW_H
#define PLUGINSESSIONVIEW_H

#include <QString>
#include <QVariant>
#include <QVector>

#include "engine/evaluationcontext.h"

namespace FlySight {

/// What a Python plugin's compute() receives as its "session": a read-only
/// view over the inputs the engine resolved for this one evaluation.
///
/// A plugin runs inside an evaluation, where inputs are already resolved in the
/// EvaluationContext and ordinary SessionData reads are forbidden, so the view
/// wraps the context rather than the session. Reading anything the plugin did
/// not return from inputs() is an undeclared read: it yields nothing and makes
/// the plugin's result unavailable.
///
/// The host calls invalidate() as soon as compute() returns. A plugin that
/// keeps the object gets an inert view, never a dangling pointer.
///
/// Header-only, Qt Core + the evaluation context only: it is compiled into both
/// the application and the bridge module, and must never reach the calculation
/// registry or create a session (each binary has its own copy of those statics).
class PluginSessionView {
public:
    explicit PluginSessionView(const EvaluationContext *ctx) : m_ctx(ctx) {}

    void invalidate() { m_ctx = nullptr; }

    QVector<double> getMeasurement(const QString &sensor, const QString &name) const
    {
        return m_ctx ? m_ctx->measurement(sensor, name) : QVector<double>();
    }

    QVariant getAttribute(const QString &key) const
    {
        return m_ctx ? m_ctx->attribute(key) : QVariant();
    }

    bool hasMeasurement(const QString &sensor, const QString &name) const
    {
        return !getMeasurement(sensor, name).isEmpty();
    }

    bool hasAttribute(const QString &key) const
    {
        return getAttribute(key).isValid();
    }

private:
    const EvaluationContext *m_ctx;
};

} // namespace FlySight

#endif // PLUGINSESSIONVIEW_H
