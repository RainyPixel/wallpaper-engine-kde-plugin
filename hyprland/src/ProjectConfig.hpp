#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

namespace wehypr
{

struct Project {
    QString     projectFile;
    QString     rootDir;
    QString     entryFile;
    QUrl        entryUrl;
    QString     type;
    QString     title;
    QJsonObject properties;
};

// Loads a Wallpaper Engine project directory or project.json. Only web projects whose entry
// is an HTML file inside the project directory are accepted.
std::optional<Project> loadProject(const QString& path, QString* error);

// Checks values against the property definitions of the project. Values can be given as
// {"name": value} or {"name": {"value": value}}. Returns the plain values on success.
std::optional<QJsonObject> validatePropertyValues(const QJsonObject& definitions,
                                                  const QJsonObject& values, QString* error);

// Writes validated values into definitions and returns the updated definitions of the
// changed properties, in the shape passed to wallpaperPropertyListener.applyUserProperties.
QJsonObject applyPropertyValues(QJsonObject* definitions, const QJsonObject& values);

// True for main-frame URLs the page may navigate to: local files inside rootDir.
bool isInsideProject(const QString& rootDir, const QUrl& url);

} // namespace wehypr
