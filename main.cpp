// SPDX-FileCopyrightText: 2022 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include <QCoreApplication>
#include <QImageReader>
#include <QCommandLineParser>
#include <QDirIterator>
#include <QBuffer>
#include <QDebug>

#include <DDciFile>

DCORE_USE_NAMESPACE

static inline void dciChecker(bool result) {
    if (!result) {
        qWarning() << "Failed on writing dci file";
        exit(-6);
    }
}

static inline QByteArray webpImageData(const QImage &image, int quality) {
    QByteArray data;
    QBuffer buffer(&data);
    bool ok = buffer.open(QIODevice::WriteOnly);
    Q_ASSERT(ok);
    dciChecker(image.save(&buffer, "webp", quality));
    return data;
}

static bool writeImage(DDciFile &dci, const QString &imageFile, const QString &targetDir)
{
    QImageReader image(imageFile);
    if (!image.canRead()) {
        qWarning() << "Ignore the null image file:" << imageFile;
        return false;
    }

    dciChecker(dci.mkdir(targetDir + "/2"));
    dciChecker(dci.mkdir(targetDir + "/3"));

    if (image.supportsOption(QImageIOHandler::ScaledSize)) {
        image.setScaledSize(QSize(512, 512));
    }

    dciChecker(dci.writeFile(targetDir + "/2/1.webp", webpImageData(image.read().scaledToWidth(512, Qt::SmoothTransformation), 100)));

    if (image.supportsOption(QImageIOHandler::ScaledSize)) {
        image.setScaledSize(QSize(768, 768));
    }

    dciChecker(dci.writeFile(targetDir + "/3/1.webp", webpImageData(image.read().scaledToWidth(768, Qt::SmoothTransformation), 90)));
    return true;
}

static bool recursionLink(DDciFile &dci, const QString &fromDir, const QString &targetDir)
{
    for (const auto &i : dci.list(fromDir, true)) {
        const QString file(fromDir + "/" + i);
        const QString targetFile(targetDir + "/" + i);
        if (dci.type(file) == DDciFile::Directory) {
            if (!dci.mkdir(targetFile))
                return false;
            if (!recursionLink(dci, file, targetFile))
                return false;
        } else {
            if (!dci.link(file, targetFile))
                return false;
        }
    }

    return true;
}

static QByteArray readNextSection(QIODevice *io) {
    QByteArray section;
    char ch;

    while (io->getChar(&ch)) {
        if (ch == '"') {
            continue;
        } else if (ch == ',') {
            break;
        }

        section.append(ch);
    }

    return section.trimmed();
}

QMultiHash<QString, QString> parseIconFileSymlinkMap(const QString &csvFile) {
    QFile file(csvFile);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed on open symlink map file:" << csvFile;
        exit(-7);
    }

    QMultiHash<QString, QString> map;
    while (!file.atEnd()) {
        QByteArray key = readNextSection(&file);
        QByteArray value = readNextSection(&file);
        for (const auto &i : value.split('\n')) {
            map.insert(QString::fromLocal8Bit(key), QString::fromLocal8Bit(i));
        }

        char ch = 0;
        while (file.getChar(&ch) && ch != '\n');
    }

    qInfo() << "Got symlinks:" << map.size();

    return map;
}

int main(int argc, char *argv[])
{
    QCommandLineOption fileFilter({"m", "match"}, "Give wirdcard rules on search icon files, "
                                                  "Each eligible icon will be packaged to a dci file, "
                                                  "If the icon have the dark mode, it needs to store "
                                                  "the dark icon file at \"dark/\" directory relative "
                                                  "to current icon file, and the file name should be "
                                                  "consistent.", "wirdcard palette");
    QCommandLineOption outputDirectory({"o", "output"}, "Save the *.dci files to the given directory.",
                                       "directory");
    QCommandLineOption symlinkMap({"s", "symlink"}, "Give a csv file to create symlinks for the output icon file.",
                                       "csv file");
    QCommandLineOption fixDarkTheme("fix-dark-theme", "Create symlinks from light theme for dark theme files.");

    QCoreApplication a(argc, argv);
    a.setApplicationName("dci-icon-theme");
    a.setApplicationVersion("0.0.1");

    QCommandLineParser cp;
    cp.addOptions({fileFilter, outputDirectory, symlinkMap, fixDarkTheme});
    cp.addPositionalArgument("source", "Search the given directory and it's subdirectories, "
                                       "get the files conform to rules of --match.",
                             "~/dci-png-icons");
    cp.addHelpOption();
    cp.addVersionOption();
    cp.process(a);

    if (a.arguments().size() == 1)
        cp.showHelp(-1);

    if (cp.positionalArguments().isEmpty()) {
        qWarning() << "Not give a source directory.";
        cp.showHelp(-2);
    }

    if (!cp.isSet(outputDirectory)) {
        qWarning() << "Not give -o argument";
        cp.showHelp(-4);
    }

    QDir outputDir(cp.value(outputDirectory));
    if (!outputDir.exists()) {
        if (!QDir::current().mkpath(outputDir.absolutePath())) {
            qWarning() << "Can't create the" << outputDir.absolutePath() << "directory";
            cp.showHelp(-5);
        }
    } else {
        qFatal("The output directory have been exists.");
    }

    QMultiHash<QString, QString> symlinksMap;
    if (cp.isSet(symlinkMap)) {
        symlinksMap = parseIconFileSymlinkMap(cp.value(symlinkMap));
    }

    const QStringList nameFilter = cp.isSet(fileFilter) ? cp.values(fileFilter) : QStringList();
    const auto sourceDirectory = cp.positionalArguments();
    for (const auto &sd : qAsConst(sourceDirectory)) {
        QDir sourceDir(sd);
        if (!sourceDir.exists()) {
            qWarning() << "Ignore the non-exists directory:" << sourceDir;
            continue;
        }

        QDirIterator di(sourceDir.absolutePath(), nameFilter,
                        QDir::NoDotAndDotDot | QDir::Files,
                        QDirIterator::Subdirectories);
        while (di.hasNext()) {
            di.next();
            QFileInfo file = di.fileInfo();

            if (cp.isSet(fixDarkTheme)) {
                const QString &newFile = outputDir.absoluteFilePath(file.fileName());

                if (file.isSymLink()) {
                    if (!QFile::copy(file.absoluteFilePath(), newFile)) {
                        qWarning() << "Failed on copy" << file.absoluteFilePath() << "to" << newFile;
                        return -6;
                    }
                    continue;
                }

                DDciFile dciFile(file.absoluteFilePath());
                if (!dciFile.isValid()) {
                    qWarning() << "Skip invalid dci file:" << file.absoluteFilePath();
                    continue;
                }

                for (const auto &i : dciFile.list("/")) {
                    if (dciFile.type(i) != DDciFile::Directory)
                        continue;

                    for (const auto &j : dciFile.list(i)) {
                        if (dciFile.type(j) != DDciFile::Directory || !j.endsWith(".light"))
                            continue;

                        const QString darkDir(j.left(j.size() - 5) + "dark");
                        Q_ASSERT(darkDir.endsWith(".dark"));
                        if (!dciFile.exists(darkDir)) {
                            dciChecker(dciFile.mkdir(darkDir));
                            dciChecker(recursionLink(dciFile, j, darkDir));
                        }
                    }
                }

                dciChecker(dciFile.writeToFile(newFile));

                continue;
            }

            if (file.isSymLink())
                continue;

            if (file.path().endsWith(QStringLiteral("/dark"))) {
                qInfo() << "Ignore the dark icon file:"  << file;
                continue;
            }

            const QString dciFilePath(outputDir.absoluteFilePath(file.completeBaseName()) + ".dci");
            if (QFile::exists(dciFilePath)) {
                qWarning() << "Skip exists dci file:" << dciFilePath;
                continue;
            }
            DDciFile dciFile;

            qInfo() << "Wrting to dci file:" << dciFilePath;

            dciChecker(dciFile.mkdir("/256"));
            dciChecker(dciFile.mkdir("/256/normal.light"));
            if (!writeImage(dciFile, file.filePath(), "/256/normal.light"))
                continue;

            dciChecker(dciFile.mkdir("/256/normal.dark"));
            QFileInfo darkIcon(file.dir().absoluteFilePath("dark/" + file.fileName()));
            if (darkIcon.exists()) {
                writeImage(dciFile, darkIcon.filePath(), "/256/normal.dark");
            } else {
                dciChecker(recursionLink(dciFile, "/256/normal.light", "/256/normal.dark"));
            }

            dciChecker(dciFile.writeToFile(dciFilePath));

            if (symlinksMap.contains(file.completeBaseName())) {
                const QString symlinkKey = QFileInfo(dciFilePath).fileName();
                for (const auto &symTarget : symlinksMap.values(file.completeBaseName())) {
                    const QString newSymlink = outputDir.absoluteFilePath(symTarget + ".dci");
                    qInfo() << "Create symlink from" << symlinkKey << "to" << newSymlink;
                    if (!QFile::link(symlinkKey, newSymlink)) {
                        qWarning() << "Failed on create symlink from" << symlinkKey << "to" << newSymlink;
                    }
                }
            }
        }
    }

    return 0;
}
