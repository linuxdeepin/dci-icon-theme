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
    QImage image(imageFile);
    if (image.isNull()) {
        qWarning() << "Ignore the null image file:" << imageFile;
        return false;
    }

    dciChecker(dci.mkdir(targetDir + "/2"));
    dciChecker(dci.mkdir(targetDir + "/3"));
    dciChecker(dci.writeFile(targetDir + "/2/1.webp", webpImageData(image.scaledToWidth(512, Qt::SmoothTransformation), 100)));
    dciChecker(dci.writeFile(targetDir + "/3/1.webp", webpImageData(image.scaledToWidth(768, Qt::SmoothTransformation), 90)));
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

    QCoreApplication a(argc, argv);
    a.setApplicationName("dci-icon-theme");
    a.setApplicationVersion("0.0.1");

    QCommandLineParser cp;
    cp.addOptions({fileFilter, outputDirectory, symlinkMap});
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

    if (!cp.isSet(fileFilter)) {
        qWarning() << "Not give -m argument";
        cp.showHelp(-3);
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
    }

    QMultiHash<QString, QString> symlinksMap;
    if (cp.isSet(symlinkMap)) {
        symlinksMap = parseIconFileSymlinkMap(cp.value(symlinkMap));
    }

    const QStringList nameFilter = cp.values(fileFilter);
    const auto sourceDirectory = cp.positionalArguments();
    for (const auto &sd : qAsConst(sourceDirectory)) {
        QDir sourceDir(sd);
        if (!sourceDir.exists()) {
            qWarning() << "Ignore the non-exists directory:" << sourceDir;
            continue;
        }

        QDirIterator di(sourceDir.absolutePath(), nameFilter,
                        QDir::NoDotAndDotDot | QDir::Files | QDir::NoSymLinks,
                        QDirIterator::Subdirectories);
        while (di.hasNext()) {
            di.next();
            QFileInfo file = di.fileInfo();
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

            QFileInfo darkIcon(file.dir().absoluteFilePath("dark/" + file.fileName()));
            if (darkIcon.exists()) {
                dciChecker(dciFile.mkdir("/256/normal.dark"));
                writeImage(dciFile, darkIcon.filePath(), "/256/normal.dark");
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
