#include "ProjectConfig.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QVariant>

#include <cmath>

namespace wehypr
{

namespace
{
constexpr qint64 k_maxProjectFileSize = 4 * 1024 * 1024;
constexpr int    k_maxProperties      = 512;
constexpr int    k_maxTextLength      = 4096;

bool isInside(const QString& rootDir, const QString& path) {
    if (rootDir.isEmpty() || path.isEmpty()) return false;
    const QString prefix =
        rootDir.endsWith(QLatin1Char('/')) ? rootDir : rootDir + QLatin1Char('/');
    return path.startsWith(prefix);
}

std::optional<Project> fail(QString* error, const QString& message) {
    *error = message;
    return std::nullopt;
}

bool valueMatches(const QJsonObject& definition, const QJsonValue& value, QString* reason) {
    const QString type = definition.value(QLatin1String("type")).toString().toLower();

    if (type == QLatin1String("slider")) {
        if (! value.isDouble() || ! std::isfinite(value.toDouble())) {
            *reason = QStringLiteral("expected a number");
            return false;
        }
        const double     number = value.toDouble();
        const QJsonValue min    = definition.value(QLatin1String("min"));
        const QJsonValue max    = definition.value(QLatin1String("max"));
        if (min.isDouble() && number < min.toDouble()) {
            *reason = QStringLiteral("%1 is below the minimum %2").arg(number).arg(min.toDouble());
            return false;
        }
        if (max.isDouble() && number > max.toDouble()) {
            *reason = QStringLiteral("%1 is above the maximum %2").arg(number).arg(max.toDouble());
            return false;
        }
        return true;
    }
    if (type == QLatin1String("bool")) {
        if (value.isBool()) return true;
        *reason = QStringLiteral("expected true or false");
        return false;
    }
    if (type == QLatin1String("color")) {
        const QStringList parts = value.toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        bool              valid = value.isString() && (parts.size() == 3 || parts.size() == 4);
        for (const QString& part : parts) {
            bool         ok        = false;
            const double component = part.toDouble(&ok);
            valid                  = valid && ok && std::isfinite(component);
        }
        if (! valid) *reason = QStringLiteral("expected a color string like \"0.5 0.2 1\"");
        return valid;
    }
    if (type == QLatin1String("combo")) {
        if (! value.isString() && ! value.isDouble()) {
            *reason = QStringLiteral("expected a string or number");
            return false;
        }
        const QJsonArray options = definition.value(QLatin1String("options")).toArray();
        if (options.isEmpty()) return true;
        const QString text = value.toVariant().toString();
        for (const QJsonValue& option : options) {
            const QJsonValue optionValue = option.toObject().value(QLatin1String("value"));
            if (optionValue == value || optionValue.toVariant().toString() == text) return true;
        }
        *reason = QStringLiteral("'%1' is not one of the combo options").arg(text);
        return false;
    }
    if (type == QLatin1String("textinput")) {
        if (value.isString() && value.toString().size() <= k_maxTextLength) return true;
        *reason = QStringLiteral("expected a string of at most %1 characters").arg(k_maxTextLength);
        return false;
    }
    if (type == QLatin1String("text") || type == QLatin1String("group")) {
        *reason = QStringLiteral("properties of type '%1' have no value").arg(type);
        return false;
    }
    if (type == QLatin1String("file") || type == QLatin1String("directory") ||
        type == QLatin1String("scenetexture")) {
        *reason = QStringLiteral("properties of type '%1' cannot be changed").arg(type);
        return false;
    }

    const QJsonValue current = definition.value(QLatin1String("value"));
    if (current.isUndefined() || current.isNull() || current.isArray() || current.isObject()) {
        *reason = QStringLiteral("property has no scalar value that could be changed");
        return false;
    }
    if (current.type() != value.type()) {
        *reason = QStringLiteral("expected the same JSON type as the current value");
        return false;
    }
    return true;
}
} // namespace

std::optional<Project> loadProject(const QString& path, QString* error) {
    QFileInfo info(path);
    if (! info.exists())
        return fail(error, QStringLiteral("project path does not exist: %1").arg(path));
    if (info.isDir())
        info = QFileInfo(QDir(info.absoluteFilePath()).filePath(QStringLiteral("project.json")));
    if (! info.isFile())
        return fail(error,
                    QStringLiteral("project.json not found: %1").arg(info.absoluteFilePath()));
    if (info.size() > k_maxProjectFileSize)
        return fail(
            error,
            QStringLiteral("project file is larger than 4 MiB: %1").arg(info.absoluteFilePath()));

    QFile file(info.absoluteFilePath());
    if (! file.open(QIODevice::ReadOnly))
        return fail(error,
                    QStringLiteral("cannot read %1: %2").arg(file.fileName(), file.errorString()));
    QByteArray data = file.read(k_maxProjectFileSize + 1);
    if (data.startsWith("\xEF\xBB\xBF")) data.remove(0, 3);

    QJsonParseError parseError;
    const auto      doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return fail(error,
                    QStringLiteral("%1 is not valid JSON: %2")
                        .arg(file.fileName(), parseError.errorString()));
    if (! doc.isObject())
        return fail(error, QStringLiteral("%1 must contain a JSON object").arg(file.fileName()));
    const QJsonObject root = doc.object();

    Project project;
    project.projectFile = info.canonicalFilePath();
    project.rootDir     = info.absoluteDir().canonicalPath();

    project.type = root.value(QLatin1String("type")).toString().toLower();
    if (project.type.isEmpty()) return fail(error, QStringLiteral("project.json has no type"));
    if (project.type == QLatin1String("scene") || project.type == QLatin1String("video"))
        return fail(error,
                    QStringLiteral("%1 projects are not supported by the Hyprland host yet; "
                                   "only web projects are")
                        .arg(project.type));
    if (project.type != QLatin1String("web"))
        return fail(error, QStringLiteral("unsupported project type '%1'").arg(project.type));

    const QJsonValue fileValue = root.value(QLatin1String("file"));
    const QString    entry     = fileValue.toString();
    if (! fileValue.isString() || entry.isEmpty())
        return fail(error, QStringLiteral("project.json has no file entry"));
    if (QDir::isAbsolutePath(entry) || entry.contains(QChar(0)))
        return fail(error, QStringLiteral("file entry must be a relative path inside the project"));

    const QString canonical = QFileInfo(QDir(project.rootDir).filePath(entry)).canonicalFilePath();
    if (canonical.isEmpty())
        return fail(error, QStringLiteral("entry file does not exist: %1").arg(entry));
    if (! isInside(project.rootDir, canonical))
        return fail(
            error,
            QStringLiteral("entry file resolves outside the project directory: %1").arg(entry));
    const QFileInfo entryInfo(canonical);
    if (! entryInfo.isFile() || ! entryInfo.isReadable())
        return fail(error, QStringLiteral("entry is not a readable file: %1").arg(entry));
    const QString suffix = entryInfo.suffix().toLower();
    if (suffix != QLatin1String("html") && suffix != QLatin1String("htm") &&
        suffix != QLatin1String("xhtml"))
        return fail(error,
                    QStringLiteral("entry of a web project must be an HTML file: %1").arg(entry));
    project.entryFile = canonical;
    project.entryUrl  = QUrl::fromLocalFile(canonical);

    const QJsonValue title = root.value(QLatin1String("title"));
    if (! title.isUndefined() && ! title.isString())
        return fail(error, QStringLiteral("title must be a string"));
    project.title = title.toString();

    const QJsonValue general = root.value(QLatin1String("general"));
    if (general.isUndefined()) return project;
    if (! general.isObject()) return fail(error, QStringLiteral("general must be an object"));
    const QJsonValue properties = general.toObject().value(QLatin1String("properties"));
    if (properties.isUndefined()) return project;
    if (! properties.isObject())
        return fail(error, QStringLiteral("general.properties must be an object"));
    project.properties = properties.toObject();
    if (project.properties.size() > k_maxProperties)
        return fail(error,
                    QStringLiteral("project defines more than %1 properties").arg(k_maxProperties));
    for (auto it = project.properties.constBegin(); it != project.properties.constEnd(); ++it) {
        if (! it.value().isObject())
            return fail(error, QStringLiteral("property '%1' must be an object").arg(it.key()));
    }
    return project;
}

std::optional<QJsonObject> validatePropertyValues(const QJsonObject& definitions,
                                                  const QJsonObject& values, QString* error) {
    if (values.size() > k_maxProperties) {
        *error = QStringLiteral("too many properties in one update");
        return std::nullopt;
    }
    QJsonObject result;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        const QString& key = it.key();
        if (! definitions.contains(key)) {
            *error = QStringLiteral("unknown property '%1'").arg(key);
            return std::nullopt;
        }
        QJsonValue value = it.value();
        if (value.isObject()) {
            const QJsonObject wrapped = value.toObject();
            if (wrapped.size() != 1 || ! wrapped.contains(QLatin1String("value"))) {
                *error =
                    QStringLiteral("property '%1': expected a value or {\"value\": ...}").arg(key);
                return std::nullopt;
            }
            value = wrapped.value(QLatin1String("value"));
        }
        QString reason;
        if (! valueMatches(definitions.value(key).toObject(), value, &reason)) {
            *error = QStringLiteral("property '%1': %2").arg(key, reason);
            return std::nullopt;
        }
        result.insert(key, value);
    }
    return result;
}

QJsonObject applyPropertyValues(QJsonObject* definitions, const QJsonObject& values) {
    QJsonObject changed;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        QJsonObject definition = definitions->value(it.key()).toObject();
        definition.insert(QStringLiteral("value"), it.value());
        definitions->insert(it.key(), definition);
        changed.insert(it.key(), definition);
    }
    return changed;
}

bool isInsideProject(const QString& rootDir, const QUrl& url) {
    if (! url.isLocalFile()) return false;
    const QString local     = url.toLocalFile();
    QString       canonical = QFileInfo(local).canonicalFilePath();
    if (canonical.isEmpty()) canonical = QDir::cleanPath(local);
    return isInside(rootDir, canonical);
}

} // namespace wehypr
