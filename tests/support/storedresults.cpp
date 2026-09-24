#include "storedresults.h"

using namespace FlySight;

namespace FlySightTest {

StoredResolution storedResolution(const DependencyKey &name, StoredResolution::Provider provider,
                                  const QString &instanceId, const QString &resultVersion)
{
    StoredResolution r;
    r.name = name;
    r.provider = provider;
    r.instanceId = instanceId;
    r.resultVersion = resultVersion;
    return r;
}

PluginCodeIngredients standInPluginIngredients(const QByteArray &aPlugin)
{
    PluginCodeIngredients i;
    i.files = {PluginSourceFile{QStringLiteral("a_plugin.py"), aPlugin},
               PluginSourceFile{QStringLiteral("helper.py"), QByteArray("y = 2\n")}};
    i.sdk = QByteArray("sdk");
    i.pythonVersion = QStringLiteral("3.13.3");
    i.numpyVersion = QStringLiteral("2.2.4");
    return i;
}

QString standInPluginIdentity(const QByteArray &aPlugin)
{
    return pluginCodeIdentity(standInPluginIngredients(aPlugin));
}

} // namespace FlySightTest
