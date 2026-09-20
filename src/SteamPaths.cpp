#include "SteamPaths.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

namespace steam
{

namespace
{

const QString k_appId = QStringLiteral("431960");

// ── KeyValues (.vdf/.acf) ──────────────────────────────────────────────────────
// Steam writes libraryfolders.vdf and appmanifest_*.acf in the same text
// format, and nothing in this repository or its dependencies can read it.

struct KvNode {
    bool                          isObject { false };
    QString                       value;
    QList<QPair<QString, KvNode>> children;

    // Keys are compared case-insensitively and duplicates are last-wins, which
    // is how Steam itself reads them.
    const KvNode* child(const QString& key) const {
        for (auto it = children.crbegin(); it != children.crend(); ++it) {
            if (it->first.compare(key, Qt::CaseInsensitive) == 0) return &it->second;
        }
        return nullptr;
    }
};

class KvLexer {
public:
    explicit KvLexer(const QString& text): m_text(text) {}

    // Returns false at end of input. Punctuation ('{', '}') comes back as a
    // one-character token with quoted == false.
    bool next(QString& token, bool& quoted) {
        skipGap();
        if (m_pos >= m_text.size()) return false;

        const QChar c = m_text.at(m_pos);
        quoted        = false;

        if (c == u'{' || c == u'}') {
            token = QString(c);
            ++m_pos;
            return true;
        }
        if (c == u'"') {
            ++m_pos;
            token  = readQuoted();
            quoted = true;
            return true;
        }
        token = readBare();
        return ! token.isEmpty();
    }

private:
    void skipGap() {
        while (m_pos < m_text.size()) {
            const QChar c = m_text.at(m_pos);
            if (c.isSpace()) {
                ++m_pos;
            } else if (c == u'/' && m_pos + 1 < m_text.size() && m_text.at(m_pos + 1) == u'/') {
                while (m_pos < m_text.size() && m_text.at(m_pos) != u'\n') ++m_pos;
            } else {
                return;
            }
        }
    }

    QString readQuoted() {
        QString out;
        while (m_pos < m_text.size()) {
            const QChar c = m_text.at(m_pos++);
            if (c == u'"') break;
            if (c != u'\\' || m_pos >= m_text.size()) {
                out.append(c);
                continue;
            }
            const QChar esc = m_text.at(m_pos++);
            switch (esc.unicode()) {
            case 'n': out.append(u'\n'); break;
            case 't': out.append(u'\t'); break;
            default: out.append(esc); break;
            }
        }
        return out;
    }

    QString readBare() {
        const int start = m_pos;
        while (m_pos < m_text.size()) {
            const QChar c = m_text.at(m_pos);
            if (c.isSpace() || c == u'{' || c == u'}' || c == u'"') break;
            ++m_pos;
        }
        return m_text.mid(start, m_pos - start);
    }

    const QString& m_text;
    int            m_pos { 0 };
};

// Parses key/value pairs until '}' or end of input. Depth is bounded so a
// malformed file cannot recurse without limit.
bool parseBody(KvLexer& lexer, KvNode& out, int depth) {
    if (depth > 32) return false;
    out.isObject = true;

    QString token;
    bool    quoted = false;
    while (lexer.next(token, quoted)) {
        if (! quoted && token == QStringLiteral("}")) return true;
        if (! quoted && token == QStringLiteral("{")) return false;

        const QString key = token;
        if (! lexer.next(token, quoted)) return false;

        if (! quoted && token == QStringLiteral("{")) {
            KvNode nested;
            if (! parseBody(lexer, nested, depth + 1)) return false;
            out.children.append({ key, nested });
            continue;
        }
        if (! quoted && token == QStringLiteral("}")) return false;

        // A trailing conditional such as [$WIN32] selects the platform the
        // pair applies to; the value has already been read, so drop it.
        KvNode leaf;
        leaf.value = token;
        out.children.append({ key, leaf });
        if (! quoted && token.startsWith(u'[')) out.children.removeLast();
    }
    return true;
}

bool readKv(const QString& path, KvNode& out) {
    QFile file(path);
    if (! file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    // Steam caps these files well below this; the limit only guards against
    // being pointed at something that is not a KeyValues file.
    if (file.size() > 8 * 1024 * 1024) return false;

    QString text = QString::fromUtf8(file.readAll());
    if (! text.isEmpty() && text.at(0) == QChar(0xFEFF)) text.remove(0, 1);

    KvLexer lexer(text);
    return parseBody(lexer, out, 0);
}

void appendUnique(QStringList& list, QSet<QString>& seen, const QString& path) {
    if (path.isEmpty()) return;
    const QString key = canonicalPath(path);
    if (seen.contains(key)) return;
    seen.insert(key);
    list.append(key);
}

// One path segment, exact spelling first. The directory listing only happens
// when the exact name is missing, so the common case costs a single stat.
QString resolveSegment(const QString& base, const QString& segment) {
    const QString exact = base + u'/' + segment;
    if (QFileInfo::exists(exact)) return exact;

    const QStringList entries = QDir(base).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    for (const QString& entry : entries) {
        if (entry.compare(segment, Qt::CaseInsensitive) == 0) return base + u'/' + entry;
    }
    return {};
}

// The directory name of app 431960 inside steamapps/common. Only the manifest
// knows its exact spelling, so it is never hardcoded.
QString manifestInstallDir(const QString& steamappsDir) {
    const QString manifest = resolveSegment(
        steamappsDir, QStringLiteral("appmanifest_") + k_appId + QStringLiteral(".acf"));
    if (manifest.isEmpty()) return {};

    KvNode root;
    if (! readKv(manifest, root)) return {};
    const KvNode* state = root.child(QStringLiteral("AppState"));
    if (state == nullptr) return {};
    const KvNode* installDir = state->child(QStringLiteral("installdir"));
    if (installDir == nullptr || installDir->value.isEmpty()) return {};
    return installDir->value;
}

// Library roots listed in one libraryfolders.vdf. Handles both shapes: the
// current one, where each numeric child is an object with a "path" key, and
// the pre-2021 one, where it maps straight to a path string.
void readLibraryFolders(const QString& vdfPath, QStringList& out, QSet<QString>& seen) {
    KvNode root;
    if (! readKv(vdfPath, root)) return;

    const KvNode* folders = root.child(QStringLiteral("libraryfolders"));
    if (folders == nullptr) return;

    for (const auto& [key, node] : folders->children) {
        bool numeric = false;
        key.toInt(&numeric);
        // Skips the bookkeeping keys the legacy shape interleaves
        // (TimeNextStatsReport, ContentStatsID).
        if (! numeric) continue;

        if (! node.isObject) {
            appendUnique(out, seen, node.value);
            continue;
        }
        const KvNode* path = node.child(QStringLiteral("path"));
        if (path != nullptr) appendUnique(out, seen, path->value);
    }
}

} // namespace

QString canonicalPath(const QString& path) {
    if (path.isEmpty()) return {};
    const QString resolved = QFileInfo(path).canonicalFilePath();
    return resolved.isEmpty() ? QDir::cleanPath(path) : resolved;
}

QStringList steamRoots() {
    const QString home = QDir::homePath();
    QString       data = qEnvironmentVariable("XDG_DATA_HOME");
    if (data.isEmpty()) data = home + QStringLiteral("/.local/share");

    // Probed in this order so the symlinks a native install keeps in ~/.steam
    // resolve first; duplicates collapse on the canonical path anyway.
    const QStringList candidates {
        home + QStringLiteral("/.steam/steam"),
        home + QStringLiteral("/.steam/root"),
        data + QStringLiteral("/Steam"),
        home + QStringLiteral("/.steam/debian-installation"),
        home + QStringLiteral("/.var/app/com.valvesoftware.Steam/.local/share/Steam"),
        home + QStringLiteral("/.var/app/com.valvesoftware.Steam/data/Steam"),
        home + QStringLiteral("/snap/steam/common/.local/share/Steam"),
        home + QStringLiteral("/Steam"),
    };

    QStringList   roots;
    QSet<QString> seen;
    for (const QString& candidate : candidates) {
        if (! QFileInfo(candidate).isDir()) continue;
        appendUnique(roots, seen, candidate);
    }
    return roots;
}

QStringList libraryRoots(const QString& steamRoot) {
    QStringList   roots;
    QSet<QString> seen;
    appendUnique(roots, seen, steamRoot);

    const QString steamapps = resolveSegment(steamRoot, QStringLiteral("steamapps"));
    if (! steamapps.isEmpty()) {
        const QString vdf = resolveSegment(steamapps, QStringLiteral("libraryfolders.vdf"));
        if (! vdf.isEmpty()) readLibraryFolders(vdf, roots, seen);
    }
    // Modern clients keep a second copy here; the two can disagree while one of
    // them is being rewritten, so both are read and merged.
    const QString configDir = resolveSegment(steamRoot, QStringLiteral("config"));
    if (! configDir.isEmpty()) {
        const QString vdf = resolveSegment(configDir, QStringLiteral("libraryfolders.vdf"));
        if (! vdf.isEmpty()) readLibraryFolders(vdf, roots, seen);
    }
    return roots;
}

QString resolvePath(const QString& base, const QStringList& segments) {
    if (base.isEmpty() || ! QFileInfo(base).isDir()) return {};

    QString current = base;
    for (const QString& segment : segments) {
        current = resolveSegment(current, segment);
        if (current.isEmpty()) return {};
    }
    return current;
}

QList<Library> detectLibrariesIn(const QStringList& steamRootList) {
    QList<Library> libraries;
    QSet<QString>  seenLibrary;

    for (const QString& steamRoot : steamRootList) {
        for (const QString& libraryRoot : libraryRoots(steamRoot)) {
            const QString key = canonicalPath(libraryRoot);
            if (seenLibrary.contains(key)) continue;
            seenLibrary.insert(key);

            const QString steamapps = resolveSegment(key, QStringLiteral("steamapps"));
            if (steamapps.isEmpty()) continue;

            Library library;
            library.root        = key;
            library.workshopDir = resolvePath(
                steamapps, { QStringLiteral("workshop"), QStringLiteral("content"), k_appId });

            const QString installDir = manifestInstallDir(steamapps);
            if (! installDir.isEmpty()) {
                // The manifest can outlive the files after a failed uninstall,
                // and assets/ is what every renderer actually needs.
                const QString candidate =
                    resolvePath(steamapps, { QStringLiteral("common"), installDir });
                if (! candidate.isEmpty() && ! assetsDir(candidate).isEmpty())
                    library.installDir = candidate;
            }
            if (library.installDir.isEmpty()) {
                // No usable manifest: fall back to the conventional directory.
                const QString candidate = resolvePath(
                    steamapps, { QStringLiteral("common"), QStringLiteral("wallpaper_engine") });
                if (! candidate.isEmpty() && ! assetsDir(candidate).isEmpty())
                    library.installDir = candidate;
            }

            if (library.workshopDir.isEmpty() && library.installDir.isEmpty()) continue;
            libraries.append(library);
        }
    }

    // The library that carries the install owns assets/ and config.json, which
    // callers needing a single answer take from the front of the list.
    std::stable_partition(libraries.begin(), libraries.end(), [](const Library& library) {
        return ! library.installDir.isEmpty();
    });
    return libraries;
}

QList<Library> detectLibraries() {
    // ponytail: stats every library root directly. A library on an unreachable
    // network mount blocks until the kernel gives up, and the KDE plugin calls
    // this on the GUI thread. Move to a worker thread if that shows up.
    return detectLibrariesIn(steamRoots());
}

QString assetsDir(const QString& installDir) {
    return resolvePath(installDir, { QStringLiteral("assets") });
}

QString globalConfigPath(const QString& installDir) {
    return resolvePath(installDir, { QStringLiteral("config.json") });
}

QString defaultProjectsDir(const QString& installDir) {
    return resolvePath(installDir,
                       { QStringLiteral("projects"), QStringLiteral("defaultprojects") });
}

QString myProjectsDir(const QString& installDir) {
    return resolvePath(installDir, { QStringLiteral("projects"), QStringLiteral("myprojects") });
}

} // namespace steam
